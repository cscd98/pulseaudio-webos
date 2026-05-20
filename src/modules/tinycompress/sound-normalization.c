#include<stdio.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "sound-normalization.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>

#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/namereg.h>
#include <pulsecore/idxset.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>
#include <pulsecore/module.h>
#include <pulsecore/conf-parser.h>
#include <sound/asound.h>
#include <alsa-intf/alsa_audio.h>

#define LGE_SOUNDNORMALIZER 1
#define DEFAULT_CONFIG_FILE PA_DEFAULT_CONFIG_DIR PA_PATH_SEP "normalizer.conf"
#define MIXER_DEVICE "/dev/snd/controlC0"
#define SLEEP_TIME 10000

static const pa_normalizer_conf default_conf = {
    .offload_normalizer_enable = true,
    .offload_normalizer_devicespeaker = true,
    .offload_normalizer_makeupgain = 15.0,
    .offload_normalizer_prefilter = true,
    .offload_normalizer_limiterthreshold = 5500,
    .offload_normalizer_limiterslope = 0.8,
    .offload_normalizer_compressorthreshold = 2000,
    .offload_normalizer_compressorslope = 0.42,
    .offload_normalizer_onoff = true,
    .config_file = NULL
};

static struct lge_normalizer_lastvalue lge_normalizer_value = {
    .enable = -E_NODATA,
    .devicespeaker = -E_NODATA,
    .makeupgain = -E_NODATA,
    .prefilter = -E_NODATA,
    .limiterthreshold = -E_NODATA,
    .limiterslope = -E_NODATA,
    .compressorthreshold = -E_NODATA,
    .compressorslope = -E_NODATA,
    .onoff = -E_NODATA,
    .start_offload = -E_NODATA,
};

struct audio_device {
    struct mixer  *mixer;
    pa_normalizer_conf *conf;
};

struct audio_device *snadev;

pa_normalizer_conf * pa_normalizer_conf_new(){
    pa_normalizer_conf *c;

    c = pa_xnewdup(pa_normalizer_conf, &default_conf, 1);

    return c;
}

int pa_normalizer_conf_load(pa_normalizer_conf *c, const char *filename){
    int r = -1;
    FILE *f = NULL;
    pa_config_item table[] = {
        {"offload_normalizer_enable", pa_config_parse_bool, &c->offload_normalizer_enable, NULL},
        {"offload_normalizer_devicespeaker", pa_config_parse_bool, &c->offload_normalizer_devicespeaker, NULL},
        {"offload_normalizer_makeupgain", pa_config_parse_double, &c->offload_normalizer_makeupgain, NULL},
        {"offload_normalizer_prefilter", pa_config_parse_bool, &c->offload_normalizer_prefilter, NULL},
        {"offload_normalizer_limiterthreshold", pa_config_parse_int, &c->offload_normalizer_limiterthreshold, NULL},
        {"offload_normalizer_limiterslope", pa_config_parse_double, &c->offload_normalizer_limiterslope, NULL},
        {"offload_normalizer_compressorthreshold", pa_config_parse_int, &c->offload_normalizer_compressorthreshold, NULL},
        {"offload_normalizer_compressorslope", pa_config_parse_double, &c->offload_normalizer_compressorslope, NULL},
        {"offload_normalizer_onoff", pa_config_parse_bool, &c->offload_normalizer_onoff, NULL},
        {NULL,NULL,NULL,NULL},
    };

    pa_xfree(c->config_file);

    f = filename ?
        pa_fopen_cloexec(c->config_file = pa_xstrdup(filename), "r") :
        pa_open_config_file(DEFAULT_CONFIG_FILE, NULL, NULL, &c->config_file);

    if(!f && errno != ENOENT){
        pa_log("Failed to open configuration file:%s", pa_cstrerror(errno));
        goto finish;
    }

    r = f ? pa_config_parse(c->config_file, f, table, NULL, NULL) : 0;

finish:
    if(f)
        fclose(f);

    return r;
}

