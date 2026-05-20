/**********************************************************************
 * Copyright (c) 2002-2009 Palm, Inc. or its subsidiaries.
 * Copyright (c) 2013-2017 LG Electronics, Inc.
 * All rights reserved.
 *
 * module-palm-policy.c - implements policy requests received via an SHM section
 * as determined by audioD.
 **********************************************************************/

#if defined(__i386__) && defined(LOCAL_TEST_BUILD)
#include <pulsecore/config.h>   /* bit of a hack, this, I have a local copy of config.h for x86 in the /usr/include/pulsecore/ dir */
#else
#include <config.h>
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>

#include <pulse/xmalloc.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/namereg.h>
#include <pulsecore/idxset.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>
#include <pulsecore/module.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pulsecore/modargs.h>

#include "module-palm-policy.h"
#include "module-palm-policy-tables.h"

#define _MEM_ZERO(object) (memset (&(object), '\0', sizeof ((object))))
#define _NAME_STRUCT_OFFSET(struct_type, member) ((long) ((unsigned char*) &((struct_type*) 0)->member))

#ifndef PALM_UP_RAMP_MSEC
#define PALM_UP_RAMP_MSEC 600
#endif

#ifndef PALM_DOWN_RAMP_MSEC
#define PALM_DOWN_RAMP_MSEC 400
#endif

#ifndef CLAMP_VOLUME_TABLE
#define CLAMP_VOLUME_TABLE(a)  (((a) < (1)) ? (a) : (1))
#endif

#define RAMP_DURATION_MSEC 1000
#define BSA_SINK_NAME "bsaa2dp"
#define PCM_SINK_NAME "pcm_output"
#define COMBINED_SINK_NAME "combined"
#define COMPRESS_SINK_NAME "tinycompress"
#define REMOTE_MONITOR_SOURCE_NAME "remote.monitor"
#define REMOTE_SINK_NAME "remote"
#define RTP_SINK_NAME "rtp"
#define SCENARIO_STRING_SIZE 28
#define BTADDRESS_STRING_SIZE 28
#define BTPROFILE_STRING_SIZE 10
#define RTP_IP_ADDRESS_STRING_SIZE 28
#define RTP_CONNECTION_TYPE_STRING_SIZE 12
#define DEVICE_NAME_SIZE 50
#define SOURCE_NAME_LENGTH 18
#define SINK_NAME_LENGTH 16
#define DEVICE_NAME_LENGTH 50
/* use this to tie an individual sink_input to the
 * virtual sink it was created against */

struct sinkinputnode {
    int32_t sinkinputidx;       /* index of this sink-input */
    int32_t virtualsinkid;      /* index of virtual sink it was created against, this is our index from
                                 * _enum_systemdependantvirtualsinkmap rather than a pulseaudio sink idx */
    pa_sink_input *sinkinput;   /* reference to sink input with this index */

    pa_bool_t paused;

    PA_LLIST_FIELDS(struct sinkinputnode); /* fields that use a pulse defined linked list */
};

struct sourceoutputnode {
    int32_t sourceoutputidx;    /* index of this sink-input */
    int32_t virtualsourceid;    /* index of virtual sink it was created against, this is our index from
                                 * _enum_systemdependantvirtualsinkmap rather than a pulseaudio sink idx */
    pa_source_output *sourceoutput; /* reference to sink input with this index */
    pa_bool_t paused;

    PA_LLIST_FIELDS(struct sourceoutputnode); /* fields that use a pulse defined linked list */
};

/* user data for the pulseaudio module, store this in init so that
 * stuff we need can be accessed when we get callbacks
 */

struct userdata {
    /* cached references to pulse internals */
    pa_core *core;
    pa_module *module;

    /* slots for hook functions, these get called by pulse */
    pa_hook_slot *sink_input_new_hook_slot; /* called prior to creation of new sink-input */
    pa_hook_slot *sink_input_fixate_hook_slot;
    pa_hook_slot *sink_input_put_hook_slot;
    pa_hook_slot *sink_input_state_changed_hook_slot; /* called on state change, play/pause */
    pa_hook_slot *sink_input_unlink_hook_slot; /* called prior to destruction of a sink-input */
    pa_hook_slot *sink_state_changed_hook_slot; /* for BT sink open-close */

    pa_hook_slot *source_output_new_hook_slot; /* called prior to creation of new source-output */
    pa_hook_slot *source_output_fixate_hook_slot;
    pa_hook_slot *source_output_put_hook_slot;
    pa_hook_slot *source_output_state_changed_hook_slot;
    pa_hook_slot *source_output_unlink_hook_slot; /* called prior to destruction of a source-output */
    pa_hook_slot *module_unload_hook_slot;
    pa_hook_slot *module_load_hook_slot;

    pa_hook_slot *sink_input_move_finish;
    pa_hook_slot *sink_new;
    pa_hook_slot *sink_unlink;
    pa_hook_slot *sink_state_changed_hook;
    pa_hook_slot *source_state_changed_hook_slot;

    /* make sure sink_mapping_table is the same size as
     * defaulmappingtable, since we'll copy that to this */
    struct _mappingtable sink_mapping_table[eVirtualSink_Count];
    struct _mappingtable source_mapping_table[eVirtualSource_Count];

    /* fields for socket - ipc support for audiod */

    int sockfd;                 /* descriptor for socket */
    int newsockfd;              /* descriptor for connections on socket */

    struct sockaddr_un name;    /* filename for socket */
    pa_io_event *sockev;        /* socket event handler */
    pa_io_event *connev;        /* connection event handler */
    pa_bool_t connectionactive; /* do we have an active connection on the socket */

    // Maintain count of sinks opened, as sent to audiod, so that we can re-send data on reconnect for audiod to resync with us
    int32_t audiod_sink_input_opened[eVirtualSink_Count];
    int32_t audiod_source_output_opened[eVirtualSource_Count];
    int32_t n_sink_input_opened;
    int32_t n_source_output_opened;

    /* list of nodes describing sink-input and source-output vs virtual device created against */
    PA_LLIST_HEAD(struct sinkinputnode, sinkinputnodelist);
    PA_LLIST_HEAD(struct sourceoutputnode, sourceoutputnodelist);

    bool bt_connected;    /* BT connection status */
    int32_t media_type;    /* store stream type for combined sink */
#if HAVE_BSA
    pa_module* bt_module;
    char *btAddress;
    char *btProfile;
#endif

    pa_module* rtp_module;
    pa_module* alsa_source;
    pa_module* default_alsa_sink;

    char *destAddress;
    int connectionPort ;
    char *connectionType;

    char *deviceName;
    char *callback_deviceName;

    int external_soundcard_number;
    int external_device_number;

    bool IsUsbConnected;
    bool IsUsbMICConnected;
    int externalSoundCardNumber;
    int externalMICCardNumber;

    pa_module *combined;
    char *scenario;

    pa_sink_state_t pout_previous_state;
    pa_sink_state_t compress_previous_state;
    pa_sink_state_t bsa_previous_state;
    pa_source_state_t current_state;
    bool device_open;
};



static void virtual_source_output_set_physical_source(int virtualsourceid, int physicalsourceid, struct userdata *u);

static void virtual_source_set_mute(int sourceid, int mute, struct userdata *u);

static void virtual_sink_input_set_volume(int sinkid, int volumetoset, int volumetable, struct userdata *u);

static void virtual_sink_input_set_ramp_volume(int volumetoset, int volumetable, bool ramp_up, struct userdata *u);

static void virtual_sink_input_set_volume_with_ramp(int sinkid, int volumetoset, int volumetable, struct userdata *u);

static void virtual_sink_input_set_physical_sink(int virtualsinkid, int physicalsinkid, struct userdata *u);

static void virtual_sink_input_set_mute(int sinkid, int volumetoset, int volumetable, struct userdata *u);

static int sink_suspend_request(struct userdata *u);

static int update_sample_spec(struct userdata *u, int rate);

static void parse_message(char *msgbuf, int bufsize, struct userdata *u);

static void handle_io_event_socket(pa_mainloop_api * ea, pa_io_event * e,
                                   int fd, pa_io_event_flags_t events, void *userdata);

static void handle_io_event_connection(pa_mainloop_api * ea, pa_io_event * e,
                                       int fd, pa_io_event_flags_t events, void *userdata);

static pa_hook_result_t route_sink_input_new_hook_callback(pa_core * c, pa_sink_input_new_data *data,
                                                          struct userdata *u);

static pa_hook_result_t route_sink_input_fixate_hook_callback(pa_core * c, pa_sink_input_new_data *data,
                                                             struct userdata *u);

static pa_hook_result_t route_sink_input_put_hook_callback(pa_core * c, pa_sink_input * si,
                                                           struct userdata *u);

static pa_hook_result_t route_source_output_new_hook_callback(pa_core *c, pa_source_output_new_data *data,
                                                              struct userdata *u);

static pa_hook_result_t route_source_output_fixate_hook_callback(pa_core *c, pa_source_output_new_data *data,
                                                                 struct userdata *u);

static pa_hook_result_t route_source_output_put_hook_callback(pa_core *c, pa_source_output * so,
                                                              struct userdata *u);

static pa_hook_result_t route_sink_input_unlink_hook_callback(pa_core *c, pa_sink_input * data,
                                                             struct userdata *u);

static pa_hook_result_t route_sink_input_state_changed_hook_callback(pa_core *c, pa_sink_input * data,
                                                                    struct userdata *u);
static pa_hook_result_t module_unload_subscription_callback(pa_core *c, pa_module *m, struct userdata *u);
static pa_hook_result_t module_load_subscription_callback(pa_core *c, pa_module *m, struct userdata *u);

/* Hook callback for combined sink routing(sink input move,sink put & unlink) */
static pa_hook_result_t route_sink_input_move_finish_cb(pa_core *c, pa_sink_input *data, struct userdata *u);

static pa_hook_result_t route_sink_put_cb(pa_core *c, pa_sink *sink, struct userdata *u);

static pa_hook_result_t route_sink_unlink_cb(pa_core *c, pa_sink *sink, struct userdata *u);

static pa_hook_result_t route_source_output_state_changed_hook_callback(pa_core *c, pa_source_output * data,
                                                                        struct userdata *u);

static pa_hook_result_t route_source_output_unlink_hook_callback(pa_core *c, pa_source_output *data,
                                                                struct userdata *u);

static pa_hook_result_t route_sink_state_changed_hook_callback(pa_core *c, pa_object *o,
                                                                 struct userdata *u);

static pa_bool_t sink_input_new_data_is_passthrough(pa_sink_input_new_data *data);


static pa_hook_result_t sink_state_changed_cb(pa_core *c, pa_object *o, struct userdata *u);

static pa_hook_result_t source_state_changed_cb(pa_core *c, pa_object *o, struct userdata *u);

static void set_source_inputdevice_on_range(struct userdata *u, char* outputdevice, int startsourcekid, int endsourceid);

static void set_default_source_routing(struct userdata *u, int startsourceid, int endsourceid);

PA_MODULE_AUTHOR("Palm, Inc.");
PA_MODULE_DESCRIPTION("Implements policy, communication with external app is a socket at /tmp/palmaudio");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("No parameters for this module");

static pa_bool_t sink_input_new_data_is_passthrough(pa_sink_input_new_data *data)
{

    if (pa_sink_input_new_data_is_passthrough(data))
        return true;
    else {
        pa_format_info *f = NULL;
        uint32_t idx = 0;

        PA_IDXSET_FOREACH(f, data->req_formats, idx) {
        if (!pa_format_info_is_pcm(f))
            return true;
        }
    }
    return false;
}

