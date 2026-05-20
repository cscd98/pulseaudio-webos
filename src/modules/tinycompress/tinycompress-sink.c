/**********************************************************************
* Copyright (c) 2015 LG Electronics, Inc.
* All rights reserved.
*
* tinycompress-sink.c - pulseaudio sink module which renders
* compress stream to tinycompress device & handle compress stream
* playback
**********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>

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

#include <linux/types.h>
#include <sound/compress_params.h>
#include <sound/compress_offload.h>
#include <tinycompress/tinycompress.h>
#include "sound-normalization.h"
#include "tinycompress-sink.h"

#define DEFAULT_SINK_NAME "tinycompress"
#define BLOCK_USEC (PA_USEC_PER_SEC *2)
#define DEFAULT_FRAG_SIZE (32 * 1024)
#define DEFAULT_FRAGS 4
#define HW_BUFFER_SIZE (DEFAULT_FRAG_SIZE * DEFAULT_FRAGS)
#define INTERNAL_Q_SIZE (32 * 1024 * 2)
#define DEFAULT_BIT_RATE 32000
#define DEFAULT_CHANNEL_IN 2
#define DEFAULT_CHANNEL_OUT 2

#define DUMP_TO_FILE 0
/* #define DEBUG_TIMING */

#if DUMP_TO_FILE
#define DUMP_FILE_PATH "/tmp/dump.dat"
#endif

typedef enum tinycompress_state {
    COMPRESS_OPEN,
    COMPRESS_STARTED,
    COMPRESS_PAUSED,
    COMPRESS_SUSPENDED,
    COMPRESS_DRAIN,
    COMPRESS_CLOSE
} tinycompress_state_t;

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_rtpoll_item *compress_rtpoll_item;
    pa_hook_slot *sink_input_fixate_hook_slot;

    pa_memblockq *memblockq;

    tinycompress_state_t compress_state;

    struct compress *compress_handle;
    struct snd_codec codec;
    struct compr_config config;
    pa_idxset *formats;

    bool drain_flag;
    bool im_buffer_eos;

    pa_usec_t compress_offset_time;

#if DUMP_TO_FILE
    FILE *fd;
#endif
};

struct snd_rate_info {
    unsigned int compress_rate;
    unsigned int snd_rate;
};

static struct snd_rate_info rate_info[] = {
    {5512,    SNDRV_PCM_RATE_5512},
    {8000,    SNDRV_PCM_RATE_8000},
    {11025,    SNDRV_PCM_RATE_11025},
    {12000,    SNDRV_PCM_RATE_12000},
    {16000,    SNDRV_PCM_RATE_16000},
    {22050,    SNDRV_PCM_RATE_22050},
    {24000,    SNDRV_PCM_RATE_24000},
    {32000,    SNDRV_PCM_RATE_32000},
    {44100,    SNDRV_PCM_RATE_44100},
    {48000,    SNDRV_PCM_RATE_48000},
    {64000,    SNDRV_PCM_RATE_64000},
    {88200,    SNDRV_PCM_RATE_88200},
    {96000,    SNDRV_PCM_RATE_96000},
    {176400,    SNDRV_PCM_RATE_176400},
    {192000,    SNDRV_PCM_RATE_192000}
};

static int sink_message_add_input(struct userdata *u, void *data);
static void sink_message_remove_input(struct userdata *u, void *data);
static void sink_message_tinycompress_flush(struct userdata *u);
static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk);
static void set_sink_supported_formats(struct userdata *u, pa_encoding_t encoded_format);
static pa_idxset *sink_get_formats(pa_sink *s);
static int sink_update_rate_cb(pa_sink *s, uint32_t rate);
static void config_hw_tinycompress(struct userdata *u);
static int tinycompress_open_device(struct userdata *u);
static void tinycompress_close_device(struct userdata *u);
static void tinycompress_build_pollfd(struct userdata *u);
static void tinycompress_close_pollfd(struct userdata *u);
static void thread_func(void *userdata);
static void thread_write(void *userdata);
static void thread_renderq(void *userdata);

