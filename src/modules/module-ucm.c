/**********************************************************************
 * Copyright (c) 2015-2016 LG Electronics, Inc.
 * All rights reserved.
 *
 * module-ucm.c - Applies mixer controls based on type of streams and
 * routing device
 **********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>

#include "module-ucm-symdef.h"
#if HAVE_ACDB
#include "acdbloader/acdb-loader.h"
#include "acdbmapper/acdb-id-mapper.h"
#endif
#include <alsa-intf/msm8960_use_cases.h>

#define MIXER_DEVICE "/dev/snd/controlC0"
#define MAX_VOLUME 8192
#define FACTOR MAX_VOLUME/100.0
#define MIXER_COMPRESS_PLAYBACK_VOLUME "Compress Playback 9 Volume"

#define MIXER_VOICE_RX_GAIN "Voice Rx Gain"
#define MIXER_VOICE_TX_MUTE "Voice Tx Mute"
#define MIXER_VOICE_RX_MUTE "Voice Rx Device Mute"
#define MIXER_SPEAKER_RX_GAIN "RX3 Digital Volume"
#define MIXER_MIC_TX_GAIN "DEC1 Volume"
#define ALL_SESSION_VSID 0xFFFFFFFF
#define DEFAULT_VOLUME_RAMP_DURATION_MS 20
#define MAX_VOL_INDEX 10
#define MIN_VOL_INDEX 0
#define PERCENT_TO_INDEX(val, min, max) \
        (max -((val) * ((max) - (min)) * 0.01))

#define SND_USE_CASE_DEV_QVOICE_VR_TX    "Qvoice DMIC Endfire"
#if HAVE_ACDB
#define SND_USE_CASE_DEV_LB_SUB_MIC1    "Loopback Submic1"
#define SND_USE_CASE_DEV_LB_SUB_MIC2    "Loopback Submic2"
#define DEVICE_LB_SUB_MIC1_ACDB_ID 71
#define DEVICE_LB_SUB_MIC2_ACDB_ID 72
#define DEVICE_QVOICE_VR_TX_ACDB_ID    60
#define DEVICE_VOICE_MEMO_ACDB_ID 63 /* Voice Memo */
#define DEVICE_PTT_SPEAKER_TX_ACDB_ID 66 /* LGU PTT */
#define DEVICE_BT_SCO_MIC_NREC_TX_ACDB_ID 50
#define DEVICE_BT_SCO_MIC_WB_NREC_TX_ACDB_ID 51
#define DEVICE_BT_SCO_WB_TX_ACDB_ID 38
#define DEVICE_BT_SCO_WB_RX_ACDB_ID 39
#endif
#define SCENARIO_BACK_SPEAKER "back_speaker"
#define SCENARIO_BT_SCO "bluetooth_sco"
#define PHONE_SCENARIO_BT_SCO "phone_bluetooth_sco"
#define SND_USE_CASE_BT_SCO_WB_NREC_TX "BT SCO WB NREC Tx"
#define SND_USE_CASE_BT_SCO_NREC_TX  "BT SCO NREC Tx"
#define SND_USE_CASE_DEV_HANDSETVM_TX "HandsetVM Tx"
#define DEFAULT_SAMPLE_RATE 8000
#define VOICE_CALL_HW "hw:0,2"
#define VOIP_CALL_HW "hw:0,3"
#define HFP_CALL_HW "hw:0,15"
#define LOOPBACK_DELAY_MSEC 60

PA_MODULE_AUTHOR("Sukesh Adiga");
PA_MODULE_DESCRIPTION(_("Module to Apply mixer controls based on type of streams and routing device"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "card_name=<name of card> ");

typedef enum ucm_stream_status {
    UCM_PLAYBACK,
    UCM_RECORDING,
    UCM_PLAYBACK_RECORDING,
    UCM_COMPRESS_PLAYBACK,
    UCM_COMPRESS_PLAYBACK_RECORDING,
    UCM_NONE
} ucm_stream_status_t;

typedef enum status {
    NO_ERROR = 0,
    NO_INIT
}enStatus;

typedef enum EBTDeviceType {
    eBTDevice_NarrowBand = 1,
    eBTDevice_Wideband = 2
}BTDeviceType;

typedef struct alsa_handle_t {

    uint32_t            devices;
    char                useCase[50];
    struct pcm *        handle;
    snd_pcm_format_t    format;
    uint32_t            channels;
    uint32_t            sampleRate;
    uint32_t            latency;         // Delay in usec
    uint32_t            bufferSize;      // Size of sample buffer
    uint32_t            periodSize;
    struct pcm *        rxHandle;
    struct pcm *        txHandle;
    struct pcm *        lterxHandle;
    struct pcm *        ltetxHandle;
    snd_use_case_mgr_t  *ucMgr;
}alsaHandle;

typedef enum audio_loopback_option{
    AUDIO_LOOPBACK_OFF=0,
    AUDIO_NORMAL_LOOPBACK=1,
    AUDIO_TESTMODE_LOOPBACK=2,
    AUDIO_DELAY_LOOPBACK=3,
    AUDIO_DELAY_LOOPBACK_BT=4,
    AUDIO_LOOPBACK_OPTION_CNT,
    AUDIO_LOOPBACK_OPTION_MAX = AUDIO_LOOPBACK_OPTION_CNT - 1,
} audio_loopback_option_t;

struct audio_device {
    bool mIsLoopbackMode;
    bool mKillLoopbackThread;
    pthread_mutex_t loopback_lock;
    pthread_t loopback_thread;
    audio_loopback_option_t loopback_option;
};

struct userdata {

    pa_core *core;
    pa_module *module;

    pa_sink_state_t sink_state;
    pa_sink_state_t tinycompress_sink_state;
    pa_source_state_t source_state;

    pa_hook_slot *sink_state_changed_slot;
    pa_hook_slot *source_state_changed_slot;

    pa_hook_slot *ucm_sink_input_fixate_hook_slot;
    pa_hook_slot *ucm_sink_input_unlink_hook_slot;

    pa_hook_slot *ucm_source_output_fixate_hook_slot;
    pa_hook_slot *ucm_source_output_new_hook_slot;

    pa_hook_slot *ucm_sink_input_state_changed_hook_slot;
    pa_hook_slot *ucm_source_output_state_changed_hook_slot;
    pa_hook_slot *ucm_sink_input_move_finish_hook_slot;

    snd_use_case_mgr_t *ucm_mgr;
    ucm_stream_status_t stream_status;

    alsaHandle *handle;
    bool onActiveCall;
    bool recording;
    bool NREC_On;
    int BTHfpConnected;
    BTDeviceType BTdeviceType;

    struct audio_device *adev;
    char *loopback_device;

    struct mixer *mixer_handle;
    bool voice_RX_muted;
};

/* will require userdata handle to implement calls from module-palm-policy*/
struct userdata *m_u;

static const char* const valid_modargs[] = {
    "card_name",
    NULL
};

static void ucm_update_mixer_control(struct userdata *u);
static void ucm_disable_verb(struct userdata *u);
static void ucm_disable_all_mods_and_devs(struct userdata *u);
static void set_mixers_for_recording(struct userdata *u,char *TxDevice);
static void  ucm_disable_mixers_for_recording(struct userdata *u);
static pa_hook_result_t sink_state_changed_cb (pa_core *c, pa_object *o, struct userdata *u);
static pa_hook_result_t source_state_changed_cb (pa_core *c, pa_object *o, struct userdata *u);
static pa_hook_result_t ucm_sink_input_state_changed_callback(pa_core *c, pa_sink_input *data, struct userdata *u);
static pa_hook_result_t ucm_source_output_state_changed_callback(pa_core * c, pa_source_output * data, struct userdata * u);

static pa_hook_result_t ucm_sink_input_move_finish_callback(pa_core *core, pa_sink_input *i, struct userdata *u);

static bool ucm_device_exists(snd_use_case_mgr_t *uc_mgr, const char *dev_name);
static bool ucm_modifier_exists(snd_use_case_mgr_t *uc_mgr, const char *mod_name);
static void ucm_update_stream_state(struct userdata *u);

/* volume settings for HFP-HF voice call*/
void update_hf_call_voiceOrMic_volume(int volume, bool voiceNotMic)
{
    struct mixer_ctl *mixer_control = NULL;

    if (voiceNotMic)
        mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_SPEAKER_RX_GAIN, 0);
    else
        mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_MIC_TX_GAIN, 0);

    if (!mixer_control)
        pa_log_error("ERROR :No mixer control found : %s volume control will not be applied\n", voiceNotMic ? "voice" : "Mic");
    else {
        if (mixer_ctl_set(mixer_control, volume) == -EINVAL)
            pa_log_error("ERROR : Unable to set %s Volume in %s\n", voiceNotMic ? "voice" : "Mic", __FUNCTION__);
        else
            pa_log_debug("%s : setting %s volume = %d\n", __FUNCTION__, voiceNotMic ? "voice" : "Mic", volume);
    }
}