static void set_source_inputdevice_on_range(struct userdata *u, char* inputdevice, int startsourceid, int endsourceid)
{
    pa_log("set_source_inputdevice_on_range: inputdevice:%s startsourceid:%d, endsourceid:%d", inputdevice, startsourceid, endsourceid);
    pa_source *destsource = NULL;
    struct sourceoutputnode *thelistitem = NULL;
    uint32_t physicalsourceid;
    int j;

    for(j = 0; j < ePhysicalSource_Count; j++) {
        if (strcmp(systemdependantphysicalsourcemap[j].physicalsourcename, inputdevice) == 0)
        {
            physicalsourceid = systemdependantphysicalsourcemap[j].physicalsourceidentifier;
            break;
        }
    }

    if( j == ePhysicalSource_Count)
    {
        pa_log_debug("can not find inputdevice %s in systemdependantphysicalsourcemap",inputdevice);
        return;
    }

    if (startsourceid >= 0 && endsourceid < eVirtualSource_Count)
    {
        for (int i  = startsourceid; i <= endsourceid; i++)
        {
            u->source_mapping_table[i].physicaldevice = physicalsourceid;
            destsource = pa_namereg_get(u->core,
                    systemdependantphysicalsourcemap[physicalsourceid].physicalsourcename, PA_NAMEREG_SOURCE);
            if (destsource == NULL) {
                pa_log_info("set_default_source_routing destsource %s is null",systemdependantphysicalsourcemap[physicalsourceid].physicalsourcename);
                return;
            }
            for (thelistitem = u->sourceoutputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
                if (thelistitem->virtualsourceid == i && ! pa_source_output_is_passthrough(thelistitem->sourceoutput)) {
                    pa_log_info("moving the virtual source %d to physical source-ID: %d", i, u->source_mapping_table[i].physicaldevice);
                    pa_source_output_move_to(thelistitem->sourceoutput, destsource, true);
                }
            }
        }
    }
    else
        pa_log_warn("set_source_inputdevice_on_range: start and end source are not in range");
}

static void set_default_source_routing(struct userdata *u, int startsourceid, int endsourceid)
{
    pa_log("set_default_source_routing: startsourceid:%d, endsourceid:%d", startsourceid, endsourceid);
    pa_source *destsource = NULL;
    struct sourceoutputnode *thelistitem = NULL;
    int physicalsourceid;

    if (startsourceid >= 0 && endsourceid < eVirtualSource_Count)
    {
        for (int i  = startsourceid; i <= endsourceid; i++)
        {
            u->source_mapping_table[i].physicaldevice = defaultsourcemappingtable[i].physicaldevice;
            physicalsourceid = u->source_mapping_table[i].physicaldevice;

            destsource =
                pa_namereg_get(u->core,
                        systemdependantphysicalsourcemap[physicalsourceid].physicalsourcename, PA_NAMEREG_SOURCE);

            /* walk the list of siource-inputs we know about and update their sources */
            for (thelistitem = u->sourceoutputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
                if ((int) thelistitem->virtualsourceid == i && !pa_source_output_is_passthrough(thelistitem->sourceoutput))
                {
                    pa_log_info("moving the virtual source %d to physical source-ID: %d", i, u->source_mapping_table[i].physicaldevice);
                    pa_source_output_move_to(thelistitem->sourceoutput, destsource, true);
                }
            }
        }
    }
    else
        pa_log_warn("set_default_source_routing: start and end source are not in range");
}

static void virtual_source_output_set_physical_source(int virtualsourceid, int physicalsourceid, struct userdata *u) {
    struct sourceoutputnode *thelistitem = NULL;
    pa_source *destsource = NULL;

    if (virtualsourceid >= 0 && virtualsourceid < eVirtualSource_Count
        && physicalsourceid >= 0 && physicalsourceid < ePhysicalSource_Count) {
        /* update the default mapping table, this causes any new streams created against the
         * virtual stream to be remapped against the requested physical source */

        u->source_mapping_table[virtualsourceid].physicaldevice = physicalsourceid;
        destsource =
            pa_namereg_get(u->core,
                           systemdependantphysicalsourcemap[physicalsourceid].physicalsourcename, PA_NAMEREG_SOURCE);

        /* walk the list of source-inputs we know about and update their sources */
        for (thelistitem = u->sourceoutputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
            if (thelistitem->virtualsourceid == virtualsourceid) {
                pa_source_output_move_to(thelistitem->sourceoutput, destsource, true);
            }
        }
    }
    else
        pa_log("virtual_source_input_set_physical_source: source ID out of range");
}

/* set the mute for all source-outputs associated with a virtual source,
 * sourceid - virtual source on which to set mute
 * mute - 0 unmuted, 1 muted. */
static void virtual_source_set_mute(int sourceid, int mute, struct userdata *u) {
    uint32_t idx, i;
    pa_source *source;

    for (i = 0; i < ePhysicalSource_Count; i++) {
        for (source = PA_SOURCE(pa_idxset_first(u->core->sources, &idx));
             source; source = PA_SOURCE(pa_idxset_next(u->core->sources, &idx))) {
            if (strcmp(systemdependantphysicalsourcemap[i].physicalsourcename, source->name) == 0) {
                pa_source_set_mute(source, mute, TRUE);
                pa_log_debug("source %s, mute %d\n", source->name, mute);
            }
        }
    }
}

static void virtual_sink_input_set_physical_sink(int virtualsinkid, int physicalsinkid, struct userdata *u) {
    struct sinkinputnode *thelistitem = NULL;
    pa_sink *destsink = NULL;

    if (virtualsinkid >= 0 && virtualsinkid < eVirtualSink_Count
        && physicalsinkid >= 0 && physicalsinkid < ePhysicalSink_Count) {
        /* update the default mapping table, this causes any new streams created against the
         * virtual stream to be remapped against the requested physical sink */

        u->sink_mapping_table[virtualsinkid].physicaldevice = physicalsinkid;
        destsink =
            pa_namereg_get(u->core, systemdependantphysicalsinkmap[physicalsinkid].physicalsinkname, PA_NAMEREG_SINK);

        /* walk the list of sink-inputs we know about and update their sinks */
        for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
            if ((int) thelistitem->virtualsinkid == virtualsinkid && !pa_sink_input_is_passthrough(thelistitem->sinkinput))
                pa_sink_input_move_to(thelistitem->sinkinput, destsink, true);
        }
    }
    else
        pa_log("virtual_sink_input_set_physical_sink: sink ID out of range");
}

static void virtual_sink_input_set_ramp_volume(int volumetoset, int volumetable, bool ramp_up, struct userdata *u) {
    struct sinkinputnode *thelistitem = NULL;
    struct pa_cvolume_ramp ramp_volume;
    int sinkid = enetflix;

    pa_assert(u);

    /* walk the list of sink-inputs we know about and update their volume */
    for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
        if(thelistitem->virtualsinkid == sinkid) {
            if (!pa_sink_input_is_passthrough(thelistitem->sinkinput)) {
                pa_usec_t msec = RAMP_DURATION_MSEC;
                u->sink_mapping_table[sinkid].volumetable = volumetable;
                pa_log_debug("Ramp volume we are setting is %u, %f db",
                             pa_sw_volume_from_dB(_mapPercentToPulseRamp
                                                  [volumetable][volumetoset]),
                             _mapPercentToPulseRamp[volumetable][volumetoset]);
                pa_cvolume_ramp_set(&ramp_volume, thelistitem->sinkinput->sample_spec.channels, PA_VOLUME_RAMP_TYPE_LINEAR, msec, pa_sw_volume_from_dB(_mapPercentToPulseRamp[volumetable][volumetoset]), ramp_up ? PA_VOLUME_FADE_IN : PA_VOLUME_FADE_OUT);
                pa_sink_input_set_volume_ramp(thelistitem->sinkinput, &ramp_volume, TRUE, TRUE);
            }
        }
    }
}

/* set the volume for all sink-inputs associated with a virtual sink,
 * sinkid - virtual sink on which to set volumes
 * volumetoset - 0..65535 gain setting for pulseaudio to use */

static void virtual_sink_input_set_volume(int sinkid, int volumetoset, int volumetable, struct userdata *u) {
    struct sinkinputnode *thelistitem = NULL;
    struct pa_cvolume cvolume;

    if (sinkid >= 0 && sinkid < eVirtualSink_Count) {
        /* set the default volume on new streams created on
         * this sink, update rules table for final ramped volume */

        /* walk the list of sink-inputs we know about and update their volume */
        for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
            if (thelistitem->virtualsinkid == sinkid) {
                if (!pa_sink_input_is_passthrough(thelistitem->sinkinput)) {
                    u->sink_mapping_table[sinkid].volumetable = volumetable;
                    pa_log_debug("volume we are setting is %u, %f db",
                             pa_sw_volume_from_dB(_mapPercentToPulseRamp
                                                  [volumetable][volumetoset]),
                             _mapPercentToPulseRamp[volumetable][volumetoset]);
                    if (volumetoset)
                        pa_cvolume_set(&cvolume,
                                   thelistitem->sinkinput->sample_spec.channels,
                                   pa_sw_volume_from_dB(_mapPercentToPulseRamp[volumetable]
                                                        [volumetoset]));
                    else
                        pa_cvolume_set(&cvolume, thelistitem->sinkinput->sample_spec.channels, 0);
                    //pa_sink_input_set_volume_with_ramping(thelistitem->sinkinput, &cvolume, TRUE, TRUE, 5 * PA_USEC_PER_MSEC);
                    pa_sink_input_set_volume(thelistitem->sinkinput, &cvolume, TRUE, TRUE);
                }
                else {
                    pa_log_debug("setting volume on Compress playback to %d",volumetoset);
#if HAVE_UCM
                    update_volume(volumetoset);
#endif
                }
            }
        }
        u->sink_mapping_table[sinkid].volume = volumetoset;
    }
    else
        pa_log("virtual_sink_input_set_volume: sink ID %d out of range", sinkid);
}

static void virtual_sink_input_set_volume_with_ramp(int sinkid, int volumetoset, int volumetable, struct userdata *u) {
    struct sinkinputnode *thelistitem = NULL;
    struct pa_cvolume cvolume, orig_cvolume;

    if (sinkid >= 0 && sinkid < eVirtualSink_Count) {
        /* set the default volume on new streams created on
         * this sink, update rules table for final ramped volume */

        /* walk the list of sink-inputs we know about and update their volume */
        for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
            if(thelistitem->virtualsinkid == sinkid) {
                if (!pa_sink_input_is_passthrough(thelistitem->sinkinput)) {
                    pa_usec_t msec;
                    u->sink_mapping_table[sinkid].volumetable = volumetable;
                    pa_log_debug("volume we are setting is %u, %f db",
                             pa_sw_volume_from_dB(_mapPercentToPulseRamp
                                                  [volumetable][volumetoset]),
                             _mapPercentToPulseRamp[volumetable][volumetoset]);
                    pa_cvolume_set(&cvolume,
                               thelistitem->sinkinput->sample_spec.channels,
                               pa_sw_volume_from_dB(_mapPercentToPulseRamp[volumetable]
                                                    [volumetoset]));

                    if (pa_cvolume_max(&cvolume) >=
                        pa_cvolume_max(pa_sink_input_get_volume(thelistitem->sinkinput, &orig_cvolume, TRUE)))
                        msec = PALM_UP_RAMP_MSEC;
                    else
                        msec = PALM_DOWN_RAMP_MSEC;

                    /* pa_sink_input_set_volume_with_ramping(thelistitem->sinkinput, &cvolume, TRUE, TRUE, msec * PA_USEC_PER_MSEC); */
                    pa_sink_input_set_volume(thelistitem->sinkinput, &cvolume, TRUE, TRUE);
                }
                else {
                    pa_log_debug("setting volume on Compress playback to %d",volumetoset);
#if HAVE_UCM
                    update_volume(volumetoset);
#endif
                }
            }
        }
        u->sink_mapping_table[sinkid].volume = volumetoset;
    }
    else
        pa_log("virtual_sink_input_set_volume: sink ID %d out of range", sinkid);
}

/* set the volume for all sink-inputs associated with a virtual sink,
 * sinkid - virtual sink on which to set volumes
 * setmuteval - (true : false) setting for pulseaudio to use */
static void virtual_sink_input_set_mute(int sinkid, int volumetoset, int volumetable, struct userdata *u) {
    struct sinkinputnode *thelistitem = NULL;
    struct pa_cvolume cvolume;

    if (sinkid >= 0 && sinkid < eVirtualSink_Count) {
        /* set the default volume on new streams created on
         * this sink, update rules table for final ramped volume */

        /* set the volume on sink_inputs with the virtual stream */

        u->sink_mapping_table[sinkid].volume = volumetoset;

        /* walk the list of sink-inputs we know about and update their volume */
        for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {
            if (thelistitem->virtualsinkid == sinkid) {
                if (!pa_sink_input_is_passthrough(thelistitem->sinkinput)) {
                    u->sink_mapping_table[sinkid].volumetable = volumetable;

                    pa_cvolume_set(&cvolume,
                               thelistitem->sinkinput->sample_spec.channels,
                               pa_sw_volume_from_dB(_mapPercentToPulseRamp[volumetable]
                                                    [volumetoset]));
                /*pa_sink_input_set_volume_with_ramping(thelistitem->sinkinput, &cvolume, TRUE, TRUE, 0); */
                }
                else {
                    pa_log_debug("setting volume on Compress playback to %d",volumetoset);
#if HAVE_UCM
                    update_volume(volumetoset);
#endif
                }
            }
        }
        u->sink_mapping_table[sinkid].volume = volumetoset;
    }
    else
        pa_log("virtual_sink_input_set_mute: sink ID %d out of range", sinkid);
}

