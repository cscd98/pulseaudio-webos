/**********************************************************************
* Copyright (c) 2015 LG Electronics, Inc.
* All rights reserved.
*
* module-tinycompress-sink.c - pulseaudio sink module which renders
* compress stream to tinycompress device & handle compress stream
* playback
**********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>

#include "tinycompress-sink.h"

PA_MODULE_AUTHOR("Sukesh Adiga, Rajitha Vemuri");
PA_MODULE_DESCRIPTION("Passthrough  Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE("sink_name=<name for the sink> "
                "device=<Tiny Compress  device> "
                "format=<sample format> "
                "rate=<sample rate> "
                "channels=<number of channels> "
                "channel_map=<channel map> ");

static const char *const valid_modargs[] = {
    "sink_name",
    "device",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};

int pa__init(pa_module * m) {

    pa_modargs *ma = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse tinycompress module arguments");
        goto fail;
    }

    if (!(m->userdata = pa_tinycompress_sink_new(m, ma, "TinyCompress"))) {
        goto fail;
    }

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

void pa__done(pa_module * m) {

    pa_sink *sink;

    pa_assert(m);

    if ((sink = m->userdata))
        pa_tinycompress_sink_free(sink);
}
