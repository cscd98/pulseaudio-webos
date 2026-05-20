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

#include <stdio.h>

#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "tinyalsa-source.h"

PA_MODULE_AUTHOR("Sukesh Adiga");
PA_MODULE_DESCRIPTION("Tinyalsa Source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "device=<Tinyalsa device> "
        "format=<sample format> "
        "fragments=<number of fragments> "
        "fragment_size=<size of fragments> "
        "rate=<sample rate> "
        "alternate_rate=<alternate sample rate> "
        "channels=<number of channels> ");

static const char* const valid_modargs[] = {
    "source_name",
    "device",
    "format",
    "fragments",
    "fragment_size",
    "rate",
    "alternate_rate",
    "channels",
    NULL
};

int pa__init(pa_module *m) {
    pa_modargs *ma = NULL;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    if (!(m->userdata = pa_tinyalsa_source_new(m, ma, __FILE__, NULL)))
        goto fail;

    pa_modargs_free(ma);

    return 0;

fail:

    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    pa_source *source;

    pa_assert(m);
    pa_assert_se(source = m->userdata);

    return pa_source_linked_by(source);
}

void pa__done(pa_module *m) {
    pa_source *source;

    pa_assert(m);

    if ((source = m->userdata))
        pa_tinyalsa_source_free(source);
}