static int sink_suspend_request(struct userdata *u) {
    struct sinkinputnode *thesinklistitem = NULL;
    struct sourceoutputnode *thesourcelistitem = NULL;

    for (thesinklistitem = u->sinkinputnodelist; thesinklistitem != NULL; thesinklistitem = thesinklistitem->next) {
        if (thesinklistitem->sinkinput->state == PA_SINK_INPUT_RUNNING) {
            pa_log("%s: sink input (%d) is active and running, close and report error",
                 __FUNCTION__, thesinklistitem->virtualsinkid);
            break;
        }
    }

    pa_sink_suspend_all(u->core, true, PA_SUSPEND_IDLE);

    for (thesourcelistitem = u->sourceoutputnodelist; thesourcelistitem != NULL;
         thesourcelistitem = thesourcelistitem->next) {
        if (thesourcelistitem->sourceoutput->state == PA_SOURCE_OUTPUT_RUNNING) {
            pa_log("%s: source output (%d) is active and running, close and report error",
                 __FUNCTION__, thesourcelistitem->virtualsourceid);
            break;
        }
    }
    pa_source_suspend_all(u->core, true, PA_SUSPEND_IDLE);

    return 0;
}

static int update_sample_spec(struct userdata *u, int rate) {
#if 0
    pa_sink *sink;
    pa_source *source;

    uint32_t idx, i, need_unsuspend = 0;
    pa_sample_spec sample_spec;

    for (i = 0; i < ePhysicalSink_Count; i++) {
        for (sink = PA_SINK(pa_idxset_first(u->core->sinks, &idx)); sink;
             sink = PA_SINK(pa_idxset_next(u->core->sinks, &idx))) {
            if (strcmp(systemdependantphysicalsinkmap[i].physicalsinkname, sink->name) == 0) {
                if (sink->update_sample_spec && sink->sample_spec.rate != rate) {
                    if (sink->state == PA_SINK_RUNNING) {
                        pa_log_info("Need to suspend then unsuspend device before changing sampling rate");
                        need_unsuspend = 1;
                    }
                    pa_sink_suspend(sink, 1, PA_SUSPEND_IDLE);

                    pa_log_info
                        ("update_sample_spec is available for sink %s, current sample_spec rate %d, changing to %d",
                         sink->name, sink->sample_spec.rate, rate);
                    sample_spec = sink->sample_spec;
                    sample_spec.rate = rate;
                    sink->update_sample_spec(sink, &sample_spec);

                    if (need_unsuspend == 1) {
                        pa_sink_suspend(sink, 0, PA_SUSPEND_IDLE);
                        need_unsuspend = 0;
                    }
                }
            }
        }
    }

    for (i = 0; i < ePhysicalSource_Count; i++) {
        for (source = PA_SOURCE(pa_idxset_first(u->core->sources, &idx));
             source; source = PA_SOURCE(pa_idxset_next(u->core->sources, &idx))) {
            if (strcmp(systemdependantphysicalsourcemap[i].physicalsourcename, source->name) == 0) {
                if (source->update_sample_spec && source->sample_spec.rate != rate) {
                    if (source->state == PA_SOURCE_RUNNING) {
                        pa_log_info("Need to suspend then unsuspend device before changing sampling rate");
                        need_unsuspend = 1;
                    }
                    pa_source_suspend(source, 1, PA_SUSPEND_IDLE);

                    pa_log_info
                        ("update_sample_spec is available for source %s, current sample_spec rate %d, changing to %d",
                         source->name, source->sample_spec.rate, rate);
                    sample_spec = source->sample_spec;
                    sample_spec.rate = rate;
                    source->update_sample_spec(source, &sample_spec);

                    if (need_unsuspend == 1) {
                        pa_source_suspend(source, 0, PA_SUSPEND_IDLE);
                        need_unsuspend = 0;
                    }
                }
            }
        }
    }
#endif
    return 0;
}

#if HAVE_BSA
static void load_Bluetooth_module(struct userdata *u)
{
    char *args = NULL;

    pa_assert(u != NULL);

    if (u->bt_connected) {
        pa_log_info("bsa a2dp module is already loaded");
        return;
    }

    u->bt_module = pa_module_load(u->core, "module-bsaa2dp-sink", args);

    if (args)
        pa_xfree(args);

    if (!u->bt_module) {
        pa_log("Error loading in module-bsaa2dp-sink");
        return;
    }
    u->bt_connected = true;
}

static void unload_BlueTooth_module(struct userdata *u)
{
    pa_assert(u);

    if (!u->bt_connected) {
        pa_log_info("module-bsaa2dp-sink is not loaded");
        return;
    }
    pa_assert(u->bt_module);

    pa_module_unload(u->bt_module, true);

    u->bt_module = NULL;
    u->bt_connected = false;
}
#endif