/* mute/unmute for call voice*/
void update_call_voice_mute(bool mute)
{
    struct mixer_ctl *mixer_control = NULL;
    uint32_t set_values[3] = {0, ALL_SESSION_VSID, DEFAULT_VOLUME_RAMP_DURATION_MS};
    mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_VOICE_RX_MUTE, 0);
    if (!mixer_control)
        pa_log_error("ERROR :No mixer control found : volume control will not be applied\n");
    else {
        set_values[0] = mute ? 1 : 0;
        if (mixer_ctl_setArray(mixer_control, set_values, 3) == -EINVAL)
            pa_log_error("ERROR : Unable to mute / unmute voice in %s\n", __FUNCTION__);
        else
           pa_log_debug("%s : setting voice %s\n", __FUNCTION__, mute ? "muted" : "unmuted");
    }
}


/* volume settings for incoming call voice */
void update_call_voice_volume (int volume)
{
    if (!volume) {
        update_call_voice_mute(true);
        m_u->voice_RX_muted = true;
    }
    else {
        if (m_u->voice_RX_muted) {
            update_call_voice_mute(false);
            m_u->voice_RX_muted = false;
        }
        struct mixer_ctl *mixer_control = NULL;
        uint32_t set_values[3] = {0, ALL_SESSION_VSID, DEFAULT_VOLUME_RAMP_DURATION_MS};

        mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_VOICE_RX_GAIN, 0);
        if (!mixer_control)
            pa_log_error("ERROR :No mixer control found : volume control will not be applied\n");
        else {
            set_values[0] = PERCENT_TO_INDEX(volume, MIN_VOL_INDEX, MAX_VOL_INDEX);
            if (mixer_ctl_setArray(mixer_control, set_values, 3) == -EINVAL)
                pa_log_error("ERROR : Unable to set Volume in %s\n", __FUNCTION__);
            else
                pa_log_debug("%s : setting voice volume = %d\n", __FUNCTION__, volume);
        }
    }
}

/* MIC mute mute/unmute */
void update_phoneMIC_volume(int volume)
{
    struct mixer_ctl *mixer_control = NULL;
    uint32_t set_values[3] = {0, ALL_SESSION_VSID, DEFAULT_VOLUME_RAMP_DURATION_MS};

    mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_VOICE_TX_MUTE, 0);
    if (!mixer_control)
        pa_log_error("ERROR :No mixer control found : volume control will not be applied\n");
    else {
        set_values[0] = (!volume) ? 1: 0;
        if (mixer_ctl_setArray(mixer_control, set_values, 3) == -EINVAL)
            pa_log_error("ERROR : Unable to mute / unmute mic in %s\n", __FUNCTION__);
        else
           pa_log_debug("%s : setting MIC volume = %d\n", __FUNCTION__, volume);
    }
}

/* Volume settings for passthrough streams */
void update_volume(int volume) {
    struct mixer_ctl *mixer_control = NULL;

    mixer_control = mixer_get_control(m_u->mixer_handle, MIXER_COMPRESS_PLAYBACK_VOLUME, 0);
    if (mixer_control) {
        int vol[2];
        vol[0] = vol[1] = (int)(volume * FACTOR);
        pa_assert(mixer_ctl_setArray(mixer_control, vol, sizeof(vol)/sizeof(vol[0]))== 0);
        pa_log_debug("Set compress volume to %d", volume);
    }
    else
        pa_log("Unable to get the mixer control name : %s", MIXER_COMPRESS_PLAYBACK_VOLUME);

}

static void ucm_update_mixer_control(struct userdata *u) {

    pa_assert(u);
    pa_assert(u->ucm_mgr);

    pa_log_debug("u->stream_status %d ", u->stream_status);

    switch (u->stream_status) {
        case UCM_PLAYBACK:
            snd_use_case_set(u->ucm_mgr, "_dismod", SND_USE_CASE_MOD_PLAY_TUNNEL);
            snd_use_case_set(u->ucm_mgr, "_dismod", SND_USE_CASE_MOD_CAPTURE_MUSIC);
            ucm_disable_mixers_for_recording(u);

            break;

        case UCM_RECORDING:
            snd_use_case_set(u->ucm_mgr, "_dismod", SND_USE_CASE_MOD_PLAY_TUNNEL);
            snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_SPEAKER);

            break;

        case UCM_PLAYBACK_RECORDING:
            snd_use_case_set(u->ucm_mgr, "_dismod", SND_USE_CASE_MOD_PLAY_TUNNEL);

            break;

        case UCM_COMPRESS_PLAYBACK:
            snd_use_case_set(u->ucm_mgr, "_dismod", SND_USE_CASE_MOD_CAPTURE_MUSIC);
            ucm_disable_mixers_for_recording(u);

            break;

        case UCM_COMPRESS_PLAYBACK_RECORDING:
            break;

        case UCM_NONE:
            /* disable all verbs, modifiers, devices */
            if(!u->onActiveCall && u->adev && !u->adev->mIsLoopbackMode){
                ucm_disable_all_mods_and_devs(u);
                ucm_disable_verb(u);
            }
            break;
    }

}

static void ucm_disable_verb(struct userdata *u) {

    const char *current_verb;
    pa_assert(u);
    pa_assert(u->ucm_mgr);

    snd_use_case_get(u->ucm_mgr, "_verb", &current_verb);
    pa_log_info("current_verb (%s)", current_verb);

    if (!pa_streq(current_verb, SND_USE_CASE_VERB_INACTIVE)) {
        pa_log_info("Setting verb to Inactive");
        snd_use_case_set(u->ucm_mgr, "_verb", SND_USE_CASE_VERB_INACTIVE);
    }

    if (current_verb)
        free((void *)current_verb);
}

