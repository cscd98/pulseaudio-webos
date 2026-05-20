/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

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
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
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
//#include <tinyalsa/asoundlib.h>

#include "tinyalsa-sink.h"

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;
    pa_memchunk memchunk;
    struct pcm *pcm_handle;
    pa_usec_t timestamp;
    pa_usec_t buffer_latency;
    int  card, device;
    size_t frame_size,
        hwbuf_size,
        nfrags;
};

#define DEFAULT_SINK_NAME "tinyalsa"
#define DEFAULT_FRAG_SIZE   (1024 ) //this is in terms of frames


static int suspend(struct userdata *u);
static int unsuspend(struct userdata *u);
static pa_usec_t tinyalsa_sink_get_latency(struct userdata *u);
static int sink_process_msg(pa_msgobject *o, int code,
                            void *data, int64_t offset,
                            pa_memchunk *chunk);
static void userdata_free(struct userdata *u);

static void process_render(struct userdata *u);
static void thread_func(void *userdata);
static struct pcm *tinyalsa_open_device(struct userdata *u, pa_sample_spec *ss,
        pa_channel_map *map, size_t periodSize, size_t periodCount);
static bool parse_device_name(const char *devName, struct userdata *u);


static int suspend(struct userdata *u){

    pa_assert(u);
    pa_assert(u->pcm_handle);

    pa_log_debug("Suspending sink....");

    pcm_close(u->pcm_handle);
    u->pcm_handle = NULL;
    return 0;
}

static int unsuspend(struct userdata *u){

    pa_assert(u);
    pa_assert(!u->pcm_handle);

    pa_log_debug("Trying to resume....");

    u->pcm_handle = tinyalsa_open_device(u, &u->module->core->default_sample_spec,
                                            &u->module->core->default_channel_map,
                                            u->hwbuf_size / u->frame_size,
                                            u->nfrags);

    if (!u->pcm_handle){
        pa_log("Unable to open alsa device");
        goto fail;
    }

    return 0;

fail:

    if (u->pcm_handle){
        pcm_close(u->pcm_handle);
        u->pcm_handle = NULL;
    }

    return -PA_ERR_IO;
}

static pa_usec_t tinyalsa_sink_get_latency(struct userdata *u){

    pa_usec_t r;
    pa_assert(u);

    r = pa_bytes_to_usec(u->hwbuf_size, &u->sink->sample_spec);
    return r;
}