static void load_unicast_rtp_module(struct userdata *u)
{
    char *args = NULL;
    pa_assert(u != NULL);
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    memset(audiodbuf, 0, sizeof(audiodbuf));

/* Request for Unicast RTP
 * Client Provides Destination IP & Port (Optional)
 * Load RTP Module
 * Send Error To AudioD in case RTP Load fails
 */
    if (strcmp(u->connectionType,"unicast")== 0) {
        pa_log("[rtp loading begins for Unicast RTP] [AudioD sent] port = %u ip_addr = %s",
            u->connectionPort,u->destAddress) ;
        if(u->connectionPort < 1 || u->connectionPort > 0xFFFF) {
            args = pa_sprintf_malloc("source=%s destination_ip=%s","rtp.monitor", u->destAddress);
        } else {
            args = pa_sprintf_malloc("source=%s destination_ip=%s port=%d","rtp.monitor",
                u->destAddress, u->connectionPort);
        }
        pa_module_load(&u->rtp_module, u->core, "module-rtp-send", args);
    }

    if (args)
        pa_xfree(args);

    if (!u->rtp_module) {
        pa_log("Error loading in module-rtp-send");
        snprintf(audiodbuf, SIZE_MESG_TO_AUDIOD, "t %d %d %s %d", 0, 1, (char *)NULL, 0);
        if(-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
            pa_log("Failed to send message to audiod ");
        else
            pa_log("Error in Loading RTP Module message sent to audiod");
        return;
    }
}

static void load_alsa_source(struct userdata *u, int status)
{
    pa_assert(u);
    char *args = NULL;

    pa_log("[alsa source loading begins for Mic Recording] [AudioD sent] cardno = %d capture device number = %d",\
            u->external_soundcard_number, u->external_device_number);

    if (!u->IsUsbMICConnected)
    {

        if (u->external_soundcard_number >= 0 && (1 == status)) {
            args = pa_sprintf_malloc("device=hw:%d,%d mmap=0 source_name=%s fragment_size=4096 tsched=0",\
                u->external_soundcard_number, u->external_device_number, u->deviceName);
        }
        else
            return;

        pa_module_load(&u->alsa_source, u->core, "module-alsa-source", args);

        if (!u->alsa_source)
            pa_log("Error loading in module-alsa-source");
        else
        {
            pa_log_info("module-alsa-source with source_name%s loaded successfully", u->deviceName);
            pa_log_info("module is loaded with index %u", u->alsa_source->index);
            u->externalMICCardNumber = u->external_soundcard_number;
            u->IsUsbMICConnected = true;
        }
    }
    else
        pa_log("Error loading in module-alsa-source, Already Loaded");

    if (args)
        pa_xfree(args);

    pa_log_info("module-alsa-source loaded");
}

static void load_alsa_sink(struct userdata *u, int status)
{
    pa_assert(u);

    int sink = 0;
    int i = 0;
    char *args = NULL;

    pa_log("[alsa sink loading begins for Usb Speaker routing] [AudioD sent] cardno = %d playback device number = %d",\
            u->external_soundcard_number, u->external_device_number);

    if (!u->IsUsbConnected)
    {
        args = pa_sprintf_malloc("device=hw:%d,%d mmap=0 sink_name=%s fragment_size=4096 tsched=0",\
                u->external_soundcard_number, u->external_device_number, u->deviceName);
        /*Loading alsa sink with sink_name*/
        pa_module_load(&u->default_alsa_sink, u->core, "module-alsa-sink", args);
        if (NULL == u->default_alsa_sink)
            pa_log("Error loading in module-alsa-sink with sink_name%s", u->deviceName);
        else
        {
            pa_log_info("module-alsa-sink with sink_name%s loaded successfully", u->deviceName);
            u->externalSoundCardNumber = u->external_soundcard_number;
            u->IsUsbConnected = true;

        }
    }
    if (args)
        pa_xfree(args);

    pa_log_info("module-alsa-sink loaded");
}

static void unload_alsa_source(struct userdata *u, int status)
{
    pa_source* usb_source;
    pa_assert(u);

    pa_log("[alsa source unloading] [AudioD sent] cardno = %d capture device number = %d",\
            u->external_soundcard_number, u->external_device_number);

    if( (usb_source = pa_namereg_get(u->core,"usb_input",PA_NAMEREG_SOURCE) ) == NULL)
    {
        pa_log_info("module-alsa-source Already unloaded");
        u->IsUsbMICConnected = false;
        u->externalMICCardNumber = -1;
        u->alsa_source = NULL;
        return;
    }

    if (u->IsUsbMICConnected && u->externalMICCardNumber == u->external_soundcard_number)
    {
        pa_log_info("Un-loading alsa source");
        pa_module_unload(u->alsa_source, true);
        u->IsUsbMICConnected = false;
        pa_log_info("Set USB physical source as null source");
        u->externalMICCardNumber = -1;
        u->alsa_source = NULL;
    }
    pa_log_info("module-alsa-source un-loaded");
}

static void unload_alsa_sink(struct userdata *u, int status)
{
    pa_assert(u);
    int i = 0;
    pa_sink* usb_sink;
    pa_log("[alsa sink unloading begins for Usb haedset routing] [AudioD sent] cardno = %d playback device number = %d",\
            u->external_soundcard_number, u->external_device_number);
    if((usb_sink = pa_namereg_get(u->core,"usb_output",PA_NAMEREG_SINK) ) == NULL)
    {
        pa_log_info("module-alsa-sink Already un-loaded");
        u->IsUsbConnected = false;
        u->externalSoundCardNumber = -1;
        u->default_alsa_sink = NULL;
        return;
    }

    if (u->IsUsbConnected && u->externalSoundCardNumber == u->external_soundcard_number)
    {
        pa_log_info("Un-loading alsa sink");
        pa_module_unload(u->default_alsa_sink, TRUE);

        u->IsUsbConnected = false;
        pa_log_info("Set USB physical sink as null sink");
        u->externalSoundCardNumber = -1;
        u->default_alsa_sink = NULL;
    }
    pa_log_info("module-alsa-sink un-loaded");
}

static void load_multicast_rtp_module(struct userdata *u)
{
    char *args = NULL;
    pa_assert(u != NULL);
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    memset(audiodbuf, 0, sizeof(audiodbuf));

/* Request for Multicast RTP
 * Client Provides Destination IP(Optional) & Port (Optional)
 * Load RTP Module
 * Send Error To AudioD in case RTP Load fails
 */
    if (strcmp(u->connectionType,"multicast")== 0) {
        pa_log("[rtp loading begins for Multicast RTP] [AudioD sent] port = %u ip_addr = %s",
            u->connectionPort,u->destAddress) ;
        if(u->connectionPort < 1 || u->connectionPort > 0xFFFF) {
            if(strcmp(u->destAddress,"default") == 0) {
                args = pa_sprintf_malloc("source=%s","rtp.monitor");
            } else {
                args = pa_sprintf_malloc("source=%s destination_ip=%s","rtp.monitor", u->destAddress);
            }
        } else {
            if(strcmp(u->destAddress,"default") == 0){
                args = pa_sprintf_malloc("source=%s port=%d","rtp.monitor", u->connectionPort);
            } else {
                args = pa_sprintf_malloc("source=%s destination_ip=%s port=%d","rtp.monitor",
                    u->destAddress, u->connectionPort);
            }
        }
        pa_module_load(&u->rtp_module, u->core, "module-rtp-send", args);
    }

    if (args)
        pa_xfree(args);

    if (!u->rtp_module) {
        pa_log("Error loading in module-rtp-send");
        snprintf(audiodbuf, SIZE_MESG_TO_AUDIOD, "t %d %d %s %d", 0, 1, (char *)NULL, 0);
        if(-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
            pa_log("Failed to send message to audiod ");
        else
            pa_log("Error in Loading RTP Module message sent to audiod");
        return;
    }
}

static void unload_rtp_module(struct userdata *u)
{
    pa_assert(u);
    pa_assert(u->rtp_module);

    pa_module_unload(u->rtp_module, true);
    pa_log_info("module-rtp-sink unloaded");
    u->rtp_module = NULL;
}

void send_rtp_connection_data_to_audiod(char *ip,char *port,struct userdata *u) {
    pa_assert(ip);
    pa_assert(port);
    int port_value = atoi(port) ;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    memset(audiodbuf, 0, sizeof(audiodbuf));

    pa_log("[send_rtp_connection_data_to_audiod] ip = %s port = %d",ip,port_value);
    snprintf(audiodbuf, SIZE_MESG_TO_AUDIOD, "t %d %d %s %d", 0 , 0, ip, port_value);
    if(-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
        pa_log("Failed to send message to audiod ");
    else
        pa_log("Message sent to audiod");
}

static pa_hook_result_t source_state_changed_cb(pa_core *c, pa_object *o, struct userdata *u)
{

    pa_source *s;
    pa_source_state_t state;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];

    pa_assert(c);
    pa_assert(o);
    pa_assert(u);

    s = PA_SOURCE(o);

    if(!pa_source_isinstance(o))
        return PA_HOOK_OK;

    if(pa_streq(s->name, "pcm_input"))
        state = s->state;
    else
        return PA_HOOK_OK;

    u->current_state = state;
    if(state == PA_SOURCE_RUNNING && u->device_open == false) {
        u->device_open = true;
        sprintf(audiodbuf, "H %d %d", 1, 1);
    } else {
        if( u->device_open == true && state == PA_SOURCE_SUSPENDED &&
             u->pout_previous_state == PA_SINK_SUSPENDED &&
             u->compress_previous_state == PA_SINK_SUSPENDED &&
             u->bsa_previous_state == PA_SINK_SUSPENDED) {
             u->device_open = false;
            sprintf(audiodbuf, "R %d %d", 1, 1);
        } else
            return PA_HOOK_OK;
    }

    if(u->connectionactive && u->connev != NULL) {
        if(-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
            pa_log("Failed to send message to audiod to hold/release wakelock");
        else
            pa_log("Message sent to audiod to %s wakelock", u->device_open?"hold":"release");
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_state_changed_cb(pa_core *c, pa_object *o, struct userdata *u)
{

    pa_sink *s;
    pa_sink_state_t state ;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];

    pa_assert(c);
    pa_assert(o);
    pa_assert(u);

    s = PA_SINK(o);
    state = s->state;

    if(!pa_sink_isinstance(o))
        return PA_HOOK_OK;

    if(pa_streq(s->name, "pcm_output")) {
        u->pout_previous_state = state;
        if(state == PA_SINK_RUNNING && u->device_open == false) {
            u->device_open = true;
            sprintf(audiodbuf, "H %d %d", 1, 1);
        } else {
            if(u->device_open == true &&
               state == PA_SINK_SUSPENDED &&
               u->current_state == PA_SOURCE_SUSPENDED &&
               u->compress_previous_state == PA_SINK_SUSPENDED &&
               u->bsa_previous_state == PA_SINK_SUSPENDED) {
               u->device_open = false;
                sprintf(audiodbuf, "R %d %d", 1, 1);
            } else
                return PA_HOOK_OK;
        }
    }
    else if(pa_streq(s->name, "tinycompress")) {
        u->compress_previous_state = state;
        if(state == PA_SINK_RUNNING && u->device_open == false) {
            u->device_open = true;
            sprintf(audiodbuf, "H %d %d", 1, 1);
        } else {
            if(u->device_open == true &&
                state == PA_SINK_SUSPENDED &&
                u->current_state == PA_SOURCE_SUSPENDED &&
                u->pout_previous_state == PA_SINK_SUSPENDED &&
                u->bsa_previous_state == PA_SINK_SUSPENDED) {
                u->device_open = false;
                sprintf(audiodbuf, "R %d %d", 1, 1);
            } else
                return PA_HOOK_OK;
        }
    }
    else if(pa_streq(s->name, "bsaa2dp")) {
        u->bsa_previous_state = state;
        if(state == PA_SINK_RUNNING && u->device_open == false) {
            u->device_open = true;
            sprintf(audiodbuf, "H %d %d", 1, 1);
        } else {
            if(u->device_open == true &&
               state == PA_SINK_SUSPENDED &&
               u->current_state == PA_SOURCE_SUSPENDED &&
               u->compress_previous_state == PA_SINK_SUSPENDED &&
               u->pout_previous_state == PA_SINK_SUSPENDED) {
               u->device_open = false;
               sprintf(audiodbuf, "R %d %d", 1, 1);
            } else
                return PA_HOOK_OK;
        }
    }
    else {
        return PA_HOOK_OK;
    }

    if(u->connectionactive && u->connev != NULL) {
        if(-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
            pa_log("Failed to send message to audiod to hold/release wakelock");
        else
            pa_log("Message sent to audiod to %s wakelock", u->device_open?"hold":"release");
    }

    return PA_HOOK_OK;
}

/* Parse a message sent from audiod and invoke
 * requested changes in pulseaudio
 */
static void parse_message(char *msgbuf, int bufsize, struct userdata *u) {
    char cmd;                                           /* all commands must start with this */
    int sinkid ;                /* and they must have a sink to operate on */
#if HAVE_UCM
    bool voLTE = FALSE;
    int BTDeviceType = 0;
    bool NREC = FALSE;
#endif
    pa_log_info("parse_message: %s", msgbuf);

    pa_assert(u);
    pa_assert(msgbuf);
    pa_assert(bufsize > 0);

    /* pick it apart with sscanf */
    if (1 == sscanf(msgbuf, "%c", &cmd)) {
        int parm1, parm2, parm3;

        if (isalpha(cmd))
            cmd = tolower(cmd);

        switch (cmd) {
        case 'a':
            {
                char inputdevice[DEVICE_NAME_LENGTH];
                int startsourceid;
                int endsourceid;
                if (4 == sscanf(msgbuf, "%c %d %d %s", &cmd, &startsourceid, &endsourceid, inputdevice))
                {
                    pa_log_info("received source routing for inputdevice:%s startsourceid:%d,\
                            inputdevice:%d", inputdevice, startsourceid, endsourceid);
                    set_source_inputdevice_on_range(u, inputdevice, startsourceid, endsourceid);
                }
                else
                    pa_log_warn("received source routing for inputdevice with invalid params");
            }
            break;

        case 'd':
            /* redirect -  D <virtualsink> <physicalsink> */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &parm1, &parm2, &parm3)) {
                /* walk list of sink-inputs on this stream and set
                 * their output sink */
                virtual_sink_input_set_physical_sink(parm1, parm2, u);
                pa_log_info("parse_message: stream redirect command received,\
                           virtual sink is %d, requested physical sink to redirect to is %d", parm1, parm2);
            }
            break;

        case 'e':
            /* redirect -  E <virtualsink> <physicalsink> */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &parm1, &parm2, &parm3)) {
                /* walk list of sink-inputs on this stream and set
                 * their output sink */
                virtual_source_output_set_physical_source(parm1, parm2, u);
                pa_log_info("parse_message: stream redirect command received,\
                            virtual source is %d, requested physical source to redirect to is %d", parm1, parm2);
            }
            break;

        case 'v':
            /* volume -  V <sink> <value 0 : 65535> */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                /* walk list of sink-inputs on this stream and set
                 * their volume */
                parm2 = CLAMP_VOLUME_TABLE(parm2);
                virtual_sink_input_set_volume(sinkid, parm1, parm2, u);
                pa_log_info("parse_message: volume command received, sink is %d, requested volume is %d, headphones:%d",
                            sinkid, parm1, parm2);
            }

            break;

        case 'b':
            /* volume -  B  <value 0 : 65535> ramup/down */
            /* walk list of sink-inputs on this stream and set their volume */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &parm1, &parm2, &parm3)) {
                parm2 = CLAMP_VOLUME_TABLE(parm2);
                virtual_sink_input_set_ramp_volume(parm1, parm2, !!parm3, u);
                pa_log_info("parse_message: Fade command received, requested volume is %d, headphones:%d, fadeIn:%d",
                            parm1, parm2, parm3);
            }

            break;

        case 'h':
            /* mute source -  H <source> <mute 0 : 1> */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                /* walk list of sink-inputs on this stream and set
                 * their volume */
                virtual_source_set_mute(sinkid, parm1, u);
                pa_log_info("parse_message: source mute command received, source is %d, mute %d", sinkid, parm1);
            }

            break;

        case 'm':
            /* mute -  M <sink> <state true : false> */

            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                /* walk list of sink-inputs on this stream and set
                 * their mute value */
                parm2 = CLAMP_VOLUME_TABLE(parm2);
                virtual_sink_input_set_mute(sinkid, parm1, parm2, u);
                pa_log_info
                    ("parse_message: mute command received, sink is %d, value is %d, headphones:%d",
                     sinkid, parm1, parm2);
            }
            break;

        case 'r':

            /* ramp -  R <sink> <volume 0 : 100> */
            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                /* walk list of sink-inputs on this stream and set
                 * their volumeramp parms
                 */
                parm2 = CLAMP_VOLUME_TABLE(parm2);
                virtual_sink_input_set_volume_with_ramp(sinkid, parm1, parm2, u);
                pa_log_info
                    ("parse_message: ramp command received, sink is %d, volumetoset:%d, headphones:%d",
                     sinkid, parm1, parm2);
            }
            break;

        case 's':

            /* suspend -  s */
            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                /* System is going to sleep, so suspend active modules */
                if (-1 == sink_suspend_request(u))
                    pa_log_info("suspend request failed: %s", strerror(errno));
                pa_log_info("parse_message: suspend command received");
            }
            break;

        case 'x':

            /* update sample rate -  x */
            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &sinkid, &parm1, &parm2)) {
                if (-1 == update_sample_spec(u, parm1))
                    pa_log_info("suspend request failed: %s", strerror(errno));
                pa_log_info("parse_message: update sample spec command received");
            }
            break;
#if HAVE_BSA
        case 'l':

            pa_log_info ("parse_message:received command l FOR BLUETOOTH module");
            if (4 == sscanf(msgbuf, "%c %d %s %s", &cmd, &sinkid, u->btAddress, u->btProfile)) {
                if (strcmp(u->btProfile, "a2dp") == 0)
                    load_Bluetooth_module(u);
                else if (strcmp(u->btProfile, "HFP-AG") == 0) {
                    pa_set_routing_HFP_call(true);
                }
            }
            break;

        case 'u':

            pa_log_info ("parse_message:received command u FOR BLUETOOTH module");
            if (3 == sscanf(msgbuf, "%c %d %s", &cmd, &sinkid, u->btProfile)) {
                if (strcmp(u->btProfile, "a2dp") == 0)
                    unload_BlueTooth_module(u);
                else if (strcmp(u->btProfile, "HFP-AG") == 0) {
                    pa_set_routing_HFP_call(false);
                }
            }
            break;
#endif

        case 't':
            pa_log_info("received rtp load cmd from Audiod");
            if (5 == sscanf(msgbuf, "%c %d %10s %28s %u", &cmd, &sinkid, u->connectionType, u->destAddress,&u->connectionPort)) {
                pa_log_info ("parse_message:received command t FOR RTP module port = %lu",u->connectionPort);
                if(strcmp(u->connectionType,"unicast") == 0)
                    load_unicast_rtp_module(u);
                else if (strcmp(u->connectionType,"multicast") == 0)
                    load_multicast_rtp_module(u);
            }
            break;

        case 'g':
            pa_log_info ("received unload command for RTP module from AudioD");
            unload_rtp_module(u);
            break;

        case 'j':
            {
                int status = 0;
                if (5 == sscanf(msgbuf, "%c %d %d %d %s", &cmd, &u->external_soundcard_number, &u->external_device_number, &status, u->deviceName))
                {
                    pa_log_info("received mic recording cmd from Audiod");
                    if (1 == status)
                        load_alsa_source(u, status);
                    else
                        unload_alsa_source(u, status);
                }
            }
            break;
        case 'w':
            {
                int status = 0;

                if (5 == sscanf(msgbuf, "%c %d %d %d %s", &cmd, &u->external_soundcard_number, &u->external_device_number, &status, u->deviceName))
                {
                    pa_log_info("received usb headset routing cmd from Audiod");
                    pa_log_info("USB ALSA SINK is not supported");
                    /*
                    if (1 == status) {
                        load_alsa_sink(u, status);
                    }
                    else
                        unload_alsa_sink(u, status);
                    */
                }

            }
            break;