static void ucm_disable_all_mods_and_devs(struct userdata *u) {

    int mods, devs, i;
    const char **mod_list = NULL;
    const char **dev_list = NULL;

    pa_assert(u);
    pa_assert(u->ucm_mgr);

    mods = snd_use_case_get_list(u->ucm_mgr, "_enamods", &mod_list);
    if (mods < 0)
        pa_log("Fail to get modifier list err = %d", mods);

    devs = snd_use_case_get_list(u->ucm_mgr, "_enadevs", &dev_list);
    if (devs < 0)
        pa_log("Fail to get device list err = %d", devs);

    if (mods) {
        for(i = 0; i < mods; i++)
            snd_use_case_set(u->ucm_mgr, "_dismod", mod_list[i]);

        snd_use_case_free_list(mod_list, mods);
    }

    if (devs) {
        for(i = 0; i < devs; i++)
            snd_use_case_set(u->ucm_mgr, "_disdev", dev_list[i]);

        snd_use_case_free_list(dev_list, devs);
    }

}

static void ucm_update_stream_state(struct userdata *u) {

    pa_assert(u);

    if (u->source_state == PA_SOURCE_SUSPENDED) {
        if (u->sink_state == PA_SINK_SUSPENDED && u->tinycompress_sink_state == PA_SINK_SUSPENDED)
            u->stream_status = UCM_NONE;
        else if (u->sink_state == PA_SINK_SUSPENDED && u->tinycompress_sink_state != PA_SINK_SUSPENDED)
            u->stream_status = UCM_COMPRESS_PLAYBACK;
        else if (u->sink_state != PA_SINK_SUSPENDED && u->tinycompress_sink_state == PA_SINK_SUSPENDED)
            u->stream_status = UCM_PLAYBACK;
        else if (u->sink_state != PA_SINK_SUSPENDED && u->tinycompress_sink_state != PA_SINK_SUSPENDED)
            u->stream_status = UCM_COMPRESS_PLAYBACK;

    } else if (u->source_state != PA_SOURCE_SUSPENDED) {
        if (u->sink_state == PA_SINK_SUSPENDED && u->tinycompress_sink_state == PA_SINK_SUSPENDED)
            u->stream_status = UCM_RECORDING;
        else if (u->sink_state == PA_SINK_SUSPENDED && u->tinycompress_sink_state != PA_SINK_SUSPENDED)
            u->stream_status = UCM_COMPRESS_PLAYBACK_RECORDING;
        else if (u->sink_state != PA_SINK_SUSPENDED && u->tinycompress_sink_state == PA_SINK_SUSPENDED)
            u->stream_status = UCM_PLAYBACK_RECORDING;
        else if (u->sink_state != PA_SINK_SUSPENDED && u->tinycompress_sink_state != PA_SINK_SUSPENDED)
            u->stream_status = UCM_COMPRESS_PLAYBACK_RECORDING;

    }
}

int setHardwareParams(alsaHandle *handle)
{
    struct snd_pcm_hw_params *params;

    params = pa_xmalloc0(sizeof(struct snd_pcm_hw_params));
    if (!params) {
        pa_log("%s : ERROR : no init error : ", __FUNCTION__);
        return NO_INIT;
    }

    strcpy(handle->useCase, SND_USE_CASE_VERB_VOICECALL);

    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 2048);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS, handle->channels *16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS, handle->channels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, handle->sampleRate);

    if (param_set_hw_refine(handle->handle, params)) {
       pa_log("%s : ERROR : param not refined : ", __FUNCTION__);
       return NO_INIT;
    }

    if (param_set_hw_params(handle->handle, params)) {
       pa_log("%s : ERROR : in hardware setting : ", __FUNCTION__);
       return NO_INIT;
    }

    param_dump(params);

    handle->handle->buffer_size = pcm_buffer_size(params);

    handle->handle->period_size = pcm_period_size(params);

    handle->handle->period_cnt = handle->handle->buffer_size/handle->handle->period_size;

    handle->handle->rate = DEFAULT_SAMPLE_RATE;

    handle->handle->channels = 1;

    handle->periodSize = handle->handle->period_size;

    handle->bufferSize = handle->handle->period_size;

    return NO_ERROR;
}

int setSoftwareParams(alsaHandle *handle)
{
    struct snd_pcm_sw_params* params;
    struct pcm* pcm = handle->handle;

    unsigned long periodSize = pcm->period_size;

    params = pa_xmalloc0(sizeof(struct snd_pcm_sw_params));
    if (!params) {
        pa_log("%s : ERROR : no init error : ", __FUNCTION__);
        return NO_INIT;
    }

    // Get the current software parameters
    params->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    params->period_step = 1;
    if (((!strncmp(handle->useCase,SND_USE_CASE_MOD_PLAY_VOIP,
                            strlen(SND_USE_CASE_MOD_PLAY_VOIP))) ||
        (!strncmp(handle->useCase,SND_USE_CASE_VERB_IP_VOICECALL,
                            strlen(SND_USE_CASE_VERB_IP_VOICECALL))))) {
          pa_log("setparam:  start & stop threshold for Voip \n\n");
          params->avail_min = handle->channels - 1 ? periodSize/4 : periodSize/2;
          params->start_threshold = periodSize/2;
          params->stop_threshold = INT_MAX;
     } else {
         params->avail_min = periodSize/2;
         params->start_threshold = handle->channels - 1 ? periodSize/2 : periodSize/4;
         params->stop_threshold = INT_MAX;
     }

    params->silence_threshold = 0;
    params->silence_size = 0;

    if (param_set_sw_params(handle->handle, params)) {
        pa_log("cannot set sw params");
        return NO_INIT;
    }
    return NO_ERROR;
}

enStatus ucm_open_pcm_device(const char *device, unsigned flags) {

    int err = 0;

    m_u->handle->handle = pcm_open(flags | DEBUG_ON, device);
    if (!m_u->handle->handle) {
        pa_log("%s : ERROR : device opened failed ", __FUNCTION__ );
        goto Error;
    }

    m_u->handle->handle->flags = flags;
    err = setHardwareParams(m_u->handle);
    if (err !=  NO_ERROR) {
        pa_log("%s :  ERROR : setHardwareParams failed", __FUNCTION__);
        goto Error;
    }

    err = setSoftwareParams(m_u->handle);
    if (err !=  NO_ERROR) {
        pa_log("%s : ERROR : setSoftwareParams failed", __FUNCTION__);
        goto Error;
    }

    err = pcm_prepare(m_u->handle->handle);
    if (err) {
        pa_log("%s : ERROR :  pcm_prepare failed", __FUNCTION__);
        goto Error;
    }

    if (ioctl(m_u->handle->handle->fd, SNDRV_PCM_IOCTL_START)) {
        pa_log("startVoiceCall:SNDRV_PCM_IOCTL_START failed\n");
        goto Error;
    }

    return NO_ERROR;

Error:
        return NO_INIT;

}