static int sink_message_add_input(struct userdata *u, void *data) {
    pa_sink_input *si;
    pa_assert(u);
    pa_assert(data);

    si = PA_SINK_INPUT(data);

    if (!pa_sink_input_is_passthrough(si)) {
         pa_log("Tinycompress sink cannot support pcm streams");
         return -1;
    }
    switch (si->format->encoding) {
         case PA_ENCODING_MPEG_IEC61937:
              pa_log_info("Got New MP3 stream");
              u->codec.id = SND_AUDIOCODEC_MP3;
              break;
         case PA_ENCODING_MPEG2_AAC_IEC61937:
              pa_log_info("Got New AAC stream");
              u->codec.id = SND_AUDIOCODEC_AAC;
              break;
         default:
              pa_log_debug("Only MP3 & AAC stream is supported");
              break;
    }
    return 0;
}

static void sink_message_remove_input(struct userdata *u, void *data) {
    pa_sink_input *si;
    pa_assert(u);
    pa_assert(data);

    si = PA_SINK_INPUT(data);

    if (pa_sink_input_is_passthrough(si)) {
         u->compress_offset_time = 0;
         tinycompress_close_device(u);
    }
}

static void sink_message_tinycompress_flush(struct userdata *u) {
    struct timespec ts;
    unsigned int avail;
    pa_assert(u);

    if (u->compress_handle) {
        size_t length;
        if (compress_get_hpointer(u->compress_handle, &avail, &ts) != 0)
            u->compress_offset_time = 0;
        else
            u->compress_offset_time += ((intmax_t)ts.tv_sec * PA_NSEC_PER_SEC +
                                    (intmax_t)ts.tv_nsec) / PA_NSEC_PER_USEC;
        if (u->compress_state != COMPRESS_PAUSED)
            compress_pause(u->compress_handle);
        compress_stop(u->compress_handle);
        u->compress_state = COMPRESS_OPEN;
        length = pa_memblockq_get_length(u->memblockq);
        if (length > 0) {
            pa_log_debug("Dropping %lu from queue", (unsigned long)length);
            pa_memblockq_drop(u->memblockq, length);
        }
    }
}