#if HAVE_UCM
        case 'p':
            if (4 == sscanf(msgbuf, "%c %d %s %d", &cmd, &voLTE, u->scenario, &BTDeviceType)) {
                pa_log ("parse_message:received command 'p' for routing: scenario = %s, voLTE(%d), BTdeviceType(%d)", u->scenario, voLTE,BTDeviceType);
                pa_set_routing_voice_call (u->scenario, voLTE, BTDeviceType);
            }
            break;

        case 'q':
            if (4 == sscanf(msgbuf, "%c %d %s %d", &cmd, &voLTE, u->scenario, &parm2)) {
                pa_log ("parse_message:received command 'q' for media routing");
                pa_set_routing_media(u->scenario);
            }
            break;

        case 't':
            if (4 == sscanf(msgbuf, "%c %d %s %d", &cmd, &parm1, u->scenario, &parm2)) {
                pa_log_debug("parse_message:recievd command 't' for loopback test");
                loopback_set_parameters(u->scenario);
            }
            break;
#endif

#if HAVE_UCM
        case 'a':
            if (3 == sscanf(msgbuf, "%c %d %d", &cmd, &sinkid, &parm1))
            {
                pa_log_info("parse_message:recievd command 'a' for voice call volume : %d",parm1);
#if HAVE_BSA
                if (pa_streq(u->btProfile, "HFP-AG"))
                    update_hf_call_voiceOrMic_volume(parm1, true);
                else
#endif
                    update_call_voice_volume(parm1);
            }
            break;

        case 'c':
            if (3 == sscanf(msgbuf, "%c %d %d", &cmd, &sinkid, &parm1))
            {
                pa_log_info("parse_message:recievd command 'c' for voice Mic volume : %d",parm1);
#if HAVE_BSA
                if (pa_streq(u->btProfile, "HFP-AG"))
                    update_hf_call_voiceOrMic_volume(parm1, false);
                else
#endif
                    update_phoneMIC_volume(parm1);
            }
            break;

        case 'z':
            if (4 == sscanf(msgbuf, "%c %d %d %s", &cmd, &parm1, &NREC, u->scenario)) {
                pa_log_info("parse_message:recievd command 'z' for NREC(%d)",NREC);
                pa_set_NREC(NREC, u->scenario);
            }
            break;

        case 'y':
            if (4 == sscanf(msgbuf, "%c %d %d %d", &cmd, &parm1, &parm2, &parm3)) {
                pa_log_info("parse_message: 'y' command received (BTDevice %s and hfpstatus->%d)",parm1?"wideband":"narrowband",parm2);
                pa_set_BTdevice_type(parm1, parm2);
            }
            break;
#endif
        case '3':
            {
                int startSourceId;
                int endSourceId;
                if (3 == sscanf(msgbuf, "%c %d %d", &cmd, &startSourceId, &endSourceId))
                {
                    pa_log_info("received default source routing for startSourceId:%d endSourceId:%d",\
                            startSourceId, endSourceId);
                    set_default_source_routing(u, startSourceId, endSourceId);
                }
            }
            break;

        default:
            pa_log_info("parse_message: unknown command received");
            break;
        }
    }
}


/* pa_io_event_cb_t - IO event handler for socket,
 * this will create connections and assign an
 * appropriate IO event handler */

static void handle_io_event_socket(pa_mainloop_api * ea, pa_io_event * e, int fd, pa_io_event_flags_t events, void *userdata) {
    struct userdata *u = userdata;
    int itslen;
    int sink;
    int source;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];

    pa_assert(u);
    pa_assert(fd == u->sockfd);

    itslen = SUN_LEN(&u->name);

    if (events & PA_IO_EVENT_NULL) {
        pa_log_info("handle_io_event_socket PA_IO_EVENT_NULL received");
    }
    if (events & PA_IO_EVENT_INPUT) {
        /* do we have a connection on our socket yet? */
        if (-1 == u->newsockfd) {
            if (-1 == (u->newsockfd = accept(u->sockfd, &(u->name), &itslen))) {
                pa_log_info("handle_io_event_socket could not create new connection on socket:%s", strerror(errno));
            }
            else {
                /* create new io handler to deal with data send to this connection */
                u->connev =
                    u->core->mainloop->io_new(u->core->mainloop, u->newsockfd,
                                              PA_IO_EVENT_INPUT |
                                              PA_IO_EVENT_HANGUP | PA_IO_EVENT_ERROR, handle_io_event_connection, u);
                u->connectionactive = true; /* flag that we have an active connection */

                /* Tell audiod how many sink of each category is opened */
                for (sink = eVirtualSink_First; sink <= eVirtualSink_Last; sink++) {
                    if (u->audiod_sink_input_opened[sink] > 0) {
                        sprintf(audiodbuf, "O %d %d", sink, u->audiod_sink_input_opened[sink]);
                        if (-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
                            pa_log("handle_io_event_socket: send failed: %s", strerror(errno));
                        else
                            pa_log_info
                                ("handle_io_event_socket: stream count for sink %d (%d)",
                                 sink, u->audiod_sink_input_opened[sink]);
                    }
                }

                /* Tell audiod how many source of each category is opened */
                for (source = eVirtualSource_First; source <= eVirtualSource_Last; source++) {
                    if (u->audiod_source_output_opened[source] > 0) {
                        sprintf(audiodbuf, "I %d %d", source, u->audiod_source_output_opened[source]);
                        if (-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
                            pa_log("handle_io_event_socket: send failed: %s", strerror(errno));
                        else
                            pa_log_info
                                ("handle_io_event_socket: stream count for source %d (%d)",
                                 source, u->audiod_source_output_opened[source]);
                    }
                }
            }
        }
        else
            pa_log("handle_io_event_socket could not create new connection on socket");
    }
}

/* pa_io_event_cb_t - IO event handler for socket
 * connections.  We enforce a single connection to the
 * client (audiod).  This routine will createlisten on
 * the socket and parse and act upon messages sent to
 * the socket connection */
static void handle_io_event_connection(pa_mainloop_api * ea, pa_io_event * e, int fd, pa_io_event_flags_t events, void *userdata) {
    struct userdata *u = userdata;
    char buf[SIZE_MESG_TO_PULSE];
    int bytesread;

    pa_assert(u);
    pa_assert(fd == u->newsockfd);

    if (events & PA_IO_EVENT_NULL) {
        pa_log_info("handle_io_event_connection PA_IO_EVENT_NULL received");
    }
    if (events & PA_IO_EVENT_INPUT) {
        if (-1 == (bytesread = recv(u->newsockfd, buf, SIZE_MESG_TO_PULSE, 0))) {
            pa_log_info("handle_io_event_connection Error in recv (%d): %s ", errno, strerror(errno));
        }
        else {
            if (bytesread != 0) { /* the socket connection will return zero bytes on EOF */
                parse_message(buf, SIZE_MESG_TO_PULSE, u);
            }
        }
    }
    if (events & PA_IO_EVENT_OUTPUT) {
        pa_log_info("handle_io_event_connection PA_IO_EVENT_OUTPUT received");
    }
    if (events & PA_IO_EVENT_HANGUP) {
        pa_log_info("handle_io_event_connection PA_IO_EVENT_HANGUP received");
        pa_log_info("handle_io_event_connection Socket is being closed");
        /* remove ourselves from the IO list on the main loop */
        u->core->mainloop->io_free(u->connev);

        /* tear down the connection */
        if (-1 == shutdown(u->newsockfd, SHUT_RDWR)) {
            pa_log_info("Error in shutdown (%d):%s", errno, strerror(errno));
        }
        if (-1 == close(u->newsockfd)) {
            pa_log_info("Error in close (%d):%s", errno, strerror(errno));
        }

        /* reset vars in userdata, allows another connection to be rebuilt */
        u->connectionactive = false;
        u->connev = NULL;
        u->newsockfd = -1;
    }
    if (events & PA_IO_EVENT_ERROR)
        pa_log_info("handle_io_event_connection PA_IO_EVENT_ERROR received");
}

static int make_socket (struct userdata *u) {

    int path_len;

    u->sockfd = -1;
    u->newsockfd = -1;          /* set to -1 to indicate no connection */

    /* create a socket for ipc with audiod the policy manager */
    path_len = strlen(PALMAUDIO_SOCK_NAME);
    _MEM_ZERO(u->name);
    u->name.sun_family = AF_UNIX;

    u->name.sun_path[0] = '\0'; /* this is what says "use abstract" */
    path_len++;                 /* Account for the extra nul byte added to the start of sun_path */

    if (path_len > _MAX_NAME_LEN) {
        pa_log("%s: Path name is too long '%s'\n", __FUNCTION__, strerror(errno));
    }

    strncpy(&u->name.sun_path[1], PALMAUDIO_SOCK_NAME, path_len);

    /* build the socket */
    if (-1 == (u->sockfd = socket(AF_UNIX, SOCK_STREAM, 0))) {
        pa_log("Error in socket (%d) ", errno);
        goto fail;
    }

    /* bind it to a name */
    if (-1 ==
        bind(u->sockfd, (struct sockaddr *) &(u->name), _NAME_STRUCT_OFFSET(struct sockaddr_un, sun_path) + path_len)) {
        pa_log("Error in bind (%d) ", errno);
        goto fail;
    }

    if (-1 == listen(u->sockfd, 5)) {
        pa_log("Error in listen (%d) ", errno);
        goto fail;
    }

    u->connectionactive = FALSE;
    u->sockev = NULL;
    u->connev = NULL;

    /* register an IO event handler for the socket, deal
     * with new connections in this handler */
    u->sockev =
        u->core->mainloop->io_new(u->core->mainloop, u->sockfd,
                                  PA_IO_EVENT_INPUT | PA_IO_EVENT_HANGUP |
                                  PA_IO_EVENT_ERROR, handle_io_event_socket, u);
    return 0;
fail:
    return -1;
}

static void connect_to_hooks(struct userdata *u) {

    pa_assert(u);

    /* bit early than module-stream-restore:
     * module-stream-restore will try to set the sink if the stream doesn't comes with device set
     * Let palm-policy do the routing before module-stream-restore
     */
    u->sink_input_new_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_NEW],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_sink_input_new_hook_callback, u);

    u->sink_input_fixate_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_sink_input_fixate_hook_callback, u);

    u->source_output_new_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_NEW],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_source_output_new_hook_callback, u);

    u->source_output_fixate_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_FIXATE],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_source_output_fixate_hook_callback, u);

    u->source_output_put_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_PUT],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_source_output_put_hook_callback, u);

    u->source_output_state_changed_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_STATE_CHANGED], PA_HOOK_EARLY - 10, (pa_hook_cb_t)
                        route_source_output_state_changed_hook_callback, u);

    u->sink_input_put_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_PUT],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_sink_input_put_hook_callback, u);

    u->sink_input_unlink_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_UNLINK],
                        PA_HOOK_EARLY - 10, (pa_hook_cb_t) route_sink_input_unlink_hook_callback, u);

    u->source_output_unlink_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_OUTPUT_UNLINK],
                        PA_HOOK_EARLY, (pa_hook_cb_t) route_source_output_unlink_hook_callback, u);

    u->sink_input_state_changed_hook_slot =
        pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_STATE_CHANGED], PA_HOOK_EARLY - 10, (pa_hook_cb_t)
                        route_sink_input_state_changed_hook_callback, u);

    u->sink_state_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED], PA_HOOK_EARLY,
                        (pa_hook_cb_t)route_sink_state_changed_hook_callback, u);

    u->sink_input_move_finish = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_INPUT_MOVE_FINISH], PA_HOOK_EARLY,
                        (pa_hook_cb_t)route_sink_input_move_finish_cb, u);

    u->sink_new = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_EARLY,
                        (pa_hook_cb_t)route_sink_put_cb, u);

    u->sink_unlink = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY,
                        (pa_hook_cb_t)route_sink_unlink_cb, u);

    u->source_state_changed_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SOURCE_STATE_CHANGED],
                      PA_HOOK_EARLY+10, (pa_hook_cb_t)source_state_changed_cb, u);


    u->module_unload_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_MODULE_UNLINK],
                      PA_HOOK_EARLY, (pa_hook_cb_t)module_unload_subscription_callback, u);

    u->module_load_hook_slot = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_MODULE_NEW],
                      PA_HOOK_EARLY, (pa_hook_cb_t)module_load_subscription_callback, u);


    u->sink_state_changed_hook = pa_hook_connect(&u->core->hooks[PA_CORE_HOOK_SINK_STATE_CHANGED],
                      PA_HOOK_EARLY, (pa_hook_cb_t)sink_state_changed_cb, u);
}

