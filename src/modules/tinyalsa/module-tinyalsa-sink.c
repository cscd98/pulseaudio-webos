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

#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/modargs.h>

#include "tinyalsa-sink.h"


PA_MODULE_AUTHOR("Rajitha Vemuri");
PA_MODULE_DESCRIPTION("TINY ALSA Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
    "sink_name=<name for the sink> "
    "device=<TINY ALSA device> "
    "format=<sample format> "
    "rate=<sample rate> "
    "channels=<number of channels> "
    "channel_map=<channel map> "
   );

static const char *const valid_modargs[] ={

    "sink_name",
    "device",
    "format",
    "rate",
    "channels",
    "channel_map",
    NULL
};

int pa__init(pa_module *m){

    pa_modargs *ma = NULL;

    pa_assert(m);


    if (!(ma = pa_modargs_new(m->argument, valid_modargs))){
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(m->userdata = pa_tinyalsa_sink_new(m, ma, "Tinyalsa"))){
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

void pa__done(pa_module *m){

   pa_sink *sink;

    pa_assert(m);

    if ((sink = m->userdata))
            pa_tinyalsa_sink_free(sink);
}