static int sink_process_msg(pa_msgobject *o, int code,
                            void *data, int64_t offset,
                            pa_memchunk *chunk){

    struct userdata *u = PA_SINK(o)->userdata;
    int r;

    switch (code){
        case PA_SINK_MESSAGE_SET_STATE:
            switch (PA_PTR_TO_UINT(data)){
                 case PA_SINK_SUSPENDED:
                    pa_log_debug("set sink to suspended state");
                    suspend(u);
                    break;

                case PA_SINK_IDLE:
                    if (u->pcm_handle)
                        pcm_stop(u->pcm_handle);
                case PA_SINK_RUNNING:
                    if (u->sink->thread_info.state == PA_SINK_SUSPENDED){
                        r = unsuspend(u);
                        if (r < 0)
                             return r;
                    }
                    pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
                    break;
            }

            break;

        case PA_SINK_MESSAGE_GET_LATENCY:
            *((pa_usec_t *) data) = tinyalsa_sink_get_latency(u);
            return 0;
      }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void userdata_free(struct userdata *u){

    pa_assert(u);

    if (u->pcm_handle){
        pcm_close(u->pcm_handle);
        u->pcm_handle= NULL;
    }

    if (u->sink){
        pa_sink_unlink(u->sink);
    }

    if (u->thread){
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
         pa_sink_unref(u->sink);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_xfree(u);
}

static void process_render(struct userdata *u){

    uint8_t *p;
    int ret;

    pa_assert(u);

    pa_sink_render_full(u->sink,u->hwbuf_size, &u->memchunk);
    pa_assert(u->memchunk.length > 0);
    p = pa_memblock_acquire(u->memchunk.memblock);

    if (!pa_memblock_is_silence(u->memchunk.memblock)) {
        ret = pcm_write(u->pcm_handle, p + u->memchunk.index, u->memchunk.length);

        if (ret < 0)
            pa_log("Error writing data to device(%d)", ret);
    }

    pa_memblock_release(u->memchunk.memblock);
    pa_memblock_unref(u->memchunk.memblock);
    pa_memchunk_reset(&u->memchunk);
}

static void thread_func(void *userdata){

    struct userdata *u = userdata;
    pa_usec_t now;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    pa_thread_mq_install(&u->thread_mq);

    for (;;){
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)){

            u->timestamp = pa_rtclock_now();

            if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
                pa_sink_process_rewind(u->sink, 0);

            if (pa_rtpoll_timer_elapsed(u->rtpoll)){
                process_render(u);

                now = pa_rtclock_now();
                if (now - u->timestamp > u->buffer_latency)
                    u->timestamp = 0;
                else
                    u->timestamp = ((u->buffer_latency - (now - u->timestamp))* 3) /4;

                pa_rtpoll_set_timer_relative(u->rtpoll, u->timestamp);
            }
        }
        else
           pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Hmm, nothing to do. Let's sleep */
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

struct pcm *tinyalsa_open_device(struct userdata *u, pa_sample_spec *ss,
                                    pa_channel_map *map, size_t periodSize,
                                    size_t periodCount){

    struct pcm *pcmHandle = NULL;
    struct pcm_config config;

    pa_assert(u);

    pa_log_debug("Requested channels %d, rate = %d period_size %lu period_cnt %lu", ss->channels, ss->rate,
           (unsigned long)periodSize,
           (unsigned long)periodCount);

    pa_assert(ss->channels == 1 || ss->channels == 2);

    config.channels = ss->channels;
    config.rate = ss->rate;
    config.period_size = periodSize;
    config.period_count = periodCount;
    config.format = PCM_FORMAT_S16_LE;
    config.start_threshold = periodSize ;
    config.stop_threshold = INT_MAX;
    config.silence_threshold = 0;
    config.avail_min = periodSize;

    if (!(pcmHandle = pcm_open(u->card, u->device, PCM_OUT, &config))){
        pa_log("unable to open pcm device");
        return NULL;
    }
    if (!pcm_is_ready(pcmHandle)) {
        pa_log("pcm device is not ready");
        pcm_close(pcmHandle);
        return NULL;
    }

    u->frame_size = pa_frame_size(ss);
    u->hwbuf_size = config.period_size * u->frame_size;
    u->nfrags = u->frame_size;

    pa_log_debug("Got period_size %lu period_cnd %lu", (unsigned long)config.period_size,
         (unsigned long)config.period_count);
    pa_log_debug("hardware buffer size %lu", (unsigned long)u->hwbuf_size);
    pa_log_debug("Number of fragmentts %lu", (unsigned long)u->nfrags);


    return pcmHandle;

}

static bool parse_device_name(const char *devName, struct userdata *u){

    bool ret = false;
    pa_assert(u);

    if (!devName)
        return false;

    u->card = 0;

    if (pa_streq(devName, "hw:0,0")){
        u->device = 0;
        ret = true;
    }
    else if (pa_streq(devName, "hw:0,15")){
        u->device = 0;
        ret = true;
    }
    else{
        u->device = -1;
        u->card = -1;
    }
    return ret;
}

pa_sink *pa_tinyalsa_sink_new(pa_module *m, pa_modargs *ma, const char *driver){

    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_sink_new_data data;
    uint32_t nfrags, fragSize;
    const char *devName = NULL;

    pa_assert(m);
    pa_assert(ma);

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    nfrags = m->core->default_n_fragments;
    fragSize = DEFAULT_FRAG_SIZE;

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0){
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);
    pa_sink_new_data_init(&data);
    data.driver = driver;
    data.module = m;
    devName = pa_modargs_get_value(ma, "device", NULL);

    if (!devName || !parse_device_name(devName, u)){
        pa_log("Failed to get device name");
        goto fail;
    }

    if (!(u->pcm_handle = tinyalsa_open_device(u, &ss, &map,fragSize, nfrags))){
        pa_log("Failed to open pcm device");
        goto fail;
    }

    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Tiny Alsa Output"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract");

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink){
        pa_log("Failed to create sink object");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->userdata = u;
    pa_memchunk_reset(&u->memchunk);
    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->buffer_latency = pa_bytes_to_usec(u->hwbuf_size, &ss);
    pa_sink_set_max_rewind(u->sink, u->hwbuf_size);
    pa_sink_set_max_request(u->sink, u->hwbuf_size);
    pa_sink_set_fixed_latency(u->sink, u->buffer_latency);

    if (!(u->thread = pa_thread_new("tinyalsa-sink", thread_func, u))){
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);
    return u->sink;

fail:
    if (u)
        userdata_free(u);

    return NULL;
}

void pa_tinyalsa_sink_free(pa_sink *s){

    struct userdata *u;

    pa_assert_se(u = s->userdata);

    userdata_free(u);
}