static void disconnect_hooks(struct userdata *u) {

    pa_assert(u);

    if(u->sink_input_new_hook_slot)
        pa_hook_slot_free(u->sink_input_new_hook_slot);

    if (u->sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->sink_input_fixate_hook_slot);

    if (u->sink_input_put_hook_slot)
        pa_hook_slot_free(u->sink_input_put_hook_slot);

    if (u->sink_input_state_changed_hook_slot)
        pa_hook_slot_free(u->sink_input_state_changed_hook_slot);

    if (u->sink_input_unlink_hook_slot)
        pa_hook_slot_free(u->sink_input_unlink_hook_slot);

    if (u->source_output_new_hook_slot)
        pa_hook_slot_free(u->source_output_new_hook_slot);

    if (u->source_output_fixate_hook_slot)
        pa_hook_slot_free(u->source_output_fixate_hook_slot);

    if (u->source_output_put_hook_slot)
        pa_hook_slot_free(u->source_output_put_hook_slot);

    if (u->source_output_state_changed_hook_slot)
        pa_hook_slot_free(u->source_output_state_changed_hook_slot);

    if (u->source_output_unlink_hook_slot)
        pa_hook_slot_free(u->source_output_unlink_hook_slot);

    if (u->sink_state_changed_hook_slot)
        pa_hook_slot_free(u->sink_state_changed_hook_slot);

    if (u->sink_input_move_finish)
        pa_hook_slot_free(u->sink_input_move_finish);

    if (u->sink_new)
        pa_hook_slot_free(u->sink_new);

    if (u->sink_unlink)
        pa_hook_slot_free(u->sink_unlink);

    if(u->sink_state_changed_hook)
        pa_hook_slot_free(u->sink_state_changed_hook);

    if(u->module_unload_hook_slot)
        pa_hook_slot_free(u->module_unload_hook_slot);

    if(u->module_load_hook_slot)
        pa_hook_slot_free(u->module_load_hook_slot);

    if(u->source_state_changed_hook_slot)
        pa_hook_slot_free(u->source_state_changed_hook_slot);
}

/* entry point for the module*/
int pa__init(pa_module * m) {
    struct userdata *u = NULL;
    int i;

    pa_assert(m);
    u = pa_xnew(struct userdata, 1);

    u->core = m->core;
    u->module = m;
    m->userdata = u;

    PA_LLIST_HEAD_INIT(struct sinkinputnode, u->sinkinputnodelist);
    PA_LLIST_HEAD_INIT(struct sourceoutputnode, u->sourceoutputnodelist);

    connect_to_hooks(u);

    /* copy the default sink mapping */
    for (i = 0; i < eVirtualSink_Count; i++) {
        u->sink_mapping_table[i].virtualdevice = defaultsinkmappingtable[i].virtualdevice;
        u->sink_mapping_table[i].physicaldevice = defaultsinkmappingtable[i].physicaldevice;
        u->sink_mapping_table[i].volume = defaultsinkmappingtable[i].volume;
        u->sink_mapping_table[i].ismuted = defaultsinkmappingtable[i].ismuted;
        u->sink_mapping_table[i].volumetable = defaultsinkmappingtable[i].volumetable;

        // Clear audiod sink opened count
        u->audiod_sink_input_opened[i] = 0;
    }
    u->n_sink_input_opened = 0;

    /* copy the default source mapping */
    for (i = 0; i < eVirtualSource_Count; i++) {
        u->source_mapping_table[i].virtualdevice = defaultsourcemappingtable[i].virtualdevice;
        u->source_mapping_table[i].physicaldevice = defaultsourcemappingtable[i].physicaldevice;
        u->source_mapping_table[i].volume = defaultsourcemappingtable[i].volume;
        u->source_mapping_table[i].ismuted = defaultsourcemappingtable[i].ismuted;
        u->source_mapping_table[i].volumetable = defaultsourcemappingtable[i].volumetable;

        // Clear audiod sink opened count
        u->audiod_source_output_opened[i] = 0;
    }
    u->n_source_output_opened = 0;
    u->combined = NULL;
    u->media_type = edefaultapp;
    u->bt_connected = false;
#if HAVE_BSA
    u->bt_module = NULL;
    u->btAddress = (char *)pa_xmalloc0(BTADDRESS_STRING_SIZE);
    u->btProfile = (char *)pa_xmalloc0(BTPROFILE_STRING_SIZE);
#endif

    u->rtp_module = NULL;
    u->alsa_source = NULL;
    u->default_alsa_sink = NULL;
    u->IsUsbConnected = false;
    u->IsUsbMICConnected = false;
    u->externalSoundCardNumber = -1;
    u->externalMICCardNumber = -1;
    u->destAddress = (char *)pa_xmalloc0(RTP_IP_ADDRESS_STRING_SIZE);
    u->connectionType = (char *)pa_xmalloc0(RTP_CONNECTION_TYPE_STRING_SIZE);
    u->connectionPort = 0;

    u->deviceName = (char *)pa_xmalloc0(DEVICE_NAME_SIZE);
    u->callback_deviceName = (char *)pa_xmalloc0(DEVICE_NAME_SIZE);

    u->scenario = (char *)pa_xmalloc0(SCENARIO_STRING_SIZE);
    u->pout_previous_state = PA_SINK_SUSPENDED;
    u->compress_previous_state = PA_SINK_SUSPENDED;
    u->bsa_previous_state = PA_SINK_SUSPENDED;
    u->current_state = PA_SOURCE_SUSPENDED;
    u->device_open = false;
    return make_socket(u);

  fail:
    return -1;
}


/* callback for stream creation */
static pa_hook_result_t route_sink_input_new_hook_callback(pa_core * c, pa_sink_input_new_data * data,
                                                           struct userdata *u) {
    int i, sink_index = edefaultapp;
    pa_sink *sink = NULL;
    pa_proplist *type = NULL;

    pa_assert(data);
    pa_assert(u);
    pa_assert(c);

    if (data->sink == NULL) {
        /* redirect everything to the default application stream */
        pa_log_info("THE DEFAULT DEVICE WAS USED TO CREATE THIS STREAM - PLEASE CATEGORIZE USING A VIRTUAL STREAM");
    }
    else if (pa_streq(data->sink->name, REMOTE_SINK_NAME)) {
        pa_log_info("data->sink->name : %s, do not route to hw sink",data->sink->name);
        return PA_HOOK_OK;
    }
    else {
        pa_log_debug("new stream is opened with sink name : %s", data->sink->name);
        if (pa_streq(data->sink->name, "pmedia_vm")) {
            /* consider the stream from voice memo as pmedia. This fix is to avoid resource conflict of tinycompress between
             * Music app and  Voice memo */
            sink = pa_namereg_get(c, "pmedia", PA_NAMEREG_SINK);
            pa_assert(sink != NULL);
            data->sink = sink;
            sink = NULL;
        }

        for (i = eVirtualSink_First; i < eVirtualSink_Count; i++) {
            if (pa_streq(data->sink->name, systemdependantvirtualsinkmap[i].virtualsinkname)) {
                pa_log_debug
                    ("found virtual sink index on virtual sink %d, name %s, index %d",
                     systemdependantvirtualsinkmap[i].virtualsinkidentifier, data->sink->name, i);
                sink_index = i;
                break;
            }
        }
    }

    type = pa_proplist_new();
    if ((data->sink != NULL) && sink_index == edefaultapp && pa_streq(data->sink->name, PCM_SINK_NAME)) {

        pa_proplist_sets(type, "media.type", systemdependantvirtualsinkmap[u->media_type].virtualsinkname);
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, type);

        sink = pa_namereg_get(c, data->sink->name, PA_NAMEREG_SINK);
        if (sink && PA_SINK_IS_LINKED(sink->state))
            pa_sink_input_new_data_set_sink(data, sink, TRUE, FALSE);
    }
    else if ((data->sink != NULL) && sink_index == edefaultapp && pa_streq(data->sink->name, BSA_SINK_NAME)) {
        pa_proplist_sets(type, "media.type", systemdependantvirtualsinkmap[u->media_type].virtualsinkname);
        pa_proplist_update(data->proplist, PA_UPDATE_MERGE, type);

        sink = pa_namereg_get(c, BSA_SINK_NAME, PA_NAMEREG_SINK);
        if (sink && PA_SINK_IS_LINKED(sink->state))
            pa_sink_input_new_data_set_sink(data, sink, TRUE, FALSE);
    }
    else {
        for (i = eVirtualSink_First; i < eVirtualSink_Count; i++) {

            if (u->sink_mapping_table[i].virtualdevice == systemdependantvirtualsinkmap[sink_index].virtualsinkidentifier) {

                pa_log_info("setting data->sink (physical) to %s for streams created on %s (virtual)",
                        systemdependantphysicalsinkmap[u->sink_mapping_table[i].physicaldevice].physicalsinkname,
                        systemdependantvirtualsinkmap[i].virtualsinkname);

                if (!sink_input_new_data_is_passthrough(data)) {
                    pa_proplist_sets(type, "media.type", systemdependantvirtualsinkmap[i].virtualsinkname);
                    pa_proplist_update(data->proplist, PA_UPDATE_MERGE, type);
                }

                if (pa_streq(systemdependantphysicalsinkmap[u->sink_mapping_table[i].physicaldevice].physicalsinkname, BSA_SINK_NAME)
                            && u->bt_connected) {
                    u->media_type = i;
                    sink = pa_namereg_get(c, BSA_SINK_NAME, PA_NAMEREG_SINK);
                }
                else if (pa_streq(systemdependantphysicalsinkmap[u->sink_mapping_table[i].physicaldevice].physicalsinkname, COMBINED_SINK_NAME)
                            && u->bt_connected) {
                    u->media_type = i;
                    sink = pa_namereg_get(c, COMBINED_SINK_NAME, PA_NAMEREG_SINK);
                }
                else if (pa_streq(systemdependantphysicalsinkmap[u->sink_mapping_table[i].physicaldevice].physicalsinkname, RTP_SINK_NAME)
                            && u->rtp_module){
                    u->media_type = i;
                    sink = pa_namereg_get(c, RTP_SINK_NAME, PA_NAMEREG_SINK);
                }
                else {
                    u->media_type = i;
                    sink = pa_namereg_get(c, systemdependantphysicalsinkmap[u->sink_mapping_table[i].physicaldevice].physicalsinkname, PA_NAMEREG_SINK);
                }
                if (sink && PA_SINK_IS_LINKED(sink->state) && !sink_input_new_data_is_passthrough(data))
                    pa_sink_input_new_data_set_sink(data, sink, FALSE, FALSE);

                if (sink_input_new_data_is_passthrough(data)) {
                    pa_proplist_sets(type, "media.type", "pmedia");
                    pa_proplist_update(data->proplist, PA_UPDATE_MERGE, type);

                    sink = pa_namereg_get(c, COMPRESS_SINK_NAME, PA_NAMEREG_SINK);
                    if (sink)
                        pa_sink_input_new_data_set_sink(data, sink, TRUE, FALSE);
                }
                break;
            }
        }

    }

    if (type)
        pa_proplist_free(type);

    return PA_HOOK_OK;
}


