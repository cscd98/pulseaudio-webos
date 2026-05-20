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

#include <signal.h>
#include <stdio.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core.h>
#include <pulsecore/i18n.h>
#include <pulsecore/module.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/source.h>
#include <pulsecore/modargs.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-util.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

//#include <tinyalsa/asoundlib.h>
#include "tinyalsa-source.h"

#define DEFAULT_SOURCE_NAME "pcm_input"
#define DEFAULT_FRAGMENT_SIZE 1024    /* default fragment size */

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    size_t hw_buffer_size;
    size_t fragment_size;
    size_t frame_size;
    int nfrags;

    pa_usec_t timestamp;

    struct pcm *pcm;
    int card;
    int device;
};

static void userdata_free(struct userdata *u);
static void config_hw_tinyalsa(struct userdata *u, struct pcm_config *config, pa_sample_spec *ss, pa_channel_map *map);
static struct pcm *open_tinyalsa_device(struct userdata *u, pa_sample_spec *ss, pa_channel_map *map);
static int thread_read(struct userdata *u);
static void thread_func(void *userdata);
static bool parse_device_name(struct userdata *u, const char *dev_name);
static int suspend(struct userdata *u);
static int unsuspend(struct userdata *u);
static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk);


static int suspend(struct userdata *u) {

    pa_assert(u);

    pa_log_debug("Suspend....");

    if (u->pcm) {
        pcm_close(u->pcm);
        u->pcm = NULL;
    }

    return 0;
}

static int unsuspend(struct userdata *u) {

    pa_assert(u);
    pa_assert(!u->pcm);

    pa_log_debug("Unsuspend....");

    u->pcm = open_tinyalsa_device(u, &u->module->core->default_sample_spec, &u->module->core->default_channel_map);

    if (!u->pcm) {
        pa_log("Failed to open tinyalsa device");
	return -1;
    }

    return 0;
}