static int sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    struct userdata *u;
    struct timespec ts;
    unsigned int avail;
    pa_assert(o);
    pa_assert_se(u = PA_SINK(o)->userdata);

    switch(code){
        case PA_SINK_MESSAGE_SET_STATE:

            switch(PA_PTR_TO_UINT(data)) {

                case PA_SINK_SUSPENDED:
                    pa_log_debug("Compress Sink Suspened...");
                    if (u->compress_handle) {
                        if (compress_get_hpointer(u->compress_handle, &avail, &ts) == 0) {
                            u->compress_offset_time += ((intmax_t)ts.tv_sec * PA_NSEC_PER_SEC
                                                        + (intmax_t)ts.tv_nsec) / PA_NSEC_PER_USEC;
                        } else
                            u->compress_offset_time = 0;

                        pa_log("compress_offset_time = %lu", (unsigned long) u->compress_offset_time);
                        u->compress_state = COMPRESS_SUSPENDED;
                        tinycompress_close_device(u);
                    }
                    break;

                case PA_SINK_IDLE:
                    pa_log_debug("Compress Sink Idle...");
                    if (u->compress_handle) {
                        compress_pause(u->compress_handle);
                        u->compress_state = COMPRESS_PAUSED;
                        tinycompress_close_pollfd(u);
                    }
                    break;

                case PA_SINK_RUNNING:
                    pa_log_debug("Compress Sink Running...");
                    if (u->compress_handle && u->compress_state == COMPRESS_PAUSED) {
                        compress_resume(u->compress_handle);
                        u->compress_state = COMPRESS_STARTED;
                        tinycompress_build_pollfd(u);
                    } else if (u->compress_state == COMPRESS_OPEN && (!u->compress_rtpoll_item))
                            tinycompress_build_pollfd(u);
                    /* if compress_state is suspended, device will be opened & started in thread func */
                    break;
            }
            break;

        case PA_SINK_MESSAGE_GET_LATENCY:
            /* We use fixed latency of BLOCK_USEC as we dont have proper latecny
               information for compress streams */
            *((pa_usec_t*)data) = BLOCK_USEC;
            return 0;

        case PA_SINK_MESSAGE_ADD_INPUT:
             pa_log_debug("Sink process message: PA_SINK_MESSAGE_ADD_INPUT");
             if(sink_message_add_input(u,data)== -1)
                 return -1;
             break;

        case PA_SINK_MESSAGE_REMOVE_INPUT:

             pa_log_debug("Sink process message: PA_SINK_MESSAGE_REMOVE_INPUT");
             sink_message_remove_input(u,data);
             break;

        case PA_SINK_MESSAGE_TINYCOMPRESS_FLUSH:

            pa_log_debug("Sink process message: PA_SINK_MESSAGE_TINYCOMPRESS_FLUSH");
            sink_message_tinycompress_flush(u);
            return 0;

        case PA_SINK_MESSAGE_GET_TINYCOMPRESS_TIMESTAMP:

            if (u->compress_handle) {
                if (compress_get_hpointer(u->compress_handle, &avail, &ts) == 0) {
                    *((pa_usec_t*) data) = ((intmax_t)ts.tv_sec * PA_NSEC_PER_SEC +
                                            (intmax_t)ts.tv_nsec) / PA_NSEC_PER_USEC + u->compress_offset_time;
                } else
                    *((pa_usec_t *) data) = 0;

            } else {
                 pa_log_debug("compress device is not opened");
                 *((pa_usec_t *) data) = u->compress_offset_time;
            }
#ifdef DEBUG_TIMING
            pa_log("Playback Time Stamp = %lu", (unsigned long)*((pa_usec_t*) data));
#endif
            return 0;

        case PA_SINK_MESSAGE_TINYCOMPRESS_DRAIN:

            pa_log_debug("Sink process message: PA_SINK_MESSAGE_TINYCOMPRESS_DRAIN");
            if (u->compress_handle) {
                u->im_buffer_eos = false;
                u->drain_flag = true;
            }
            return 0;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

unsigned int tinycompress_get_rate(unsigned int rate)
{
    int i;
    for(i = 0; i < PA_ELEMENTSOF(rate_info); i++) {
        if (rate == rate_info[i].compress_rate)
            return rate_info[i].snd_rate;
    }

    return SNDRV_PCM_RATE_44100;
}

static pa_hook_result_t sink_input_fixate_hook_callback(pa_core *c, pa_sink_input_new_data *i, struct userdata *u) {

    const char *sink_name;

    pa_assert(i);
    pa_assert(u);

    if (pa_sink_input_new_data_is_passthrough(i)) {

        sink_name = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);
        pa_assert(sink_name);

        if (!pa_streq(sink_name, "pulsesink probe")) {
            int rate;
            if (pa_format_info_get_prop_int(i->format, PA_PROP_FORMAT_RATE, &rate)) {
                pa_log("Sample rate is not present in the compress stream");
                return PA_HOOK_CANCEL;
            }
            u->codec.sample_rate = tinycompress_get_rate(rate);
            pa_log_debug("Sample rate of the compress stream is %d codec.sample_rate %d", rate, u->codec.sample_rate);
        }
    }

    return PA_HOOK_OK;
}

static void set_sink_supported_formats(struct userdata *u, pa_encoding_t encoded_format) {

    pa_format_info *format;
    pa_assert(u);
    pa_assert(u->formats);

    format = pa_format_info_new();
    format->encoding = encoded_format;
    pa_idxset_put(u->formats, format, NULL);
}

static pa_idxset *sink_get_formats(pa_sink *s) {

    struct userdata *u = NULL;
    pa_idxset *ret = NULL;
    pa_format_info *f;
    uint32_t idx;

    pa_assert(s);

    u = s->userdata;
    pa_assert(u);

    ret = pa_idxset_new(NULL, NULL);

    PA_IDXSET_FOREACH(f, u->formats, idx) {
        pa_idxset_put(ret, pa_format_info_copy(f), NULL);
    }

    return ret;
}


static int sink_update_rate_cb(pa_sink * s, uint32_t rate) {
// TODO: is this require ??
    struct userdata *u = NULL;
    pa_assert(s);

    u = s->userdata;
    pa_assert(u);

    if (!PA_SINK_IS_OPENED(s->state)) {
        pa_log_debug("Updating device rate to %d", rate);
        u->sink->sample_spec.rate = rate;
        return 0;
    }

    return -1;
}

static void tinycompress_close_pollfd(struct userdata *u) {

    pa_assert(u);
    pa_assert(u->compress_rtpoll_item);
    pa_log_debug("Removing Compress fd from rtpoll item");

    pa_rtpoll_item_free(u->compress_rtpoll_item);
    u->compress_rtpoll_item = NULL;
}