static pa_hook_result_t route_sink_input_fixate_hook_callback(pa_core * c, pa_sink_input_new_data * data,
                                                              struct userdata *u) {

    int i, sink_index, volumetoset, volumetable;
    const char *type;
    struct pa_cvolume cvolume;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    if (data->sink != NULL && pa_streq(data->sink->name, REMOTE_SINK_NAME))
    {
        volumetoset = pa_sw_volume_from_dB(0);
        pa_cvolume_set(&cvolume, data->channel_map.channels, volumetoset);
        pa_sink_input_new_data_set_volume(data, &cvolume);
        pa_log_info("data->sink->name : %s, Setting volume(%d)",data->sink->name, volumetoset);
        return PA_HOOK_OK;
    }

    //Returning if the stream is passthrough
    if (sink_input_new_data_is_passthrough(data))
        return PA_HOOK_OK;

    type = pa_proplist_gets(data->proplist, "media.type");

    for (i = 0; i < eVirtualSink_Count; i++) {
        if (pa_streq(type, systemdependantvirtualsinkmap[i].virtualsinkname)) {
            sink_index = i;
            break;
        }
    }
    pa_assert(sink_index >= eVirtualSink_First);
    pa_assert(sink_index <= eVirtualSink_Last);

    volumetable = u->sink_mapping_table[sink_index].volumetable;
    volumetoset = pa_sw_volume_from_dB(_mapPercentToPulseRamp[volumetable]
                                       [u->sink_mapping_table[sink_index].volume]);

    pa_log_debug("Setting volume(%d) for stream type(%s)", volumetoset, type);

    pa_cvolume_set(&cvolume, data->channel_map.channels, volumetoset);
    pa_sink_input_new_data_set_volume(data, &cvolume);

    return PA_HOOK_OK;
}

