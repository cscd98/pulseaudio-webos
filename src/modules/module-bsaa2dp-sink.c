/**********************************************************************
 * Copyright (c) 2014-2015 LG Electronics, Inc.
 * All rights reserved.
 *
 * module-bsaa2dp-sink- this sink is used for data rendering
 * through Broadcom bluetooth stack - bsa
 **********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

/* Warning: bsa.h files needs to be included before
  * pulse.h files, otherwise compiler refuses to
  * compile due to macro conflict(not sure anyway)*/

#include <bsa/bsa_av_api.h>
#include <bsa/bsa_dm_api.h>
#include <bsa/uipc_bsa.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>


PA_MODULE_AUTHOR(" arnab.samanta@lge.com kiran.krishnappa@lge.com shiburaj.cp@lge.com");
PA_MODULE_DESCRIPTION(_("bsa sink for broadcomm bt stack"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
        "sink_name=<name of sink> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> ");

#define DEFAULT_SINK_NAME "bsaa2dp"
#define DEFAULT_SAMPLING_FREQ 44100
#define DEFAULT_CHANNELS 2
#define DEFAULT_BITS_PER_SAMPLE 16
#define UIPC_INIT_PATH  "/var/lib/bluetooth"  /* Ideally this macro should come from bsa package */
#define DEFAULT_FRAG_SIZE  (1024 * 48)      /* fragment size 48k */
#define BSA_BUFFER_SIZE 200000
#define UIPC_CHANNEL_NUMBER 5
#define P(_x)   ((unsigned long long)(_x))

/* for debugging - to be enabled for logs
     that appear recursively if pa_log_debug/info
     is used */
#define DEBUG_BSA 0

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_usec_t block_usec;
    pa_usec_t timestamp;
    tUIPC_CH_ID   stream_uipc_channel;
    BOOLEAN is_channel_opened;
    size_t bsa_buffer_size;
    size_t frag_size;
};

/* TBD : To be added/deleted */
static const char* const valid_modargs[] = {
    "sink_name",
    "format",
    "rate",
    "channels",
    NULL
};

static void bsa_userdata_init (struct userdata *pcontext);
static int  open_uipc_device(struct userdata *u);
static int  close_uipc_channel(struct userdata *u);
/* BSA API */
extern BOOLEAN UIPC_Avail(tUIPC_CH_ID ch_id, INT32 *p_avail);

static int sink_process_msg(
        pa_msgobject *o,
        int code,
        void *data,
        int64_t offset,
        pa_memchunk *chunk) {

    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_SET_STATE:

            switch(PA_PTR_TO_UINT(data)) {
                case PA_SINK_IDLE:
                case PA_SINK_RUNNING:
                    if (PA_PTR_TO_UINT(data) == PA_SINK_RUNNING)
                        u->timestamp = pa_rtclock_now();

                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED)
                        open_uipc_device(u);
                    break;

                case PA_SINK_SUSPENDED:
                    close_uipc_channel(u);
                    break;
            }

            break;

        case PA_SINK_MESSAGE_GET_LATENCY: {
            pa_usec_t latency = 0;
            INT32 avail = 0;
            if (u->is_channel_opened) {
                UIPC_Avail(u->stream_uipc_channel, &avail);
                if (avail != -1)
                    latency = pa_bytes_to_usec(u->bsa_buffer_size - avail, &u->sink->sample_spec);
            }
            *((pa_usec_t*) data) = latency;

            return 0;
        }
    }
    return pa_sink_process_msg(o, code, data, offset, chunk);
}


static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    if (u->block_usec == (pa_usec_t) -1)
        u->block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);
    pa_sink_set_max_rewind_within_thread(s, nbytes);
    pa_sink_set_max_request_within_thread(s, nbytes);
}

static void process_rewind(struct userdata *u, pa_usec_t now) {
    size_t rewind_nbytes;
    size_t pending_nbytes;
    INT32 avail = -1;

    pa_assert(u);

    /* Figure out how much we shall rewind and reset the counter */
    rewind_nbytes = u->sink->thread_info.rewind_nbytes;

    if (!PA_SINK_IS_OPENED(u->sink->thread_info.state) || rewind_nbytes <= 0)
        goto do_nothing;

    pa_log_debug("Requested to rewind %lu bytes.", (unsigned long) rewind_nbytes);

    if (u->timestamp <= now)
        goto do_nothing;

    UIPC_Avail(u->stream_uipc_channel, &avail);
    if (avail == -1) {
      pa_log("Avail is not succesful");
      goto do_nothing;
    }

    pending_nbytes = u->bsa_buffer_size - avail;

    if (pending_nbytes <= 0)
        goto do_nothing;

    if (rewind_nbytes > pending_nbytes)
        rewind_nbytes = pending_nbytes;
    else
        rewind_nbytes = pending_nbytes;

    /* flush the driver */
    UIPC_Ioctl(u->stream_uipc_channel, UIPC_REQ_RX_FLUSH, NULL);
    pa_sink_process_rewind(u->sink, rewind_nbytes);
    u->timestamp -= pa_bytes_to_usec(rewind_nbytes, &u->sink->sample_spec);
    pa_log_debug("Rewound %lu bytes", (unsigned long) rewind_nbytes);
    return;

do_nothing:
    pa_log_debug("Nothing to rewind");
    pa_sink_process_rewind(u->sink, 0);
}