void pa_normalizer_conf_free(pa_normalizer_conf *c){
    pa_xfree(c->config_file);
    pa_xfree(c);
}

char * pa_convert_int_to_string(int val)
{
    static char str[32];
    snprintf(str, 32, "%d", val);
    return str;
}

int LGESoundNormalizer_init(void )
{
    if(snadev){
         pa_log_debug("already object is created\n");
         return  E_SUCCESS;
    }
    snadev = calloc(1, sizeof(struct audio_device));
    if (!snadev) {
        pa_log_debug("Could not create the adev object\n");
        goto finish;
    }
    snadev->mixer =  mixer_open(MIXER_DEVICE);
    if(!snadev->mixer ) {
        pa_log("Could not open the mixer device\n");
        goto finish;
    }

    snadev->conf = pa_normalizer_conf_new();
    if(!snadev->conf){
        pa_log("Failed to create conf object");
        goto finish;
    }

    pa_normalizer_conf_load(snadev->conf, NULL);

    return E_SUCCESS;

finish:
    if(snadev){
        if(snadev->conf)
            pa_normalizer_conf_free(snadev->conf);
        if(snadev->mixer)
            mixer_close(snadev->mixer);
        free(snadev);
        snadev = NULL;
    }

    return -E_NODATA;
}

int LGESoundNormalizer_close(void )
{
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if (snadev) {
        mixer_close(snadev->mixer);
        pa_normalizer_conf_free(snadev->conf);
        free(snadev);
        snadev = NULL;
    }

    pa_log_debug("%s:%d ENDS",__func__,__LINE__);

    return LGESoundNormalizer_reset(-E_NODATA);
}

int LGESoundNormalizer_reset(int val)
{
     lge_normalizer_value.enable = val;
     lge_normalizer_value.devicespeaker = val;
     lge_normalizer_value.makeupgain = val;
     lge_normalizer_value.prefilter = val;
     lge_normalizer_value.limiterthreshold = val;
     lge_normalizer_value.limiterslope = val;
     lge_normalizer_value.compressorthreshold = val;
     lge_normalizer_value.compressorslope = val;
     lge_normalizer_value.onoff = val;
     lge_normalizer_value.start_offload = val;

     return E_SUCCESS;
}