static pa_hook_result_t route_sink_input_put_hook_callback(pa_core * c, pa_sink_input * data, struct userdata *u) {

    struct sinkinputnode *si_data = NULL;
    const char *si_type;
    pa_sink_input_state_t state;
    int i;

    pa_assert(c);
    pa_assert(u);

    if (data->sink != NULL && pa_streq(data->sink->name, REMOTE_SINK_NAME))
    {
        pa_log_info("data->sink->name : %s, do not add to sink-input node",data->sink->name);
        return PA_HOOK_OK;
    }

    si_data = pa_xnew0(struct sinkinputnode, 1);

    si_data->sinkinput = data;
    si_data->sinkinputidx = data->index;
    si_data->paused = true;
    si_data->virtualsinkid = -1;

    si_type = pa_proplist_gets(data->proplist, "media.type");
    for (i = 0; i < eVirtualSink_Count; i++) {
        if (pa_streq(si_type, systemdependantvirtualsinkmap[i].virtualsinkname)) {
            si_data->virtualsinkid = systemdependantvirtualsinkmap[i].virtualsinkidentifier;
            break;
        }
    }
    pa_assert(si_data->virtualsinkid != -1);
    pa_assert(si_data->virtualsinkid >= eVirtualSink_First);
    pa_assert(si_data->virtualsinkid <= eVirtualSink_Last);

    u->n_sink_input_opened++;
    PA_LLIST_PREPEND(struct sinkinputnode, u->sinkinputnodelist, si_data);

    state = data->state;

    /* send notification to audiod only if sink_input is in uncorked state */
    if (state == PA_SINK_INPUT_CORKED) {
        //si_data->paused = true; already done as part of init
        pa_log_debug("stream type (%s) is opened in corked state", si_type);
        return PA_HOOK_OK;
    }

    /* notify audiod of stream open */
    if (u->connectionactive && u->connev) {
        char audiobuf[SIZE_MESG_TO_AUDIOD];
        int ret;
        /* we have a connection send a message to audioD */
        si_data->paused = false;
        /* Currently setsw('s') mode is not supported in TV audiod
        change msg signal back to 's' when platform audiod is merged*/
        sprintf(audiobuf, "o %d %d", si_data->virtualsinkid, si_data->sinkinputidx);
        u->audiod_sink_input_opened[si_data->virtualsinkid]++;

        ret = send(u->newsockfd, audiobuf, SIZE_MESG_TO_AUDIOD, 0);
        if (-1 == ret)
            pa_log("send() failed: %s", strerror(errno));
        else
            pa_log_info("sent playback stream open message to audiod");
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t route_sink_state_changed_hook_callback (pa_core *c, pa_object *o, struct userdata *u){

    pa_sink *s = NULL;
    pa_sink_state_t  state;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    memset(audiodbuf, 0, SIZE_MESG_TO_AUDIOD);

    pa_assert(c);
    pa_assert(o);
    pa_assert(u);

    if (pa_sink_isinstance(o)) {
        s = PA_SINK(o);

        if (!pa_streq(s->name, BSA_SINK_NAME))
            return PA_HOOK_OK;

        state = s->state;
        /* notify audiod of Sink States*/
        if (state == PA_SINK_RUNNING)
           sprintf(audiodbuf, "a %d %d", 0, 0);
        else if (state == PA_SINK_SUSPENDED)
           sprintf(audiodbuf, "b %d %d", 0, 0);

        if (send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, MSG_NOSIGNAL) < 0)
            pa_log("route_sink_state_changed_hook_callback: send failed: %s",strerror(errno));
        else
            pa_log_debug("route_sink_state_changed_hook_callback: sent BT sink state message(%s) to audiod", audiodbuf);
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t route_source_output_new_hook_callback(pa_core * c, pa_source_output_new_data * data,
                                                              struct userdata *u) {
    int i, source_index = erecord;
    pa_proplist *stream_type;
    pa_proplist *a;
    char *port,*dest_ip,*prop_name;
    pa_assert(data);
    pa_assert(u);
    pa_assert(c);

    prop_name = pa_strnull(pa_proplist_gets(data->proplist, PA_PROP_MEDIA_NAME));

    if(!strcmp(prop_name,"RTP Monitor Stream")) {
        port = pa_strnull(pa_proplist_gets(data->proplist, "rtp.port"));
        dest_ip = pa_strnull(pa_proplist_gets(data->proplist, "rtp.destination"));
        send_rtp_connection_data_to_audiod(dest_ip,port,u) ;
    }

    if (data->source == NULL) {
        /* redirect everything to the default application stream */
        pa_log("THE DEFAULT DEVICE WAS USED TO CREATE THIS STREAM - PLEASE CATEGORIZE USING A VIRTUAL STREAM");
    }
    else {

        if (strstr(data->source->name, "monitor")) {
            pa_log_info("found a monitor source, do not route to hw sink!");
            return PA_HOOK_OK;
        }

        for (i = 0; i < eVirtualSource_Count; i++) {
            if (pa_streq(data->source->name, systemdependantvirtualsourcemap[i].virtualsourcename)) {
                pa_log_debug
                    ("found virtual source index on virtual source %d, name %s, index %d",
                     systemdependantvirtualsourcemap[i].virtualsourceidentifier, data->source->name, i);
                source_index = i;
                break;
            }
        }
    }

    stream_type = pa_proplist_new();
    pa_proplist_sets(stream_type, "media.type", systemdependantvirtualsourcemap[source_index].virtualsourcename);
    pa_proplist_update(data->proplist, PA_UPDATE_MERGE, stream_type);
    pa_proplist_free(stream_type);


    /* implement policy */
    for (i = 0; i < eVirtualSource_Count; i++) {
        if (i == (int) systemdependantvirtualsourcemap[source_index].virtualsourceidentifier) {
            pa_source *s;

            pa_log_debug
                ("setting data->source (physical) to %s for streams created on %s (virtual)",
                 systemdependantphysicalsourcemap[u->source_mapping_table[i].physicaldevice].physicalsourcename,
                 systemdependantvirtualsourcemap[i].virtualsourcename);

            s = pa_namereg_get(c,
                               systemdependantphysicalsourcemap[u->source_mapping_table
                                                                [i].physicaldevice].physicalsourcename,
                               PA_NAMEREG_SOURCE);
            if (s && PA_SOURCE_IS_LINKED(s->state))
                pa_source_output_new_data_set_source(data, s, false, true);
            break;
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t route_source_output_fixate_hook_callback(pa_core * c, pa_source_output_new_data * data,
                                                                 struct userdata *u) {

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    /* nothing much to to in fixate as of now */
    return PA_HOOK_OK;
}

static pa_hook_result_t route_source_output_put_hook_callback(pa_core * c, pa_source_output * so, struct userdata *u) {
    const char *so_type = NULL;
    struct sourceoutputnode *node = NULL;
    int i, source_index = -1;
    pa_source_output_state_t state;

    pa_assert(c);
    pa_assert(so);
    pa_assert(u);

    so_type = pa_proplist_gets(so->proplist, "media.type");

    if (!so_type && strstr(so->source->name, "monitor"))
        return PA_HOOK_OK;      /* nothing to be done for monitor source */

    pa_assert(so_type != NULL);

    node = pa_xnew0(struct sourceoutputnode, 1);
    node->virtualsourceid = -1;
    node->sourceoutput = so;
    node->sourceoutputidx = so->index;
    node->paused = false;

    for (i = 0; i < eVirtualSource_Count; i++) {
        if (pa_streq(so_type, systemdependantvirtualsourcemap[i].virtualsourcename)) {
            source_index = systemdependantvirtualsourcemap[i].virtualsourceidentifier;
            break;
        }
    }

    pa_assert(source_index != -1);
    node->virtualsourceid = source_index;

    u->n_source_output_opened++;
    PA_LLIST_PREPEND(struct sourceoutputnode, u->sourceoutputnodelist, node);

    state = so->state;
    if (state == PA_SOURCE_OUTPUT_CORKED) {
        node->paused = true;
        pa_log_debug("Record stream of type(%s) is opened in corked state", so_type);
        return PA_HOOK_OK;
    }
    if (u->connectionactive && u->connev) {
        char audiobuf[SIZE_MESG_TO_AUDIOD];
        int ret;

        sprintf(audiobuf, "d %d %d", node->virtualsourceid, node->sourceoutputidx);
        ret = send(u->newsockfd, audiobuf, SIZE_MESG_TO_AUDIOD, 0);
        if (ret == -1)
            pa_log("Record stream type(%s): send failed(%s)", so_type, strerror(errno));

    }
    u->audiod_source_output_opened[source_index]++;

    return PA_HOOK_OK;
}

/* callback for stream deletion */
static pa_hook_result_t route_sink_input_unlink_hook_callback(pa_core * c, pa_sink_input * data, struct userdata *u) {
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    struct sinkinputnode *thelistitem = NULL;

    pa_assert(data);
    pa_assert(u);

    /* delete the list item */
    for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {

        if (thelistitem->sinkinput == data) {

            /* we have a connection send a message to audioD */
            if (!thelistitem->paused) {

                /* notify audiod of stream closure */
                if (u->connectionactive && u->connev != NULL) {

                    sprintf(audiodbuf, "c %d %d", thelistitem->virtualsinkid, thelistitem->sinkinputidx);
                    if (-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
                        pa_log_info("route_sink_input_unlink_hook_callback: send failed: %s", strerror(errno));
                    else
                        pa_log_info("route_sink_input_unlink_hook_callback: sending close notification to audiod");
                }

                // decrease sink opened count, even if audiod doesn't hear from it
                if (thelistitem->virtualsinkid >= eVirtualSink_First && thelistitem->virtualsinkid <= eVirtualSink_Last)
                    u->audiod_sink_input_opened[thelistitem->virtualsinkid]--;
            }

            /* remove this node from the list and free */
            PA_LLIST_REMOVE(struct sinkinputnode, u->sinkinputnodelist, thelistitem);

            pa_xfree(thelistitem);
            pa_assert(u->n_sink_input_opened > 0);
            u->n_sink_input_opened--;
            break;
        }
    }

    return PA_HOOK_OK;
}

/* callback for stream pause/unpause */
static pa_hook_result_t
route_sink_input_state_changed_hook_callback(pa_core * c, pa_sink_input * data, struct userdata *u) {
    pa_sink_input_state_t state;
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    struct sinkinputnode *thelistitem = NULL;

    pa_assert(data);
    pa_assert(u);

    /* delete the list item */
    for (thelistitem = u->sinkinputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {

        if (thelistitem->sinkinput == data) {

            //state = pa_sink_input_get_state(thelistitem->sinkinput);
            state = data->state;

            /* we have a connection send a message to audioD */
            if (!thelistitem->paused && state == PA_SINK_INPUT_CORKED) {
                thelistitem->paused = true;
                sprintf(audiodbuf, "c %d %d", thelistitem->virtualsinkid, thelistitem->sinkinputidx);

                // decrease sink opened count, even if audiod doesn't hear from it
                if (thelistitem->virtualsinkid >= eVirtualSink_First && thelistitem->virtualsinkid <= eVirtualSink_Last)
                    u->audiod_sink_input_opened[thelistitem->virtualsinkid]--;

            }
            else if (thelistitem->paused && state != PA_SINK_INPUT_CORKED) {
                thelistitem->paused = false;
                sprintf(audiodbuf, "o %d %d", thelistitem->virtualsinkid, thelistitem->sinkinputidx);

                // increase sink opened count, even if audiod doesn't hear from it
                if (thelistitem->virtualsinkid >= eVirtualSink_First && thelistitem->virtualsinkid <= eVirtualSink_Last)
                    u->audiod_sink_input_opened[thelistitem->virtualsinkid]++;
            }
            else
                continue;

            /* notify audiod of stream closure */
            if (u->connectionactive && u->connev != NULL) {
                if (-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
                    pa_log("route_sink_input_state_changed_hook_callback: send failed: %s", strerror(errno));
                else
                    pa_log_info
                        ("route_sink_input_state_changed_hook_callback: sending state change notification to audiod");
            }
        }
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t route_source_output_state_changed_hook_callback(pa_core * c, pa_source_output * so, struct userdata *u) {

    pa_source_output_state_t state;
    struct sourceoutputnode *node;
    char audiobuf[SIZE_MESG_TO_AUDIOD];
    int ret;

    pa_assert(c);
    pa_assert(so);
    pa_assert(u);

    state = so->state;

    for (node = u->sourceoutputnodelist; node; node = node->next) {
        if (node->sourceoutput == so) {
            if (state == PA_SOURCE_OUTPUT_CORKED) {
                pa_assert(node->paused == false);
                sprintf(audiobuf, "k %d %d", node->virtualsourceid, node->sourceoutputidx);
                node->paused = true;

                /* decrease source opened count, even if audiod doesn't hear from it */
                if (node->virtualsourceid >= eVirtualSource_First && node->virtualsourceid <= eVirtualSource_Last)
                    u->audiod_source_output_opened[node->virtualsourceid]--;
            }
            else if (state == PA_SOURCE_OUTPUT_RUNNING) {
                pa_assert(node->paused == true);
                node->paused = false;
                sprintf(audiobuf, "d %d %d", node->virtualsourceid, node->sourceoutputidx);

                /* increase source opened count, even if audiod doesn't hear from it */
                if (node->virtualsourceid >= eVirtualSource_First && node->virtualsourceid <= eVirtualSource_Last)
                    u->audiod_source_output_opened[node->virtualsourceid]++;
            }
            ret = send(u->newsockfd, audiobuf, SIZE_MESG_TO_AUDIOD, 0);
            if (ret == -1)
                pa_log("Error sending recording stream msg (%s)", audiobuf);
            break;
        }
    }
    return PA_HOOK_OK;
}

/* callback for source deletion */
static pa_hook_result_t route_source_output_unlink_hook_callback(pa_core * c, pa_source_output * data, struct userdata *u) {
    char audiodbuf[SIZE_MESG_TO_AUDIOD];
    struct sourceoutputnode *thelistitem = NULL;

    pa_assert(data);
    pa_assert(u);
    pa_assert(c);

    /* delete the list item */
    for (thelistitem = u->sourceoutputnodelist; thelistitem != NULL; thelistitem = thelistitem->next) {

        if (thelistitem->sourceoutput == data) {

            if (!thelistitem->paused) {
                /* we have a connection send a message to audioD */
                /* notify audiod of stream closure */
                if (u->connectionactive && u->connev != NULL) {
                    sprintf(audiodbuf, "k %d %d", thelistitem->virtualsourceid, thelistitem->sourceoutputidx);

                    if (-1 == send(u->newsockfd, audiodbuf, SIZE_MESG_TO_AUDIOD, 0))
                        pa_log("route_source_output_unlink_hook_callback: send failed: %s", strerror(errno));
                    else
                        pa_log_info("route_source_output_unlink_hook_callback: sending close notification to audiod");
                }

                // decrease source opened count, even if audiod doesn't hear from it
                if (thelistitem->virtualsourceid >= eVirtualSource_First
                    && thelistitem->virtualsourceid <= eVirtualSource_Last)
                    u->audiod_source_output_opened[thelistitem->virtualsourceid]--;
            }

            /* remove this node from the list and free */
            PA_LLIST_REMOVE(struct sourceoutputnode, u->sourceoutputnodelist, thelistitem);

            pa_xfree(thelistitem);

            // maintain count, even if we can't talk to audiod
            pa_assert(u->n_source_output_opened > 0);
            u->n_source_output_opened--;
            break;
        }
    }

    return PA_HOOK_OK;
}

/* exit and cleanup */

void pa__done(pa_module * m) {
    struct userdata *u;
    struct sinkinputnode *thelistitem = NULL;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->connev != NULL) {
        /* a connection exists on the socket, tear down */
        /* remove ourselves from the IO list on the main loop */
        u->core->mainloop->io_free(u->connev);

        /* tear down the connection */
        if (-1 == shutdown(u->newsockfd, SHUT_RDWR)) {
            pa_log_info("Error in shutdown (%d):%s", errno, strerror(errno));
        }
        if (-1 == close(u->newsockfd)) {
            pa_log_info("Error in close (%d):%s", errno, strerror(errno));
        }
    }

    if (u->sockev != NULL) {
        /* a socket still exists, tear it down and remove
         * ourselves from the IO list on the pulseaudio
         * main loop */
        u->core->mainloop->io_free(u->sockev);

        /* tear down the connection */
        if (-1 == shutdown(u->sockfd, SHUT_RDWR)) {
            pa_log_info("Error in shutdown (%d):%s", errno, strerror(errno));
        }
        if (-1 == close(u->sockfd)) {
            pa_log_info("Error in close (%d):%s", errno, strerror(errno));
        }
    }

    disconnect_hooks(u);

    /* free the list of sink-inputs */
    while ((thelistitem = u->sinkinputnodelist) != NULL) {
        PA_LLIST_REMOVE(struct sinkinputnode, u->sinkinputnodelist, thelistitem);
        pa_xfree(thelistitem);
    }

#if HAVE_BSA
    pa_xfree(u->btAddress);
    pa_xfree(u->btProfile);
#endif

    pa_xfree(u->destAddress);
    pa_xfree(u->connectionType);
    pa_xfree(u->scenario);
    u->scenario = NULL;

    pa_xfree(u);
}

static const char* const device_valid_modargs[] = {
    "name",
    "source_name",
    "source_properties",
    "namereg_fail",
    "device",
    "device_id",
    "format",
    "rate",
    "alternate_rate",
    "channels",
    "channel_map",
    "fragments",
    "fragment_size",
    "mmap",
    "tsched",
    "tsched_buffer_size",
    "tsched_buffer_watermark",
    "ignore_dB",
    "control",
    "deferred_volume",
    "deferred_volume_safety_margin",
    "deferred_volume_extra_delay",
    "fixed_latency_range",
    "sink_name",
    "sink_properties",
    "rewind_safeguard",
    NULL
};


static pa_hook_result_t module_unload_subscription_callback(pa_core *c, pa_module *m, struct userdata *u)
{
    pa_log_info("module_unload_subscription_callback");
    pa_assert(c);
    pa_assert(m);
    pa_assert(u);
    pa_modargs *ma = NULL;

    if (!(ma = pa_modargs_new(m->argument, device_valid_modargs))) {
        pa_log("Failed to parse module arguments.");
    }
    else
    {
        u->callback_deviceName = NULL;
        pa_log_info("module other = %s %d", m->name, m->index);
        if (0 == strncmp(m->name, "module-alsa-source", SOURCE_NAME_LENGTH))
            u->callback_deviceName = pa_xstrdup(pa_modargs_get_value(ma, "source_name", NULL));
        else if (0 == strncmp(m->name, "module-alsa-sink", SINK_NAME_LENGTH))
            u->callback_deviceName = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL));
        else
            pa_log_info("module other than alsa source and sink is unloaded");
        if (NULL != u->callback_deviceName)
        {
            pa_log_debug("module_unloaded with device name:%s", u->callback_deviceName);
            /* notify audiod of device insertion */
            if (u->connectionactive && u->connev) {
                char audiobuf[SIZE_MESG_TO_AUDIOD];
                int ret = -1;
                /* we have a connection send a message to audioD */
                sprintf(audiobuf, "%c %s", '3', u->callback_deviceName);
                pa_log_info("payload:%s", audiobuf);
                ret = send(u->newsockfd, audiobuf, SIZE_MESG_TO_AUDIOD, 0);
                if (-1 == ret)
                    pa_log("send() failed: %s", strerror(errno));
                else
                    pa_log_info("sent device unloaded message to audiod");
            }
            else
                pa_log_warn("connectionactive is not active");
        }
        else
            pa_log_warn("error reading device name");
    }

    if (ma)
        pa_modargs_free(ma);

    return PA_HOOK_OK;
}

static pa_hook_result_t module_load_subscription_callback(pa_core *c, pa_module *m, struct userdata *u)
{
    pa_log_info("module_load_subscription_callback");
    pa_assert(c);
    pa_assert(m);
    pa_assert(u);
    pa_log_debug("module_loaded with name:%s", m->name);
    pa_modargs *ma = NULL;
    if (!(ma = pa_modargs_new(m->argument, device_valid_modargs))) {
        pa_log("Failed to parse module arguments.");
    }
    else
    {
        u->callback_deviceName = NULL;
        if (0 == strncmp(m->name, "module-alsa-source", SOURCE_NAME_LENGTH))
            u->callback_deviceName = pa_xstrdup(pa_modargs_get_value(ma, "source_name", NULL));
        else if (0 == strncmp(m->name, "module-alsa-sink", SINK_NAME_LENGTH))
            u->callback_deviceName = pa_xstrdup(pa_modargs_get_value(ma, "sink_name", NULL));
        else
            pa_log_info("module other than alsa source and sink is loaded");
        if (NULL != u->callback_deviceName)
        {
            pa_log_debug("module_loaded with device name:%s", u->callback_deviceName);
            /* notify audiod of device insertion */
            if (u->connectionactive && u->connev) {
                char audiobuf[SIZE_MESG_TO_AUDIOD];
                int ret = -1;
                /* we have a connection send a message to audioD */
                sprintf(audiobuf, "%c %s", 'i', u->callback_deviceName);
                pa_log_info("payload:%s", audiobuf);
                ret = send(u->newsockfd, audiobuf, SIZE_MESG_TO_AUDIOD, 0);
                if (-1 == ret)
                    pa_log("send() failed: %s", strerror(errno));
                else
                    pa_log_info("sent device loaded message to audiod");
            }
            else
                pa_log_warn("connectionactive is not active");
        }
        else
            pa_log_warn("error reading device name");
    }

    if (ma)
        pa_modargs_free(ma);

    return PA_HOOK_OK;
}

static pa_hook_result_t route_sink_input_move_finish_cb(pa_core *c, pa_sink_input *data, struct userdata *u)
{
    int i;

    pa_assert(c);
    pa_assert(data);
    pa_assert(u);

    for (i = eVirtualSink_First; i<= eVirtualSink_Last; i++)
        virtual_sink_input_set_volume(i, u->sink_mapping_table[i].volume, 0, u);

    pa_log_debug ("moved sink inputs to the destination sink");
    return PA_HOOK_OK;
}

pa_hook_result_t route_sink_put_cb(pa_core *c, pa_sink *sink, struct userdata *u)
{
    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

#if HAVE_BSA
    if (strstr(sink->name, BSA_SINK_NAME)) {
        char *args = NULL;
        pa_log_debug("BT is connected, sink (%s) link", sink->name);
        args = pa_sprintf_malloc("slaves=pcm_output,%s", sink->name);

        u->combined = pa_module_load(u->core, "module-combine-sink", args);
        if (u->combined)
            pa_log_debug("module-combine-sink loaded");
        else
            pa_log("module-combine-sink loading failed");

        pa_xfree(args);
    }
#endif

    return PA_HOOK_OK;
}

pa_hook_result_t route_sink_unlink_cb(pa_core *c, pa_sink *sink, struct userdata *u)
{
    pa_assert(c);
    pa_assert(sink);
    pa_assert(u);

#if HAVE_BSA
    if (strstr(sink->name, BSA_SINK_NAME)) {
        pa_log_debug("BT disconnected, sink (%s) unlink", sink->name);
        if (u->combined) {
            pa_log_debug("Unloading module-combined-sink");
            pa_module_unload(u->combined, TRUE);
            u->combined = NULL;
        } else
            pa_log_debug("module-combined-sink already unloaded");
    }
#endif

    return PA_HOOK_OK;
}

