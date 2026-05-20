#ifndef __tinycompresssink__
#define __tinycompresssink__

/**********************************************************************
* Copyright (c) 2015 LG Electronics, Inc.
* All rights reserved.
*
* tinycompress-sink.h
**********************************************************************/

#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/sink.h>

pa_sink *pa_tinycompress_sink_new(pa_module *m, pa_modargs *ma, const char *driver);
void pa_tinycompress_sink_free(pa_sink *s);

/*
 * Following block was removed from tinycompress commit:
 * commit 6ca3d352147ce24508f6f2dbfb2db72ec1b85c08
 * Author: Vinod Koul <vinod.koul@intel.com>
 * Date:   Mon Dec 16 20:37:24 2013 +0530
 *
 *   tinycompress: remove usage of SNDRV_RATE_xxx
 *
 * since the SNDRV_PCM_RATE_* is not availble anywhere in userspace
 * and we have used these to define the sampling rate, we need to define
 * then here
 */
#define SNDRV_PCM_RATE_5512     (1<<0)      /* 5512Hz */
#define SNDRV_PCM_RATE_8000     (1<<1)      /* 8000Hz */
#define SNDRV_PCM_RATE_11025        (1<<2)      /* 11025Hz */
#define SNDRV_PCM_RATE_12000        (1<<3)      /* 12000Hz */
#define SNDRV_PCM_RATE_16000        (1<<4)      /* 16000Hz */
#define SNDRV_PCM_RATE_22050        (1<<5)      /* 22050Hz */
#define SNDRV_PCM_RATE_24000        (1<<6)      /* 24000Hz */
#define SNDRV_PCM_RATE_32000        (1<<7)      /* 32000Hz */
#define SNDRV_PCM_RATE_44100        (1<<8)      /* 44100Hz */
#define SNDRV_PCM_RATE_48000        (1<<9)      /* 48000Hz */
#define SNDRV_PCM_RATE_64000        (1<<10)     /* 64000Hz */
#define SNDRV_PCM_RATE_88200        (1<<11)     /* 88200Hz */
#define SNDRV_PCM_RATE_96000        (1<<12)     /* 96000Hz */
#define SNDRV_PCM_RATE_176400       (1<<13)     /* 176400Hz */
#define SNDRV_PCM_RATE_192000       (1<<14)     /* 192000Hz */

#endif