void startVoiceCall(const char *device)
{

    m_u->handle = pa_xnew0(alsaHandle, 1);

    if (m_u->handle) {
        memset (m_u->handle, 0x00, sizeof *m_u->handle);
        m_u->handle->channels   = 1; //change for equal volume levels in headset
        m_u->handle->sampleRate = DEFAULT_SAMPLE_RATE;
        m_u->handle->latency    = 0;
        m_u->handle->rxHandle   = 0;
        m_u->handle->txHandle   = 0;
        m_u->handle->ucMgr      = 0;
        m_u->handle->lterxHandle   = 0;
        m_u->handle->ltetxHandle   = 0;
    } else {
        pa_log("ERROR : Unable to allocate memory for alsa_handle_t");
        goto Error;
    }

    if (ucm_open_pcm_device(device, PCM_OUT | PCM_MONO)) {
        pa_log_error("Error : OUTPUT device opening failed\n");
        goto Error;
    }

    m_u->handle->rxHandle = m_u->handle->handle;

    if (ucm_open_pcm_device(device, PCM_IN | PCM_MONO)) {
        pa_log_error("Error : INPUT device opening failed\n");
        goto Error;
    }

    m_u->handle->txHandle = m_u->handle->handle;

    return;

Error:

    if(m_u->handle->handle)
        close(m_u->handle->handle);

}


int ucm_close_pcm_device(struct pcm *pcmHandle)
{

    if (pcmHandle)
        return( pcm_close(pcmHandle));
    else
        return 0;
}


void stopVoiceCall()
{

    if (ucm_close_pcm_device(m_u->handle->rxHandle))
        pa_log("pcm_close failed for rxHandle");
    else {
        m_u->handle->rxHandle = 0;
        pa_log_debug("rxHandle pcm_close successful");
    }

    if (ucm_close_pcm_device(m_u->handle->txHandle))
        pa_log("pcm_close failed for txhandle");
    else {
        m_u->handle->txHandle = 0;
        pa_log_debug("txhandle pcm_close successful");
    }

    if (ucm_close_pcm_device(m_u->handle->lterxHandle))
        pa_log("pcm_close failed for lterxHandle");
    else {
        m_u->handle->lterxHandle = 0;
        pa_log_debug("lterxHandle pcm_close successful");
    }

    if (ucm_close_pcm_device(m_u->handle->ltetxHandle))
        pa_log("pcm_close failed for ltetxHandle");
    else {
        m_u->handle->ltetxHandle = 0;
        pa_log_debug("ltetxHandle pcm_close successful");
    }

    m_u->handle->handle = NULL;

    if (m_u->handle) {
        pa_xfree(m_u->handle);
        m_u->handle = NULL;
    }
}


void ucm_apply_mixerctl_for_HFP(void) {

#if HAVE_ACDB
    int new_rx_device = 0;
    int new_tx_device = 0;
#endif

    if (m_u->BTdeviceType == eBTDevice_Wideband) {

        snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_WB_RX);
#if HAVE_ACDB
        new_rx_device = DEVICE_BT_SCO_WB_RX_ACDB_ID;
#endif

        if (!m_u->NREC_On) {
            snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_BT_SCO_WB_NREC_TX);
#if HAVE_ACDB
            new_tx_device = DEVICE_BT_SCO_MIC_WB_NREC_TX_ACDB_ID;
#endif
        } else {
            snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_WB_TX);
#if HAVE_ACDB
            new_tx_device = DEVICE_BT_SCO_WB_TX_ACDB_ID;
#endif
        }
    } else {

         snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_NB_RX);
#if HAVE_ACDB
         new_rx_device = DEVICE_BT_SCO_RX_ACDB_ID;
#endif

         if (!m_u->NREC_On) {
             snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_BT_SCO_NREC_TX);
#if HAVE_ACDB
             new_tx_device = DEVICE_BT_SCO_MIC_NREC_TX_ACDB_ID;
#endif
          } else {
              snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_NB_TX);
#if HAVE_ACDB
              new_tx_device = DEVICE_BT_SCO_TX_ACDB_ID;
#endif
          }
    }
}

void pa_set_NREC(bool value, char *scenario){

    int devs=0, i;
    const char **devList = NULL;

    if (m_u->NREC_On != value) {

        m_u->NREC_On = value;
        pa_log_debug("setNREC: NREC_On set to %d and scenario = %s", value, scenario);

        if ( m_u->onActiveCall &&
             m_u->BTHfpConnected &&
             pa_streq (scenario, PHONE_SCENARIO_BT_SCO)) {

             devs = snd_use_case_get_list(m_u->ucm_mgr, "_enadevs", &devList);
             if (devs) {
                 for(i = 0; i < devs; i++)
                     snd_use_case_set(m_u->ucm_mgr, "_disdev", devList[i]);
              }
              ucm_apply_mixerctl_for_HFP();
        } else
            pa_log_info("SetNREC:Not Voice Call scenario, Nothing to change");
    }
}

void pa_set_BTdevice_type(int type, int hfpStatus){
    if (type != 0)
        m_u->BTdeviceType = eBTDevice_Wideband;
    else
        m_u->BTdeviceType = eBTDevice_NarrowBand;

    m_u->BTHfpConnected = hfpStatus;
    pa_log_info("Set BT device to %s and hfpStatus = %d",type?"Wideband":"narrowband", hfpStatus);
}

void pa_set_routing_voice_call(char *scenario, bool voLTE, int BTdeviceType) {

#if HAVE_ACDB
    int new_rx_device = 0;
    int new_tx_device = 0;
#endif

    pa_log_debug("module-ucm: pa_set_routing_voice_call: (%s), voLTE (%d), BTdeviceType(%d)", scenario, voLTE, BTdeviceType);

    snd_use_case_set(m_u->ucm_mgr, "_verb", SND_USE_CASE_VERB_INACTIVE);

    ucm_disable_all_mods_and_devs(m_u);

    snd_use_case_set(m_u->ucm_mgr, "_verb", SND_USE_CASE_VERB_VOICECALL);
    snd_use_case_set(m_u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_MUSIC);
    snd_use_case_set(m_u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_CAPTURE_MUSIC);

    if (pa_streq(scenario, SCENARIO_BACK_SPEAKER)) {
        snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_VOC_SPEAKER);
        snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER_DUAL_MIC_ENDFIRE);
#if HAVE_ACDB
        new_tx_device = DEVICE_DUALMIC_SPEAKER_TX_ENDFIRE_FV5_ACDB_ID;
        new_rx_device = DEVICE_SPEAKER_MONO_RX_ACDB_ID;
#endif
    } else if (pa_streq (scenario, SCENARIO_BT_SCO)) {
        m_u->BTdeviceType = BTdeviceType;
        ucm_apply_mixerctl_for_HFP();
    } else
        pa_log_error("\nError : setting nothing as device........... scenario = %s\n", scenario);

    if (voLTE)
        snd_use_case_set(m_u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOLTE);

#if HAVE_ACDB
    pa_log_debug("ACDB fine tuning for voice call\n");
    acdb_loader_send_voice_cal(new_rx_device, new_tx_device);
#endif

    if (!m_u->onActiveCall) {
        startVoiceCall(VOICE_CALL_HW);
        m_u->onActiveCall = true;
    } else
        pa_log_info("already on call");
}