static void process_render(struct userdata *u, pa_usec_t now) {
    INT32 avail = 0;
    size_t write = 0;
    size_t process_usec = 0;
    uint8_t *p;
    pa_memchunk chunk;

    pa_assert(u);

    UIPC_Avail(u->stream_uipc_channel, &avail);

#if DEBUG_BSA
    pa_log ("BSA : Avail(%d) max_request(%lu)", avail, (unsigned long)u->sink->thread_info.max_request);
#endif

    if (avail == 0) {
       u->timestamp = pa_rtclock_now() + pa_bytes_to_usec(u->bsa_buffer_size/2, &u->sink->sample_spec);
       return;
    }

    if ((unsigned long)avail > u->sink->thread_info.max_request) {
           avail = u->sink->thread_info.max_request;
    }

    write = (size_t)avail;
    do{

        pa_sink_render_full (u->sink, write, &chunk);
        p = pa_memblock_acquire(chunk.memblock);

        if (UIPC_Send(u->stream_uipc_channel, 0, p + chunk.index, chunk.length) == FALSE)
            pa_log("Error writing to UIPC channel");

#if DEBUG_BSA
        pa_log ("BSA : Asked(%lu) chunk.length got =(%lu)", (unsigned long)write, (unsigned long)chunk.length);
#endif

        write -=chunk.length;
        pa_memblock_release(chunk.memblock);
    }while(write > 0);

    UIPC_Avail(u->stream_uipc_channel, &avail);
    process_usec = (size_t)(u->bsa_buffer_size - avail);

    /* setting Timer half of h/w buffer */
    u->timestamp += pa_bytes_to_usec(process_usec/2, &u->sink->sample_spec);

#if DEBUG_BSA
    pa_log("Process time: diff(%llu)", P(u->timestamp - pa_rtclock_now())/1000);
#endif

    pa_memblock_unref(chunk.memblock);
    pa_memchunk_reset(&chunk);
}

static void thread_func(void *userdata) {
    pa_usec_t now;
    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("BSA : Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            process_rewind(u, now);

        /* Render some data and drop it immediately */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now) {
                process_render(u, now);
                pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
            }
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    pa_sink_new_data data;
    size_t nbytes;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    map = m->core->default_channel_map;

    ss.format = DEFAULT_BITS_PER_SAMPLE == 16 ? PA_SAMPLE_S16LE: PA_SAMPLE_U8;
    ss.rate   = DEFAULT_SAMPLING_FREQ;
    ss.channels = DEFAULT_CHANNELS;

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    bsa_userdata_init(u);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    pa_sink_new_data_init(&data);
    data.driver = "Broadcomm BT driver";
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Sink for BSA bluetooth stack"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    if (open_uipc_device(u))
        goto fail;

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    //u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->block_usec = pa_bytes_to_usec(u->bsa_buffer_size, &u->sink->sample_spec) ;
    nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
    pa_sink_set_max_request(u->sink, u->frag_size);
    pa_sink_set_fixed_latency(u->sink, u->block_usec);
    //pa_sink_set_latency_range(u->sink, 0, u->block_usec);

    pa_sink_set_max_rewind(u->sink, nbytes);

    if (!(u->thread = pa_thread_new("bsaa2dp-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    close_uipc_channel(u);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

     pa_xfree(u);
}

static void bsa_userdata_init (struct userdata *u) {

    pa_assert(u);
    memset(u, 0x00, sizeof (struct userdata));

    u->is_channel_opened = FALSE;
    u->frag_size = DEFAULT_FRAG_SIZE;
    u->bsa_buffer_size = 0;
    u->stream_uipc_channel = UIPC_CHANNEL_NUMBER;
}

static int open_uipc_device(struct userdata *u) {
    INT32 avail;

    pa_log_debug("Opening bt device");
    if (u->is_channel_opened == TRUE) {
        pa_log("BT device is already opened");
        return 0;
    }

    UIPC_Init(UIPC_INIT_PATH);
    if (UIPC_Open(u->stream_uipc_channel, NULL) == FALSE) {
        pa_log("Error opening UIPC channel(%d)", u->stream_uipc_channel);
        return -1;
    }
    u->is_channel_opened = TRUE;

    /* Flush the driver, the previous session might still have some data at the driver */
    UIPC_Ioctl(u->stream_uipc_channel, UIPC_REQ_RX_FLUSH, NULL);

    UIPC_Avail(u->stream_uipc_channel, &avail);
    if (avail != -1) {
        pa_log_debug("Available bytes at BSA(%d)", avail);
        u->bsa_buffer_size = (size_t)avail;
    } else {
        pa_log("Error getting avail bytes");
        //FIXME: peeked bsa code to check ringbuffer size
        u->bsa_buffer_size = BSA_BUFFER_SIZE;    /* 200k bsa buffer size */
    }
    pa_log_debug ("BSA Buffer size(%lu)  Fragment size(%lu)", (unsigned long)u->bsa_buffer_size,\
                                                                                    (unsigned long)u->frag_size);

    return 0;
}

static int close_uipc_channel(struct userdata *u) {
    pa_assert(u);

    if (u->is_channel_opened) {
        UIPC_Close(u->stream_uipc_channel);
        u->is_channel_opened = FALSE;
        pa_log_debug("Closed bt device");
        return 0;
    }
    pa_log_debug("uipc channel(%d) is not opened", u->is_channel_opened);
    return -1;
}