static int source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    struct userdata *u = PA_SOURCE(o)->userdata;
    int ret;

    pa_assert(u);

    switch(code) {

        case PA_SOURCE_MESSAGE_SET_STATE:

            switch(PA_PTR_TO_UINT(data)) {

                case PA_SOURCE_SUSPENDED:
                    if ((ret = suspend(u)) < 0)
                        return ret;
                    break;

                case PA_SOURCE_IDLE:
                case PA_SOURCE_RUNNING:
                    if (u->source->thread_info.state == PA_SOURCE_SUSPENDED) {
                        if ((ret = unsuspend(u)) < 0)
                            return ret;
                        pa_rtpoll_set_timer_absolute(u->rtpoll, pa_rtclock_now());
                    }
                    break;
            }
            break;
        /* Latency is not set for source so returning 0 */
        case PA_SOURCE_MESSAGE_GET_LATENCY:
            return 0;

    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static int thread_read(struct userdata *u) {

    void *p;
    pa_memchunk chunk;
    int ret;

    pa_assert(u);
    pa_assert(u->pcm);

    chunk.memblock = pa_memblock_new (u->core->mempool, (size_t)u->hw_buffer_size);
    p = pa_memblock_acquire(chunk.memblock);

    /* capture data from tinayalsa device */
    ret = pcm_read(u->pcm, p, u->hw_buffer_size);
    pa_memblock_release(chunk.memblock);

    if (ret < 0) {
        pa_log("pcm_read error(%s)", pcm_get_error(u->pcm));
        goto fail;
    }

    u->timestamp += pa_bytes_to_usec(u->hw_buffer_size, &u->source->sample_spec);
    chunk.index = 0;
    chunk.length = u->hw_buffer_size;

    if (chunk.length > 0) {
        pa_source_post (u->source, &chunk);
        pa_log_debug("Pushing memchunk of length(%ld)", (unsigned long)u->hw_buffer_size);
    } else
        pa_log_debug("Not pushing data, as length is 0");

fail:
    pa_memblock_unref (chunk.memblock);

    return ret;
}

static void thread_func(void *userdata) {

    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    if (u->core->realtime_scheduling)
        pa_make_realtime(u->core->realtime_priority);

    for(;;) {
        int ret;

        /* Read some data and pass it to the sources */
        if (PA_SOURCE_IS_OPENED(u->source->thread_info.state)) {

            u->timestamp = pa_rtclock_now();

            thread_read(u);

            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else
            pa_rtpoll_set_timer_disabled(u->rtpoll);

        /* Sleep */
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

static bool parse_device_name(struct userdata *u, const char *dev_name) {

    bool ret = false;

    pa_assert(u);

    if (!dev_name)
        return ret;

    u->card = -1;
    u->device = -1;
    if (pa_streq(dev_name, "hw:0,0")) {
        u->card = 0;
        u->device = 0;
        ret = true;
    } else if (pa_streq(dev_name, "hw:0,15")) {
        u->card = 0;
        u->device = 15;
        ret = true;
    }

    return ret;
}

static void config_hw_tinyalsa(struct userdata *u, struct pcm_config *config, pa_sample_spec *ss, pa_channel_map *map) {

    pa_assert(u);
    pa_assert(config);

    config->channels = ss->channels;
    config->rate = ss->rate;
    config->period_size = u->fragment_size;
    config->period_count = u->nfrags;
    config->format = PCM_FORMAT_S16_LE;

    config->avail_min = config->period_size / 2;
    config->start_threshold = 1;
    config->stop_threshold = INT_MAX;
    config->silence_threshold = 0;

    pa_log_debug("Driver rate(%d)", config->rate);
    pa_log_debug("Driver period_size(%d)", config->period_size);
    pa_log_debug("Driver period count(%d)", config->period_count);
}

static struct pcm *open_tinyalsa_device(struct userdata *u, pa_sample_spec *ss, pa_channel_map *map) {

    struct pcm *pcm = NULL;
    struct pcm_config config;

    pa_assert(u);
    pa_assert(ss->channels == 1 || ss->channels == 2);

    pa_log_debug("Requested channels %d, rate = %d period_size %lu period_cnt %lu", ss->channels, ss->rate,
                      (unsigned long)u->fragment_size, (unsigned long)u->nfrags);

    config_hw_tinyalsa(u, &config, ss, map);

    pcm = pcm_open(u->card, u->device, PCM_IN, &config);

    if (!pcm || !pcm_is_ready(pcm)) {
        pa_log("Unable to open device (%s)", pcm_get_error(pcm));
        if (pcm)
            pcm_close(pcm);
        return NULL;
    }

    u->hw_buffer_size = pcm_get_buffer_size(pcm);

    pa_log_debug("Driver hw_buffer_size set (%lu)", (unsigned long)u->hw_buffer_size);
    return pcm;
}

pa_source *pa_tinyalsa_source_new(pa_module *m, pa_modargs *ma, const char *driver, pa_card *card) {

    struct userdata *u = NULL;
    pa_sample_spec sample_spec;
    pa_channel_map channel_map;
    pa_source_new_data data;
    uint32_t frag_size, nfrags;
    size_t frame_size;
    uint32_t alternate_sample_rate;
    const char *device_name = NULL;

    pa_assert(m);
    pa_assert(ma);
    pa_assert(driver);

    sample_spec = m->core->default_sample_spec;
    channel_map = m->core->default_channel_map;

    if (pa_modargs_get_sample_spec_and_channel_map(ma, &sample_spec, &channel_map, PA_CHANNEL_MAP_AIFF) < 0) {
        pa_log("Failed to parse sample specification and channel map.");
        goto fail;
    }

    alternate_sample_rate = m->core->alternate_sample_rate;
    if (pa_modargs_get_alternate_sample_rate(ma, &alternate_sample_rate) < 0) {
        pa_log("Failed to parse alternate sample rate.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    frame_size = pa_frame_size(&sample_spec);

    nfrags = m->core->default_n_fragments;
    frag_size = DEFAULT_FRAGMENT_SIZE;
    if (pa_modargs_get_value_u32(ma, "fragments", &nfrags) < 0 ||
        pa_modargs_get_value_u32(ma, "fragment_size", &frag_size) < 0) {
        pa_log("Failed to parse fragments metrics");
        goto fail;
    }
    u->frame_size = frame_size;
    u->nfrags = nfrags;
    u->fragment_size = frag_size;

    device_name = pa_modargs_get_value(ma, "device", NULL);
    if (!device_name || !parse_device_name(u, device_name)) {
        pa_log("Failed to get device name");
        goto fail;
    }

    pa_log_debug("Device name %s", device_name);
    pa_log_debug("No of Fragments %ld", (unsigned long)u->nfrags);
    pa_log_debug("Frame size %ld", (unsigned long)u->frame_size);
    pa_log_debug("Fragments size %ld", (unsigned long)u->fragment_size);
    pa_log_debug("sample_spec.rate %d", sample_spec.rate);
    pa_log_debug("sample_spec.channels %d", sample_spec.channels);
    pa_log_debug("sample_spec.format %d", sample_spec.format);

    /* Open tinyalsa source device */
    u->pcm = open_tinyalsa_device(u, &sample_spec, &channel_map);
    if (!u->pcm) {
        pa_log("Failed to open tinyalsa device");
        goto fail;
    }

    pa_source_new_data_init(&data);
    data.driver = driver;
    data.module = m;

    pa_source_new_data_set_name(&data, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_source_new_data_set_sample_spec(&data, &sample_spec);
    pa_source_new_data_set_channel_map(&data, &channel_map);
    pa_source_new_data_set_alternate_sample_rate(&data, alternate_sample_rate);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, pa_modargs_get_value(ma, "description", "Tiny alsa source"));

    u->source = pa_source_new(m->core, &data, PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data);

    if (!u->source) {
        pa_log("Failed to create source object.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
    //u->source->update_requested_latency = source_update_requested_latency_cb;
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);

    pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(u->hw_buffer_size, &sample_spec));
    pa_log_debug("Set fixed latency %" PRIu64 " usec", pa_bytes_to_usec(u->hw_buffer_size, &sample_spec));

    /* Disable rewind for droid source */
    pa_source_set_max_rewind(u->source, 0);

    if (!(u->thread = pa_thread_new("tinyalsa-source", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);

    return u->source;

fail:
    userdata_free(u);

    return NULL;
}

static void userdata_free(struct userdata *u) {

    pa_assert(u);

    if (u->pcm) {
        pcm_close(u->pcm);
        u->pcm = NULL;
    }

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source)
        pa_source_unref(u->source);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_xfree(u);
}

void pa_tinyalsa_source_free(pa_source *s) {
    struct userdata *u;

    pa_source_assert_ref(s);
    pa_assert_se(u = s->userdata);

    userdata_free(u);
}