void pa_set_routing_media(char *scenario) {

    if (m_u->onActiveCall) {
        pa_log_debug("pa_setRoutingMedia Scenario %s", scenario);
        snd_use_case_set(m_u->ucm_mgr, "_verb", SND_USE_CASE_VERB_INACTIVE);

        ucm_disable_all_mods_and_devs(m_u);

        stopVoiceCall();
        m_u->onActiveCall = false;
        m_u->BTHfpConnected = false;

       if ( m_u->sink_state != PA_SINK_SUSPENDED ||
            m_u->tinycompress_sink_state != PA_SINK_SUSPENDED) {
            snd_use_case_set(m_u->ucm_mgr, "_verb", SND_USE_CASE_VERB_HIFI);
            snd_use_case_set(m_u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
            snd_use_case_set(m_u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
#if HAVE_ACDB
            acdb_loader_send_audio_cal(DEVICE_SPEAKER_RX_ACDB_ID, 1);
#endif
        }
    }
}

static void set_mixers_for_recording(struct userdata *u,char *TxDevice) {

    const char *current_verb = NULL;
    bool foundDevice = false;
    bool foundModifier = false;

    pa_assert(u);
    pa_assert(TxDevice);

    foundDevice = ucm_device_exists(u->ucm_mgr, TxDevice);
    foundModifier = ucm_modifier_exists(u->ucm_mgr, SND_USE_CASE_MOD_CAPTURE_MUSIC);

    snd_use_case_get(u->ucm_mgr, "_verb", &current_verb);
    if (pa_streq(current_verb, SND_USE_CASE_VERB_INACTIVE)) {
        pa_log_info("setting verb (HiFi)");
        snd_use_case_set(u->ucm_mgr, "_verb", SND_USE_CASE_VERB_HIFI);
    }
    if (!foundDevice) {
        pa_log_info("setting device (%s)", TxDevice);
        snd_use_case_set(u->ucm_mgr, "_enadev", TxDevice);
    }
    if (!foundModifier) {
        pa_log_info("setting Capture Music");
        snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_CAPTURE_MUSIC);
    }
    if (current_verb)
        free (current_verb);
}

static void ucm_disable_mixers_for_recording(struct userdata *u)
{
    pa_assert(u);

    if (!u->onActiveCall) {
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_QVOICE_VR_TX);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_VOICE_RECOGNITION);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_BTSCO_NB_TX);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_BT_SCO_NREC_TX);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_BTSCO_WB_TX);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_BT_SCO_WB_NREC_TX);
        snd_use_case_set(u->ucm_mgr, "_disdev", SND_USE_CASE_DEV_HANDSETVM_TX);
    }
}

int  select_devices(struct userdata *u)
{
    int new_tx_device = 0;

    if (u) {
        pa_log_debug("%s %s",__func__,u->loopback_device);

        snd_use_case_set(u->ucm_mgr, "_verb", SND_USE_CASE_VERB_HIFI);
        snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_VOIP);

        if (!strcmp(u->loopback_device , SND_USE_CASE_DEV_BLUETOOTH)) {
            if (u->BTdeviceType == eBTDevice_Wideband) {
                pa_log_debug("loopBack select_devices for Wide Band");
                snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_WB_RX);
                snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_WB_TX);
            } else {
                pa_log_debug("loopBack select_devices for Narrow Band");
                snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_NB_RX);
                snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_BTSCO_NB_TX);
            }
            new_tx_device = DEVICE_BT_SCO_TX_ACDB_ID;
        }
        else {
            if (!strcmp(u->loopback_device , SND_USE_CASE_DEV_LB_SUB_MIC1)) {
                    snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
                    snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_LB_SUB_MIC1);
                    new_tx_device = DEVICE_LB_SUB_MIC1_ACDB_ID;
            }
            else if (!strcmp(u->loopback_device , SND_USE_CASE_DEV_LB_SUB_MIC2)) {
                    snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
                    snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_LB_SUB_MIC2);
                    new_tx_device = DEVICE_LB_SUB_MIC2_ACDB_ID;
            }
        }
        acdb_loader_send_audio_cal(new_tx_device, 2);
    }
    return 0;
}

void *loopback_thread_func(struct userdata *u)
{
    int ret = 0;
    int pcm_dev_rx_id, pcm_dev_tx_id;
    uint32_t size;

    pa_log_debug("%s: entered",__func__);

    if (u) {
        select_devices(u);

        pcm_dev_rx_id = 3;
        pcm_dev_tx_id = 3;

        if (pcm_dev_rx_id < 0 || pcm_dev_tx_id < 0) {
            pa_log("%s: Invalid PCM devices (rx: %d tx: %d) for the usecase",
                  __func__, pcm_dev_rx_id, pcm_dev_tx_id);
            ret = -EIO;
            goto error_loopback_thread;
        }

        startVoiceCall(VOIP_CALL_HW);
        size =320;

        pa_log_debug("%s: size(%d)", __func__, size);
        if (u->adev->loopback_option == AUDIO_DELAY_LOOPBACK) {
            char *buffer[LOOPBACK_DELAY_MSEC];
            int i, buffer_count = -1;

            memset(buffer, 0, sizeof(buffer));

            pa_log_debug("%s: delayed loopback mode", __func__);

            do {
                buffer_count++;

                if (buffer_count < LOOPBACK_DELAY_MSEC)
                    buffer[buffer_count] = malloc(size);

                if (u->handle && pcm_read(u->handle->txHandle,
                    buffer[buffer_count % LOOPBACK_DELAY_MSEC], size)) {
                    pa_log("%s: %s", __func__, pcm_error(u->handle->txHandle));
                    ret = -EIO;
                    break;
                }

                if (buffer_count < LOOPBACK_DELAY_MSEC - 1)
                    continue;

                if (u->handle && pcm_write(u->handle->rxHandle,
                    buffer[buffer_count % (LOOPBACK_DELAY_MSEC - 1)], size)) {
                    pa_log("%s: %s", __func__, pcm_error(u->handle->rxHandle));
                    ret = -EIO;
                    break;
                }

                memset(buffer[buffer_count % (LOOPBACK_DELAY_MSEC - 1)], 0, size);
            } while (u->adev->loopback_option ==
                    AUDIO_DELAY_LOOPBACK && u->adev->mKillLoopbackThread == false);

            for (i = 0; i < LOOPBACK_DELAY_MSEC; i++)
                if (buffer[i] != NULL)
                    free(buffer[i]);
        } else {
            char *buffer = malloc(size);

            if (buffer) {
                pa_log_debug("%s: normal loopback mode", __func__);
                while ((u->adev->loopback_option == AUDIO_NORMAL_LOOPBACK
                    || u->adev->loopback_option == AUDIO_TESTMODE_LOOPBACK) &&
                    !pcm_read(u->handle->txHandle, buffer, size)) {
                    if (pcm_write(u->handle->rxHandle, buffer, size)) {
                        pa_log("%s: %s", __func__, pcm_error(u->handle->rxHandle));
                        ret = -EIO;
                        break;
                    }
                }
                free(buffer);
            }
        }

        if (u->handle) {
            pcm_close(u->handle->rxHandle);
            pcm_close(u->handle->txHandle);
        }
    }

    pa_log_debug("%s: done", __func__);

error_loopback_thread:

    snd_use_case_set(u->ucm_mgr,"_disdev", SND_USE_CASE_DEV_LB_SUB_MIC1);
    snd_use_case_set(u->ucm_mgr,"_disdev", SND_USE_CASE_DEV_LB_SUB_MIC2);

    pa_log_debug("%s: exit: status(%d)", __func__, ret);
}