static void tinycompress_build_pollfd(struct userdata *u) {

    struct pollfd *pollfd;

    pa_assert(u);
    pa_assert(!u->compress_rtpoll_item);
    pa_assert(u->compress_handle);
    pa_log_debug("Get Compress fd & add to rtpoll item");

    u->compress_rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);

    pollfd = pa_rtpoll_item_get_pollfd(u->compress_rtpoll_item, NULL);
    pollfd->fd = compress_get_file_descriptor(u->compress_handle);
    pollfd->events = POLLOUT;
    pollfd->revents = 0;
}

static void tinycompress_close_device(struct userdata *u) {

    size_t length;
    unsigned int avail;
    struct timespec ts;
    int ret = 0;
    pa_assert(u);

    if(u->compress_state == COMPRESS_SUSPENDED){
        if (compress_get_hpointer(u->compress_handle, &avail, &ts) == 0){
            length = HW_BUFFER_SIZE - avail;
            pa_memblockq_rewind(u->memblockq,length);
        }
    }
    else{
        length = pa_memblockq_get_length(u->memblockq);
        if (length > 0) {
            pa_log_debug("Dropping %lu from queue", (unsigned long)length);
            pa_memblockq_drop(u->memblockq, length);
        }
    }
    if (u->compress_handle) {
        pa_log_debug("Closing Compress Device");
        compress_stop(u->compress_handle);
        compress_close(u->compress_handle);
        u->compress_handle = NULL;
    }

    if (u->compress_rtpoll_item)
        tinycompress_close_pollfd(u);

    u->compress_state = COMPRESS_CLOSE;
    ret = adev_set_parameters(false);
    if (ret >= 0)
        pa_log_debug("normalization parameters successfullly disabled\n");
}

static void config_hw_tinycompress(struct userdata *u) {

    pa_assert(u);
    u->codec.id = SND_AUDIOCODEC_MP3;
    u->codec.ch_in = DEFAULT_CHANNEL_IN;
    u->codec.ch_out = DEFAULT_CHANNEL_OUT;
    u->codec.sample_rate = SNDRV_PCM_RATE_44100;
    u->codec.bit_rate = DEFAULT_BIT_RATE;
    u->codec.rate_control = 0;
    u->codec.profile = 0;
    u->codec.level = 0;
    u->codec.ch_mode = 0;
    u->codec.format = 0;
    u->config.fragment_size = DEFAULT_FRAG_SIZE;
    u->config.fragments = DEFAULT_FRAGS;
    u->config.codec = &(u->codec);
}

static int tinycompress_open_device(struct userdata *u) {

    int ret = 0;
    pa_assert(u);
    pa_log_debug("Opening Compress Device");

    if (u->compress_handle) {
        pa_log("Device already opened");
        return -1;
    }

    u->compress_handle = compress_open(0, 9, COMPRESS_IN, &u->config);
    if (!u->compress_handle) {
        pa_log("Failed to open Compress device");
        return -1;
    }
    if (!is_compress_ready(u->compress_handle)) {
        pa_log("Error : %s", compress_get_error(u->compress_handle));
        u->compress_handle = NULL;
        return -1;
    }

    if (!u->compress_rtpoll_item)
        tinycompress_build_pollfd(u);

    u->compress_state = COMPRESS_OPEN;
    ret = adev_set_parameters(true);
    if(ret >= 0)
        pa_log_debug("normalization parameters successfullly applied\n");

    return 0;
}