static int LGESoundNormalizer_enable(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Enable";
    struct mixer_ctl *ctl;
    int ret;

    lge_normalizer_value.enable = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    ret = mixer_ctl_set(ctl, val);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_devicespeaker(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Devicespeaker";
    struct mixer_ctl *ctl;
    int ret;

    lge_normalizer_value.devicespeaker = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    ret = mixer_ctl_set(ctl, val);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_makeupgain(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Makeupgain";
    struct mixer_ctl *ctl;
    int ret;
    char *temp = NULL;

    lge_normalizer_value.makeupgain = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    temp = pa_convert_int_to_string(val);
    ret = mixer_ctl_set_value(ctl, 1, &temp);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_prefilter(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Prefilter";
    struct mixer_ctl *ctl;
    int ret;

    lge_normalizer_value.prefilter = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    ret = mixer_ctl_set(ctl, val);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_limiterthreshold(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Limiterthreshold";
    struct mixer_ctl *ctl;
    int ret;
    char *temp = NULL;

    lge_normalizer_value.limiterthreshold = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    temp = pa_convert_int_to_string(val);
    ret = mixer_ctl_set_value(ctl, 1, &temp);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_limiterslope(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Limiterslope";
    struct mixer_ctl *ctl;
    int ret;
    char *temp = NULL;

    lge_normalizer_value.limiterslope = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    temp = pa_convert_int_to_string(val);
    ret = mixer_ctl_set_value(ctl, 1, &temp);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_compressorthreshold(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Compressorthreshold";
    struct mixer_ctl *ctl;
    int ret;
    char *temp = NULL;

    lge_normalizer_value.compressorthreshold = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    temp = pa_convert_int_to_string(val);
    ret = mixer_ctl_set_value(ctl, 1, &temp);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_compressorslope(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Compressorslope";
    struct mixer_ctl *ctl;
    int ret;
    char *temp = NULL;

    lge_normalizer_value.compressorslope = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    temp = pa_convert_int_to_string(val);
    ret = mixer_ctl_set_value(ctl, 1, &temp);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

static int LGESoundNormalizer_onoff(struct audio_device *adev, int val)
{
    const char *mixer_ctl_name = "Offload Normalizer Onoff";
    struct mixer_ctl *ctl;
    int ret;

    lge_normalizer_value.onoff = val;
    pa_log_debug("%s:%d STARTS",__func__,__LINE__);
    if(lge_normalizer_value.start_offload != 1) {
        return 0;
    }
    ctl = mixer_get_control(adev->mixer, mixer_ctl_name, MIXER_CARD);
    if (!ctl) {
        pa_log_debug("%s: Could not get ctl for mixer cmd - %s",
                  __func__, mixer_ctl_name);
        return -E_INVALID;
    }
    ret = mixer_ctl_set(ctl, val);
    pa_log_debug("ENDS value=%d ret=%d", val, ret);
    return ret;
}

int adev_set_parameters( int value)
{
    int ret;
    pa_log_debug("%s:%d STARTS ",__func__,__LINE__);
    if(value) {
        ret = LGESoundNormalizer_init();
        if(ret >= 0) {
            pa_log_debug("success");
            #ifdef LGE_SOUNDNORMALIZER
                lge_normalizer_value.start_offload = 1;
            #endif
            ret = LGESoundNormalizer_set_parameters();
        } else {
            pa_log_debug("failed");
            return -E_INVALID;
          }

    } else
       ret = LGESoundNormalizer_close();

   pa_log_debug("%s:%d ENDS ",__func__,__LINE__);
   return ret;
}

int LGESoundNormalizer_set_parameters( void )
{
    int ret = -1;
 #ifdef LGE_SOUNDNORMALIZER
    ret=LGESoundNormalizer_enable(snadev, snadev->conf->offload_normalizer_enable);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Enable %d", __func__, snadev->conf->offload_normalizer_enable);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_devicespeaker(snadev, snadev->conf->offload_normalizer_devicespeaker);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Devicespeaker %d", __func__, snadev->conf->offload_normalizer_devicespeaker);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_makeupgain(snadev, snadev->conf->offload_normalizer_makeupgain * 1000);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Makeupgain %d", __func__, snadev->conf->offload_normalizer_makeupgain);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_prefilter(snadev, snadev->conf->offload_normalizer_prefilter);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Prefilter %d", __func__, snadev->conf->offload_normalizer_prefilter);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_limiterthreshold(snadev, snadev->conf->offload_normalizer_limiterthreshold);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Limiterthreshold %d", __func__, snadev->conf->offload_normalizer_limiterthreshold);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_limiterslope(snadev, snadev->conf->offload_normalizer_limiterslope * 1000);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Limiterslope %d", __func__, snadev->conf->offload_normalizer_limiterslope);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_compressorthreshold(snadev, snadev->conf->offload_normalizer_compressorthreshold);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Compressorthreshold %d", __func__, snadev->conf->offload_normalizer_compressorthreshold);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_compressorslope(snadev, snadev->conf->offload_normalizer_compressorslope * 1000);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Compressorslope %d", __func__, snadev->conf->offload_normalizer_compressorslope);
    usleep(SLEEP_TIME);

    ret = LGESoundNormalizer_onoff(snadev, snadev->conf->offload_normalizer_onoff);
    if (ret < 0)
        pa_log_error("%s: unexpected Normalizer Onoff %d", __func__, snadev->conf->offload_normalizer_onoff);
    usleep(SLEEP_TIME);

#endif
  pa_log_debug("%s:%d ENDS\n",__func__,__LINE__);
  return ret;
}