int create_loopback_thread(struct audio_device *adev) {
    pa_log_debug("%s: enter",__func__);

    if (adev) {
        pthread_mutex_lock(&adev->loopback_lock);
        adev->mKillLoopbackThread = false;
        pthread_create(&adev->loopback_thread, (const pthread_attr_t *) NULL,
            &loopback_thread_func, m_u);
        adev->mIsLoopbackMode = true;
        pthread_mutex_unlock(&adev->loopback_lock);
    }
    pa_log_debug("%s: done",__func__);

    return 0;
}

static int destroy_loopback_thread(struct audio_device * adev) {
    const char **modList = NULL;
    const char **devList = NULL;
    int mods, devs, i;

    if (adev) {
        if(adev->mIsLoopbackMode == false) {
            pa_log_debug("%s: loopback thread not live",__func__);
            return -EINVAL;
        }

        pa_log_debug("%s: enter, mLoopbackMode: %d",__func__, adev->mIsLoopbackMode);
        pthread_mutex_lock(&adev->loopback_lock);

        adev->mKillLoopbackThread = true;
        adev->mIsLoopbackMode = false;
        pthread_join(adev->loopback_thread, (void **) NULL);

        pthread_mutex_unlock(&adev->loopback_lock);
        mods = snd_use_case_get_list(m_u->ucm_mgr, "_enamods", &modList);
        devs = snd_use_case_get_list(m_u->ucm_mgr, "_enadevs", &devList);
        if (mods) {
            for(i = 0; i < mods; i++) {
                snd_use_case_set(m_u->ucm_mgr, "_dismod", modList[i]);
            }
        }
        if (devs) {
            for(i = 0; i < devs; i++) {
                snd_use_case_set(m_u->ucm_mgr, "_disdev", devList[i]);
            }
        }
        snd_use_case_set(m_u->ucm_mgr, "_verb", "Inactive");
    }
    pa_log_debug("%s: done",__func__);

    return 0;
}

void set_loopback_handle(struct audio_device * adev , char * device, int enable){
    pa_log_debug("@@@set_loopback_handle :: entered");
    m_u->loopback_device = device;
    /*
     enable = 1 : AAT    (can not control loopback volume)
     enable = 2 : Testmode Loopback (can control loopback volume via voip volume)
     enable = 3 : Delayed (1sec) Loopback (tinyaloop -o 2)
     */

    if (((enable == 1) || (enable == 2) || (enable == 3))) {
        if (!adev->mIsLoopbackMode) {
            pa_log_debug("@@@set_loopback_handle :: enable, mode = %d",enable);

            adev->mIsLoopbackMode = true;

            if (enable == 2) { /* testmode loopback*/
                adev->loopback_option  = AUDIO_TESTMODE_LOOPBACK;
                create_loopback_thread(adev);
                usleep(200*1000);
            }
            else if (enable == 1) { /* AAT loopback */
                adev->loopback_option  = AUDIO_NORMAL_LOOPBACK;
                create_loopback_thread(adev);
                usleep(200*1000);
                /*platform_set_voip_volume(adev->platform, 1);*/
            } else { /* delayed loopback, mode 3*/
                adev->loopback_option  = AUDIO_DELAY_LOOPBACK;
                create_loopback_thread(adev);
                usleep(200*1000);
                /*platform_set_voip_volume(adev->platform, 1);*/
            }
        }
    } else {
        if (adev->mIsLoopbackMode) {
            pa_log_debug("@@@set_loopback_handle :: disable start");
            adev->loopback_option  = AUDIO_LOOPBACK_OFF;

            destroy_loopback_thread(adev);
            usleep(50*1000);

            adev->mIsLoopbackMode = false; /*set 'false' after destroy_loopback_thread is called*/
            pa_log_debug("@@@set_loopback_handle :: disable end");
        }
    }
    pa_log_debug("@@@set_loopback_handle :: end");
}

int loopback_set_parameters(char * value) {
    if (pa_streq(value, "SPK_ON")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_LB_SUB_MIC1, 1);
        pa_log_debug("@@@LOOPBACK_HSPK_ON DONE");
    } else if (pa_streq(value, "SPK_OFF")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_LB_SUB_MIC1 , 0);
        pa_log_debug("@@@LOOPBACK_HSPK_OFF DONE");
    } else if (pa_streq(value, "BT_ON")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_BLUETOOTH, 1);
        pa_log_debug("@@@LOOPBACK_BT_ON DONE");
    } else if (pa_streq(value, "BT_OFF")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_BLUETOOTH , 0);
        pa_log_debug("@@@LOOPBACK_BT_OFF DONE");
    } else if (pa_streq(value, "SUBMIC_ON")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_LB_SUB_MIC2, 1);
        pa_log_debug("@@@LOOPBACK_SUBMIC_ON DONE");
    } else if (pa_streq(value, "SUBMIC_OFF")) {
        set_loopback_handle(m_u->adev, SND_USE_CASE_DEV_LB_SUB_MIC2 , 0);
        pa_log_debug("@@@LOOPBACK_SUBMIC_OFF DONE");
    }

    return 0;
}

static pa_hook_result_t sink_state_changed_cb (pa_core *c, pa_object *o, struct userdata *u) {

    pa_sink *s = PA_SINK(o);
    pa_assert(c);
    pa_assert(u);

    if(!pa_sink_isinstance(o))
        return PA_HOOK_OK;

    if (pa_streq(s->name, "pcm_output")) {
        u->sink_state = pa_sink_get_state(s);
        pa_log_info("sink state is (%d)", u->sink_state);

    } else if (pa_streq(s->name, "tinycompress")) {
        u->tinycompress_sink_state = pa_sink_get_state(s);
        pa_log_info("tinycompress sink state is (%d)", u->tinycompress_sink_state);

    } else
        return PA_HOOK_OK;

    /* if any sink suspened then update mixer control */
    if (pa_sink_get_state(s) != PA_SINK_SUSPENDED)
        return PA_HOOK_OK;

    /* update stream state */
    ucm_update_stream_state(u);

    /* update mixer controls */
    ucm_update_mixer_control(u);

    return PA_HOOK_OK;
}

static pa_hook_result_t source_state_changed_cb (pa_core *c, pa_object *o, struct userdata *u) {

    pa_source *s = PA_SOURCE(o);
    pa_assert(c);
    pa_assert(u);

    if(!pa_source_isinstance(o))
        return PA_HOOK_OK;

    if (pa_streq(s->name, "pcm_input")) {
        u->source_state = pa_source_get_state(s);
        pa_log_info("source state is (%d)", u->source_state);
    } else
        return PA_HOOK_OK;

    /* if any source suspened then update mixer control */
    if (pa_source_get_state(s) != PA_SOURCE_SUSPENDED)
        return PA_HOOK_OK;

    /* update stream state */
    ucm_update_stream_state(u);

    /* update mixer controls */
    ucm_update_mixer_control(u);
    u->recording = false;
    return PA_HOOK_OK;
}