static void thread_renderq(void *userdata) {

     struct userdata *u = userdata;
     size_t length;
     size_t missing;

     pa_assert(u);
     pa_assert(u->memblockq);

     length = pa_memblockq_get_length(u->memblockq);
     if (length >= INTERNAL_Q_SIZE) {
#ifdef DEBUG_TIMING
         pa_log("MemblockQ is full");
#endif
         if (!u->compress_handle) {
             if (tinycompress_open_device(u) < 0)
                 pa_log_debug("This case will happen only after device suspend");
         }
         return;
     }

     missing = INTERNAL_Q_SIZE - length;

    /* fill empty data in internal memblockq */
     for(;;) {
         pa_memchunk chunk;

         if (missing == 0)
             break;

         pa_sink_render(u->sink, missing, &chunk);

        /* check if chunk is silence or not */
         if (pa_memblock_is_silence(chunk.memblock)) {
#ifdef DEBUG_TIMING
             pa_log("Got silence");
#endif
             pa_memblock_unref(chunk.memblock);
             break;
         }

#ifdef DEBUG_TIMING
         pa_log("Asked = %lu, Got = %lu", (unsigned long)missing, (unsigned long)chunk.length);
#endif
         missing -= chunk.length;
         pa_memblockq_push_align(u->memblockq, &chunk);
         pa_memblock_unref(chunk.memblock);
     }

     /* open device when internal memblockq is full */
     if (!u->compress_handle) {
         length = pa_memblockq_get_length(u->memblockq);
         if (length == INTERNAL_Q_SIZE) {
             if (tinycompress_open_device(u) < 0)
                 pa_log("failed to open tinycompress device");
         }
     }
}

static void thread_write(void *userdata) {

    struct userdata *u = userdata;
    pa_memchunk chunk;
    const void *p;
    size_t length;
    struct timespec ts;
    unsigned int avail = 0;
    int ate;

    length = pa_memblockq_get_length(u->memblockq);
    length = PA_MIN(length, DEFAULT_FRAG_SIZE);

    if (length < DEFAULT_FRAG_SIZE && u->drain_flag == false) {
#ifdef DEBUG_TIMING
        pa_log("Internal queue contains(%lu) less than fragment size", (unsigned long)length);
#endif
        return;
    }

    if (u->compress_handle && compress_get_hpointer(u->compress_handle, &avail, &ts) == 0) {
        if (avail  < DEFAULT_FRAG_SIZE) {
            pa_log_debug(" HW buffer is less than frag size");
            return;
        }

#ifdef DEBUG_TIMING
        pa_log("length(%lu) Avail %lu", (unsigned long)length, (unsigned long)avail);
#endif

        if (u->drain_flag == true && length < DEFAULT_FRAG_SIZE) {
#ifdef DEBUG_TIMING
            pa_log("Drain is enabled & memblockq length is less than frag size");
            pa_log("Wait for HW to consume till last frags");
#endif
            if (avail > (HW_BUFFER_SIZE - DEFAULT_FRAG_SIZE)) {
                pa_log_debug("Drain Internal Memblock");
                u->im_buffer_eos = true;
                if (length == 0)
                    goto drain_hw_buffer;
                else
                    goto drain_queue;
            }
            return;
        }
    } else {
        pa_log("Error in getting avail (%s)", compress_get_error(u->compress_handle));
        return;
    }

drain_queue:

    pa_memblockq_peek_fixed_size(u->memblockq, length, &chunk);
    p = pa_memblock_acquire(chunk.memblock);

#if DUMP_TO_FILE
    if (u->fd) {
        ate = fwrite(((uint8_t*)p + chunk.index), 1, chunk.length, u->fd);
        fflush(u->fd);
    } else
        pa_log("Error : file is not opened");
#endif

    /* Write the data into device */
    ate = c_write(u->compress_handle, (uint8_t*)p + chunk.index, chunk.length);

#ifdef DEBUG_TIMING
    pa_log("Writing(%lu), device ate(%d)", (unsigned long)length, ate);
#endif

    if (ate < 0)
        pa_log("Compress write error(%s)", compress_get_error(u->compress_handle));

    pa_memblockq_drop(u->memblockq, chunk.length);
    pa_memblock_release(chunk.memblock);
    pa_memblock_unref(chunk.memblock);

    /* Start playback once */
    if (u->compress_state == COMPRESS_OPEN) {
        pa_log_debug("Starting Compress playback");
        compress_start(u->compress_handle);
        u->compress_state = COMPRESS_STARTED;
    }

drain_hw_buffer:
    /* Drain the remaining buffer in HW */
    if (u->drain_flag == true && u->im_buffer_eos == true) {
        if (u->compress_handle) {
            pa_log_debug("Drain the hardware buffer");
            compress_drain(u->compress_handle);
            u->drain_flag = false;
            u->im_buffer_eos = false;
        }
    }

}