static pa_hook_result_t ucm_sink_input_move_finish_callback(pa_core *core, pa_sink_input *i, struct userdata *u) {

    pa_assert(core);
    pa_assert(i);
    pa_assert(u);

    const char *current_verb = NULL;
    long value = 0;

    pa_log_debug("ucm_sink_input_move_end_callback : moved stream to %s", i->sink->name);

    if (pa_streq( i->sink->name , "pcm_output") || pa_streq( i->sink->name , "tinycompress")) {

            snd_use_case_get(u->ucm_mgr, "_verb", &current_verb);
            if (current_verb && pa_streq(current_verb, SND_USE_CASE_VERB_INACTIVE))
                snd_use_case_set(u->ucm_mgr, "_verb", "HiFi");

            if (snd_use_case_geti(u->ucm_mgr,"_devstatus/Speaker", &value) != -EINVAL){
                if (!value)
                    snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
            }

            if (pa_sink_input_is_passthrough(i)) {
                snd_use_case_geti(u->ucm_mgr,"_modstatus/Play Tunnel", &value);
                if (!value)
                    snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
            }

            if (current_verb) {
                free(current_verb);
                current_verb = NULL;
             }

#if HAVE_ACDB
           acdb_loader_send_audio_cal(DEVICE_SPEAKER_RX_ACDB_ID, 1);
#endif
    }
           return PA_HOOK_OK;
}

static bool ucm_modifier_device_exists(snd_use_case_mgr_t *uc_mgr, const char *usecase, char *dev_mod) {

    const char **list = NULL;
    bool ret = false;
    int i, devs;

    pa_assert(uc_mgr);
    pa_assert(usecase);

    devs = snd_use_case_get_list(uc_mgr, usecase, &list);

    for(i = 0; i < devs; i++) {
        if (pa_streq(list[i], dev_mod)) {
            ret = true;
            break;
        }
    }

    snd_use_case_free_list(list, devs);

    return ret;
}

static bool ucm_device_exists(snd_use_case_mgr_t *uc_mgr, const char *dev_name) {

    return ucm_modifier_device_exists(uc_mgr, "_enadevs", dev_name);
}

static bool ucm_modifier_exists(snd_use_case_mgr_t *uc_mgr, const char *mod_name) {

    return ucm_modifier_device_exists(uc_mgr, "_enamods", mod_name);
}

static pa_hook_result_t ucm_sink_input_fixate_hook_callback(pa_core *c, pa_sink_input_new_data *data, struct userdata *u) {

    const char *current_verb = NULL;
    bool found_device = false;
    bool found_modifier = false;

    pa_assert(c);
    pa_assert(u);

    found_device = ucm_device_exists(u->ucm_mgr, SND_USE_CASE_DEV_SPEAKER);
    found_modifier = ucm_modifier_exists(u->ucm_mgr, SND_USE_CASE_MOD_PLAY_TUNNEL);

    snd_use_case_get(u->ucm_mgr, "_verb", &current_verb);

    if (pa_streq(current_verb, SND_USE_CASE_VERB_INACTIVE)) {

        pa_log_info("setting verb (HiFi)");
        snd_use_case_set(u->ucm_mgr, "_verb", SND_USE_CASE_VERB_HIFI);

        if (pa_sink_input_new_data_is_passthrough(data) && !found_modifier) {
            pa_log_info("setting modifier (play tunnel)");
            snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
        }

        if (!found_device) {
            pa_log_info("setting device (Speaker)");
            snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
#if HAVE_ACDB
            acdb_loader_send_audio_cal(DEVICE_SPEAKER_RX_ACDB_ID, 1);
#endif
        }

    } else if (pa_streq(current_verb, SND_USE_CASE_VERB_HIFI)) {

        if (pa_sink_input_new_data_is_passthrough(data) && !found_modifier) {
            pa_log_info("setting modifier (play tunnel)");
            snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_PLAY_TUNNEL);
        }

        if (!found_device) {
            pa_log_info("setting device (Speaker)");
            snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);
#if HAVE_ACDB
            acdb_loader_send_audio_cal(DEVICE_SPEAKER_RX_ACDB_ID, 1);
#endif
        }

    }

    if (current_verb)
        free((void *)current_verb);

    return PA_HOOK_OK;
}

static pa_hook_result_t ucm_sink_input_state_changed_callback(pa_core *c, pa_sink_input *data, struct userdata *u) {

    pa_assert(c);
    pa_assert(u);

    const char *current_verb = NULL;
    long value = 0;
    pa_sink_input_state_t state = pa_sink_input_get_state(data);

    if (state == PA_SINK_INPUT_CORKED) {
        //nothing to do
    } else if (state == PA_SINK_INPUT_RUNNING || state == PA_SINK_INPUT_DRAINED) {

        snd_use_case_get(u->ucm_mgr, "_verb", &current_verb);
        if (pa_streq(current_verb, SND_USE_CASE_VERB_INACTIVE))
            snd_use_case_set(u->ucm_mgr, "_verb", "HiFi");

        free(current_verb);
        current_verb = NULL;

        snd_use_case_geti(u->ucm_mgr,"_devstatus/Speaker", &value);
        if (!value)
            snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER);

        if (pa_sink_input_is_passthrough(data)) {
            snd_use_case_geti(u->ucm_mgr,"_modstatus/Play Tunnel", &value);
            if (!value)
                snd_use_case_set(u->ucm_mgr, "_enamod", "Play Tunnel");
        }

#if HAVE_ACDB
        acdb_loader_send_audio_cal(DEVICE_SPEAKER_RX_ACDB_ID, 1);
#endif
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t ucm_source_output_state_changed_callback(pa_core *c, pa_source_output *data, struct userdata *u) {

    pa_source_output_state_t state;
    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    state = pa_source_output_get_state(data);

    if (state == PA_SOURCE_OUTPUT_RUNNING) {
        pa_log_info("PA_SOURCE_OUTPUT_RUNNING");
        set_mixers_for_recording(u, SND_USE_CASE_DEV_HANDSETVM_TX);
    }

    return PA_HOOK_OK;
}


static pa_hook_result_t ucm_source_output_fixate_hook_callback(pa_core *c, pa_source_output_new_data *data,
            struct userdata *u) {

#if HAVE_ACDB
    int new_tx_device = 0;
#endif
    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    if (u->BTHfpConnected && pa_streq(data->source->name, "pqvoice")) {
        if (u->BTdeviceType == eBTDevice_Wideband) {
            if (!u->NREC_On)
                set_mixers_for_recording(u,SND_USE_CASE_BT_SCO_WB_NREC_TX);
            else
                set_mixers_for_recording(u,SND_USE_CASE_DEV_BTSCO_WB_TX);
        } else {
            if (!u->NREC_On)
                set_mixers_for_recording(u,SND_USE_CASE_BT_SCO_NREC_TX);
            else
                set_mixers_for_recording(u,SND_USE_CASE_DEV_BTSCO_NB_TX);
        }
    } else {
        if (!u->recording) {
            if (pa_streq(data->source->name, "pvoiceactivator")) {
                set_mixers_for_recording(u,SND_USE_CASE_DEV_VOICE_RECOGNITION);
#if HAVE_ACDB
                new_tx_device = DEVICE_VOICE_RECOGNITION_ACDB_ID;   /* 62 for Voice activator */
#endif
            } else if (pa_streq(data->source->name, "pqvoice")) {
                set_mixers_for_recording(u,SND_USE_CASE_DEV_QVOICE_VR_TX);
#if HAVE_ACDB
                new_tx_device = DEVICE_QVOICE_VR_TX_ACDB_ID;    /* 60 for Qvoice */
#endif
            } else if (pa_streq(data->source->name, "pptt")) {
                set_mixers_for_recording(u,"SpeakerPTT Tx");
#if HAVE_ACDB
                new_tx_device = DEVICE_PTT_SPEAKER_TX_ACDB_ID;    /* 66 for LGU PTT */
#endif
            } else {
                set_mixers_for_recording(u, SND_USE_CASE_DEV_HANDSETVM_TX);
#if HAVE_ACDB
                new_tx_device = DEVICE_VOICE_MEMO_ACDB_ID;    /* 63 for voice memo */
#endif
            }
#if HAVE_ACDB
            acdb_loader_send_audio_cal(new_tx_device, 2);
#endif
            u->recording = true;
        }
    }

    return PA_HOOK_OK;
}

void pa_set_routing_HFP_call(bool scostatus) {
    pa_log_info("pa_set_routing_HFP_call: SCO status =%d ", scostatus);
    if (scostatus) {
        if (!m_u->onActiveCall) {
            pa_log_info("pa_set_routing_HFP_call: start HFP call");
            ucm_disable_verb(m_u);
            ucm_disable_all_mods_and_devs(m_u);
            snd_use_case_set(m_u->ucm_mgr, "_verb", "HFP Call");
            snd_use_case_set(m_u->ucm_mgr, "_enadev", "HandsetVM Tx");
            snd_use_case_set(m_u->ucm_mgr, "_enadev", "Speaker");
            startVoiceCall(HFP_CALL_HW);
            m_u->onActiveCall = true;
        }
    } else {
        if (m_u->onActiveCall) {
            pa_log_info("pa_set_routing_HFP_call: End HFP call");
            stopVoiceCall();
            snd_use_case_set(m_u->ucm_mgr, "_verb", "Inactive");
            snd_use_case_set(m_u->ucm_mgr, "_disdev", "HandsetVM Tx");
            snd_use_case_set(m_u->ucm_mgr, "_disdev", "Speaker");
            m_u->onActiveCall = false;
        }
    }
}

int pa__init(pa_module *m) {

    struct userdata *u;
    pa_modargs *ma = NULL;
    const char *card_name = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module ucm arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->ucm_mgr = NULL;
    u->sink_state = PA_SINK_SUSPENDED;
    u->tinycompress_sink_state = PA_SINK_SUSPENDED;
    u->source_state = PA_SOURCE_SUSPENDED;
    u->stream_status = UCM_NONE;
    u->onActiveCall = false;
    u->BTHfpConnected = false;
    u->BTdeviceType = eBTDevice_Wideband;
    u->NREC_On = true;
    u->recording = false;
    u->adev = pa_xnew0(struct audio_device, 1);
    if (!u->adev) {
        pa_log("Could not create audio device");
        goto fail;
    }
    u->adev->mIsLoopbackMode = false;
    u->adev->loopback_option= AUDIO_LOOPBACK_OFF;
    if (!(card_name = pa_modargs_get_value(ma, "card_name", NULL))) {
        pa_log("Failed to parse card name");
        goto fail;
    }

    if(snd_use_case_mgr_open(&u->ucm_mgr, card_name)){
        pa_log("Failed to open ucm card" );
        goto fail;
    }

    if (u->ucm_mgr) {
        if (snd_use_case_set(u->ucm_mgr, "_verb", SND_USE_CASE_VERB_HIFI) < 0)
            pa_log("failed to set verb to HiFi");
        if (snd_use_case_set(u->ucm_mgr, "_enamod", SND_USE_CASE_MOD_CAPTURE_MUSIC) < 0)
            pa_log("failed to set modifier to Capture Music");
        if (snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_SPEAKER) < 0)
            pa_log ("failed to set device  to Speaker");
        if (snd_use_case_set(u->ucm_mgr, "_enadev", SND_USE_CASE_DEV_QVOICE_VR_TX) < 0)
            pa_log ("failed to set device  to Qvoice DMIC Endfire");
    }

#if HAVE_ACDB
    if ((acdb_loader_init_ACDB()) < 0)
        pa_log("Failed to initialize ACDB");
#endif

    u->sink_state_changed_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED],
            PA_HOOK_EARLY, (pa_hook_cb_t) sink_state_changed_cb, u);

    u->source_state_changed_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED],
            PA_HOOK_EARLY, (pa_hook_cb_t) source_state_changed_cb, u);

    u->ucm_sink_input_fixate_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE],
            PA_HOOK_EARLY, (pa_hook_cb_t) ucm_sink_input_fixate_hook_callback, u);

    u->ucm_source_output_fixate_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE],
            PA_HOOK_EARLY, (pa_hook_cb_t) ucm_source_output_fixate_hook_callback, u);

    u->ucm_sink_input_state_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED],
            PA_HOOK_EARLY, (pa_hook_cb_t)ucm_sink_input_state_changed_callback, u);

    u->ucm_source_output_state_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED],
            PA_HOOK_EARLY, (pa_hook_cb_t)ucm_source_output_state_changed_callback, u);

    u->ucm_sink_input_move_finish_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH],
            PA_HOOK_EARLY, (pa_hook_cb_t)ucm_sink_input_move_finish_callback, u);

    u->voice_RX_muted = false;
    u->mixer_handle = mixer_open(MIXER_DEVICE);
    if (!u->mixer_handle)
    {
        pa_log("Unable to open control device");
        goto fail;
    }

    m_u = u;

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module *m) {

    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

#if HAVE_ACDB
    acdb_loader_deallocate_ACDB();
#endif

    if (u->ucm_mgr)
        snd_use_case_mgr_close(u->ucm_mgr);

    if (u->sink_state_changed_slot)
        pa_hook_slot_free(u->sink_state_changed_slot);

    if (u->source_state_changed_slot)
        pa_hook_slot_free(u->source_state_changed_slot);

    if (u->ucm_sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->ucm_sink_input_fixate_hook_slot);

    if (u->ucm_source_output_fixate_hook_slot)
        pa_hook_slot_free(u->ucm_source_output_fixate_hook_slot);

    if (u->ucm_sink_input_state_changed_hook_slot)
        pa_hook_slot_free(u->ucm_sink_input_state_changed_hook_slot);

    if (u->ucm_source_output_state_changed_hook_slot)
        pa_hook_slot_free(u->ucm_source_output_state_changed_hook_slot);

    if (u->ucm_sink_input_move_finish_hook_slot)
        pa_hook_slot_free(u->ucm_sink_input_move_finish_hook_slot);

    if (u->mixer_handle)
        mixer_close(u->mixer_handle);

    pa_xfree(u->adev);
    pa_xfree(u);
    m_u = NULL;
}