static void thread_func(void *userdata) {

    struct userdata *u = userdata;
    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    for(;;){
        int ret;
        struct pollfd *pollfd = NULL;

        if(u->compress_rtpoll_item)
            pollfd = pa_rtpoll_item_get_pollfd(u->compress_rtpoll_item, NULL);

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)){
            pa_log_debug("rewind requested");
            pa_sink_process_rewind(u->sink, 0);
        }

        if (PA_SINK_IS_RUNNING(u->sink->thread_info.state)){
            /* Try writing data into the device for pollfd event */
            if (u->compress_handle && pollfd && pollfd->revents)
                thread_write(u);

            /* Fill Internal memblockq */
            thread_renderq(u);
        }

        if (pollfd)
            pollfd->events = (short)(u->sink->thread_info.state == PA_SINK_RUNNING ? POLLOUT : 0);

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

static void userdata_free(struct userdata *u) {

    pa_assert(u);

#if DUMP_TO_FILE
    if (u->fd) {
        fclose(u->fd);
        u->fd = NULL;
    }
#endif

    if (u->compress_handle) {
        compress_close(u->compress_handle);
        u->compress_state = COMPRESS_CLOSE;
        u->compress_handle = NULL;
    }

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->memblockq)
        pa_memblockq_free(u->memblockq);

    if (u->sink_input_fixate_hook_slot)
        pa_hook_slot_free(u->sink_input_fixate_hook_slot);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    if (u->formats)
        pa_idxset_free(u->formats, (pa_free_cb_t) pa_format_info_free);

    pa_xfree(u);
}

pa_sink *pa_tinycompress_sink_new(pa_module * m, pa_modargs * ma, const char *driver) {

    struct userdata *u = NULL;
    pa_sink_new_data data;
    pa_sample_spec ss;
    pa_channel_map map;

    pa_assert(m);
    pa_assert(ma);

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;

    /* Added to support bpf = 1 */
    ss.format = PA_SAMPLE_U8;
    ss.channels = 1;
    map.channels = 1;

    /* memory allocation for  userdata */
    m->userdata =  u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->compress_handle = NULL;
    u->compress_state = COMPRESS_CLOSE;
    u->compress_offset_time = 0;
    u->compress_rtpoll_item = NULL;
    u->drain_flag = false;
    u->im_buffer_eos = false;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, u->core->mainloop, u->rtpoll);

    /* Initialize sink data */
    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;

    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Sink for Passthrough Output");
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "sound");

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY | PA_SINK_SET_FORMATS);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create tinycompress sink object");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    /* Initialize internal memblockq */
    u->memblockq = pa_memblockq_new("tinycompress-memblockq", 0, HW_BUFFER_SIZE, INTERNAL_Q_SIZE, &ss, 0, 1, HW_BUFFER_SIZE, &u->sink->silence);

    pa_sink_set_max_rewind(u->sink, 0);
    pa_sink_set_max_request(u->sink, HW_BUFFER_SIZE); /*TODO: need to check the max size */
    /*TODO: need to check the usage of latency in compress case*/
    pa_sink_set_fixed_latency(u->sink, BLOCK_USEC);

    /* Add  supported formats */
    u->formats = pa_idxset_new(NULL, NULL);
    set_sink_supported_formats(u, PA_ENCODING_MPEG_IEC61937);
    set_sink_supported_formats(u, PA_ENCODING_MPEG2_AAC_IEC61937);

    /* sink input fixate callback to update compress rate information */
    u->sink_input_fixate_hook_slot = pa_hook_connect(&m->core->hooks[PA_CORE_HOOK_SINK_INPUT_FIXATE],
                                      PA_HOOK_EARLY, (pa_hook_cb_t)sink_input_fixate_hook_callback, u);

    u->sink->get_formats = sink_get_formats;
    u->sink->update_rate = sink_update_rate_cb;

    /* configure HW parameter */
    config_hw_tinycompress(u);

#if DUMP_TO_FILE
    u->fd = fopen(DUMP_FILE_PATH, "wb");
    if (!u->fd) {
        pa_log("Unable to open file discriptor to dump data");
    } else
        pa_log_debug("Opened file for dump");
#endif

    /* Starting I/O thread */
    if (!(u->thread = pa_thread_new("tinycompress-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

    return u->sink;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (u)
        userdata_free(u);

    return NULL;
}

void pa_tinycompress_sink_free(pa_sink * s) {

    struct userdata *u;

    pa_assert_se(u = s->userdata);

    userdata_free(u);
}
