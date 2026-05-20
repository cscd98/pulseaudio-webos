/**********************************************************************
 * Copyright (c) 2013-2016 LG Electronics, Inc.
 * All rights reserved.
 *
 * paplayext - A basic PA client implemented to play MP3/aac/wav files.
 * This client is used for playing pass through stream through
 * tiny compress sink for low power audio or for playing pcm/wav
 * through alsa sink.
 * This client does not take any input argument to start with.
 * It takes keyboard inputs for input file and format information and
 * takes keyboard options to play /pause /stop the playback.
 **********************************************************************/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <curses.h>
#include <pulse/pulseaudio.h>
#include <pulsecore/log.h>

/* defines */
#ifndef bool
    #define bool int
    #define true 1
    #define false 0
#endif

#define FORMAT           PA_SAMPLE_S16NE
#define RATE                44100
#define CHANNELS       2
#define NO_OF_TRIES 3
/* Too many of this callback will keep coming during playback
     adviced to use option 'l' instead to get latency during execution*/
#define USE_LATENCY_CB 1

/* enums and data structures */
typedef enum formatSupportedEN {
    wav = 1,
    mp3,
    aac
} formatSupported;

typedef struct pulseAttributesSt {
    pa_threaded_mainloop *ml;
    pa_context *c;
    pa_stream *s;
    pa_buffer_attr attr;
} pulse;

typedef struct customDataST {
  FILE *fp;
  formatSupported format;
  pulse *pa;
  int64_t offset;
  GMainLoop *loop;
} customData;

/* function prototypes*/
static gboolean handle_keyboard(GIOChannel *source, GIOCondition cond, gpointer gdata);
static void context_state_cb(pa_context *c, void *userdata);
static void stream_write_cb(pa_stream *s, size_t nbytes, void *userdata);
static void stream_buffer_attr_changed_callback(pa_stream *s, void *userdata);
static void stream_state_callback(pa_stream *s, void *userdata );
static void stream_underflow_callback(pa_stream *s, void *userdata);
static void stream_overflow_callback(pa_stream *s, void *userdata);
#if USE_LATENCY_CB
static void stream_latency_update_callback(pa_stream *s, void *userdata);
#endif
static void stop_stream(customData *data );
static void get_time(customData *data);
static void get_buffer_attr(customData *data);
static void change_buffer_attr(customData *data);
static void get_latency(customData *data);


/* stop the playback*/
static void stop_stream(customData *data ) {

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
        pa_log("stopping : wait for stream-stopped notification before you end the app\n");
        pa_stream_set_write_callback(data->pa->s , NULL, NULL);
        pa_stream_flush(data->pa->s, NULL, NULL);
        pa_stream_disconnect(data->pa->s);
    } else
        pa_log("Stream is already stopped \n");
}

/* fetch and print time */
static void get_time(customData *data) {

    pa_usec_t time = 0;

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
        if (pa_stream_get_time (data->pa->s, &time) < 0)
            pa_log("ERROR : could not fetch stream timing info\n");
        else
            pa_log("Received stream time = %llu\n", time);
    } else
        pa_log("ERROR : No playback stream found to fetch the timing\n");
}

/* fetch and print buffer attributes*/
static void get_buffer_attr(customData *data) {

    pa_buffer_attr *attr;

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
        attr = pa_stream_get_buffer_attr(data->pa->s);
        pa_log("\nprinting buffer attributes :: ");
        pa_log("tlength= %u  maxlength= %u  prebuf= %u  minreq= %u  fragsize= %u\n", \
        attr->tlength, attr->maxlength, attr->prebuf, attr->minreq, attr->fragsize);
    } else
        pa_log("ERROR : No playback stream found to fetch the buffer attributes\n");
}

/* change buffer attributes*/
static void change_buffer_attr(customData *data) {

    pa_buffer_attr setAttr;

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
        fflush(stdin);
        pa_stream_cork(data->pa->s, 1, NULL, NULL);
        pa_log("Enter tlength (in unsigned integer)");
        scanf("%u", &(setAttr.tlength));
        pa_log("Enter maxlength (in unsigned integer)");
        scanf("%u", &(setAttr.maxlength));
        pa_log("Enter prebuf (in unsigned integer)");
        scanf("%u", &(setAttr.prebuf));
        pa_log("Enter minreq (in unsigned integer)");
        scanf("%u", &(setAttr.minreq));
        pa_stream_set_buffer_attr(data->pa->s, &setAttr, NULL, NULL);
        pa_stream_cork(data->pa->s, 0, NULL, NULL);
    } else
        pa_log("ERROR : No playback stream found to set the buffer attributes\n");
}

/* get the latency */
static void get_latency(customData *data) {

    pa_usec_t latency = 0;
    int negative = 0;

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
        pa_stream_get_latency(data->pa->s, &latency, &negative);
        pa_log("Received latency = %llu\n", latency);
        if (negative)
            pa_log("Captured data is not played yet\n");
    } else
        pa_log("ERROR : No playback stream found to get the latency\n");
}

/* Process keyboard input */
static gboolean handle_keyboard (GIOChannel *source, GIOCondition cond, gpointer gdata) {

    gchar *str = NULL;
    customData *data = (customData *)gdata;

    if (g_io_channel_read_line (source, &str, NULL, NULL, NULL) != G_IO_STATUS_NORMAL)
        return TRUE;

    pa_threaded_mainloop_lock(data->pa->ml);

    switch (str[0]) {
        case 'p':
           if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY) {
               pa_stream_cork(data->pa->s,
                                        !pa_stream_is_corked(data->pa->s),
                                         NULL,
                                         NULL);
           } else
                pa_log("ERROR : No playback stream found to cork /uncork it\n");
            break;

        case 's':
            stop_stream(data);
            break;

        case 't':
            get_time(data);
            break;

        case 'b':
            get_buffer_attr(data);
            break;

        case 'c':
            pa_log("NOTE : Playback will be paused till you are done\n");
            change_buffer_attr(data);
            break;

        case 'l':
            get_latency(data);
            break;

         case 'd':
            pa_log("Disabling latency call back\n");
            pa_stream_set_latency_update_callback(data->pa->s, NULL, (void *)data);
            break;

        case 'q':
             stop_stream(data);
             if (data->loop && g_main_loop_is_running(data->loop))
                g_main_loop_quit (data->loop);
                break;

        default:
            break;
    }
    pa_threaded_mainloop_unlock(data->pa->ml);

    g_free (str);
    return TRUE;
}

static void stream_buffer_attr_changed_callback(pa_stream *s, void *userdata) {

    pa_assert(s);
    customData *data = (customData *) userdata;

    pa_buffer_attr *attr = pa_stream_get_buffer_attr(data->pa->s);
    pa_log("\nChanged buffer attributes \t");
    pa_log("tlength= %u  maxlength= %u  prebuf= %u  minreq= %u  fragsize= %u\n", \
    attr->tlength, attr->maxlength, attr->prebuf, attr->minreq, attr->fragsize);
}

static void stream_underflow_callback(pa_stream *s, void *userdata) {

    pa_assert(s);
    pa_log("Stream underrun........\n");
}

static void stream_overflow_callback(pa_stream *s, void *userdata) {

    pa_assert(s);
    pa_log("Stream overrun..........\n");
}

#if USE_LATENCY_CB
static void stream_latency_update_callback(pa_stream *s, void *userdata) {

    pa_assert(s);
    customData *data = (customData *) userdata;
    pa_log("LATENCY : If you feel you are getting this CB too much then press 'd' and use 'l' to get latency \n");
    get_latency(data);
}
#endif

/* write data*/
static void stream_write_cb(pa_stream *s, size_t nbytes, void *userdata) {

    pa_assert(s);
    int err = -1;

    customData *data = (customData *) userdata;
    char *buffer = NULL;
    ssize_t n, writable = nbytes, towrite = nbytes;

    if (pa_stream_get_state (data->pa->s) == PA_STREAM_READY ) {

        while (nbytes > 0) {

             pa_stream_writable_size(data->pa->s);

             err = pa_stream_begin_write(data->pa->s, (void **)&buffer, &writable);
             if ((err !=0) || (data->pa->s == NULL))
             {
                 pa_log("pa_stream_begin_write() failed. Error : %s(%d)", pa_strerror(err), err);
                 return;
             }

             towrite = (writable < nbytes) ? writable : nbytes;
             n = fread(buffer, 1, towrite, data->fp);
             pa_stream_write(data->pa->s, (void *)buffer, n, NULL, data->offset, PA_SEEK_ABSOLUTE);
             data->offset += n;

             if (feof(data->fp)) {
                 pa_log("END OF FILE reached.: wait till the playback gets over\n");
                 stop_stream(data);
                 return;
            }
            nbytes -= n;
        }
    }
}

/* handle stream state */
static void stream_state_callback(pa_stream *s, void *userdata ) {

    pa_assert(s);

    switch (pa_stream_get_state(s)) {
        case PA_STREAM_UNCONNECTED:
        case PA_STREAM_CREATING:
            break;
        case PA_STREAM_TERMINATED:
            pa_log("Stream is now Stopped......\n");
            break;
        case PA_STREAM_READY:
            break;
        default:
        case PA_STREAM_FAILED:
            break;
    }
}

/* handle context state */
static void context_state_cb(pa_context *c, void *userdata) {

    customData *data = (customData *) userdata;

    switch (pa_context_get_state(c)) {

        case PA_CONTEXT_READY: {

             pa_stream_flags_t streamflag;
             pa_format_info *formats[1];

             formats[0] = pa_format_info_new();
             if (data->format == mp3)
                  formats[0]->encoding = PA_ENCODING_MPEG_IEC61937;
             else if (data->format == aac)
                  formats[0]->encoding = PA_ENCODING_MPEG2_AAC_IEC61937;
             else if (data->format == wav)
                  formats[0]->encoding = PA_ENCODING_PCM;

             pa_format_info_set_sample_format(formats[0], FORMAT);
             pa_format_info_set_rate(formats[0], RATE);
             pa_format_info_set_channels(formats[0], CHANNELS);

             data->pa->s = pa_stream_new_extended(data->pa->c,
                                                  "playback-stream",
                                                   formats,
                                                   1,
                                                   NULL);

              pa_assert(data->pa->s);
              pa_stream_set_state_callback(data->pa->s, stream_state_callback, (void *)data);
              pa_stream_set_underflow_callback(data->pa->s, stream_underflow_callback, (void *)data);
              pa_stream_set_overflow_callback(data->pa->s, stream_overflow_callback, (void *)data);
              pa_stream_set_write_callback(data->pa->s , stream_write_cb, data);
              pa_stream_set_buffer_attr_callback(data->pa->s, stream_buffer_attr_changed_callback, data);
#if USE_LATENCY_CB
              pa_stream_set_latency_update_callback(data->pa->s, stream_latency_update_callback, (void *)data);
#endif

              streamflag = PA_STREAM_ADJUST_LATENCY |
                                  PA_STREAM_INTERPOLATE_TIMING |
                                  PA_STREAM_AUTO_TIMING_UPDATE;
              if (data->format == mp3 ||
                         data->format == aac)
                  streamflag = streamflag | PA_STREAM_PASSTHROUGH;
              pa_stream_connect_playback(data->pa->s, NULL, &(data->pa->attr), streamflag , NULL, NULL);
              pa_threaded_mainloop_signal(data->pa->ml, 0);
              break;
         }

        case PA_CONTEXT_FAILED:
             pa_log("ERROR : Context failed to connect\n");
             break;
         case PA_CONTEXT_TERMINATED:
             pa_log("Context terminated\n");
             break;
         default:
             break;
    }
}

int main(int argc, char *argv[]) {

    GIOChannel *io_stdin;
    int count = 0;
    char filename[100];
    char ch;

    /* Print usage map */
    print_usage_map:
    pa_log ( "\nUSAGE: \n"
      "1. This executable takes no input arguement.\n"
      "2. Start the program.\n"
      "3. Enter the file format [1 for wav/pcm | 2 for mp3 | 3 for aac] and press enter\n"
      "4. Enter file name with path and press enter.\n"
      "5. Choose one of the following options during playback and press enter.\n"
      "   'p' - toggle between pause / resume \n"
      "   'b' - print buffer attributes during playback\n"
      "   'c' - change buffer attributes during playback\n"
      "   't' - print timing info of the stream during playback\n"
      "   'l' - print latency during playback\n"
      "   'd' - disable latency callback during playback\n"
      "   's' - stop playback \n"
      "   'q' - quit \n\n");

    if (argc >1 && strstr(argv[1], "help")) {
        return 0;
    }

    customData *data = (customData *)calloc(sizeof(customData), sizeof(char));
    data->pa = (pulse *)calloc(sizeof(pulse), sizeof(char));

    /* take the file format input*/
    pa_log("\nEnter the format of the file to be played : [1 for wav/pcm | 2 for mp3 | 3 for aac]\n");
    scanf("%d", &data->format);

    try_again:
    /* take input file name */
    memset(filename, 0, sizeof(filename));
    pa_log("Enter file to be played : ex : /tmp/media/xyz.mp3\n");
    scanf("%s", filename);

    data->fp = fopen(filename, "r");
    if (!data->fp) {
        perror("fopen");
        count++;
        if (count < NO_OF_TRIES) {
            pa_log("\nTypo error ? Let's try again.... you get %d tries at the max\n",NO_OF_TRIES );
            goto try_again;
        } else {
            pa_log("Maximum try exceeded : ............quiting application\n");
            return -1;
        }
    }
    fflush(stdin);
    memset (&data->pa->attr, 0, sizeof (data->pa->attr));
    pa_log("Do you want to set the buffer attributes (y|n) ?\n");
    scanf(" %c", &ch);
    if (ch == 'y') {
         pa_log("Enter tlength (in unsigned integer)");
         scanf("%u", &(data->pa->attr.tlength));
         pa_log("Enter maxlength (in unsigned integer)");
         scanf("%u", &(data->pa->attr.maxlength));
         pa_log("Enter prebuf (in unsigned integer)");
         scanf("%u", &(data->pa->attr.prebuf));
         pa_log("Enter minreq (in unsigned integer)");
         scanf("%u", &(data->pa->attr.minreq));
    } else {
        data->pa->attr.tlength                = (uint32_t)-1;
        data->pa->attr.maxlength           = (uint32_t)-1;
        data->pa->attr.prebuf                 = (uint32_t)-1;
        data->pa->attr.minreq                = (uint32_t)-1;
    }

    io_stdin = g_io_channel_unix_new (fileno (stdin));
    g_io_add_watch (io_stdin, G_IO_IN, (GIOFunc)handle_keyboard, (gpointer)data);

    data->pa->ml = pa_threaded_mainloop_new();
    pa_assert(data->pa->ml);
    if (!(data->pa->c = pa_context_new(pa_threaded_mainloop_get_api(data->pa->ml), NULL)))
        pa_log("ERROR : context creation failed\n");

    pa_context_set_state_callback(data->pa->c, context_state_cb, (void *) data);

    if (pa_context_connect(data->pa->c, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        pa_log("ERROR : context connect failed\n");

    pa_threaded_mainloop_lock(data->pa->ml);
    pa_threaded_mainloop_start(data->pa->ml);
    pa_threaded_mainloop_wait(data->pa->ml);
    pa_threaded_mainloop_unlock(data->pa->ml);

    data->loop = g_main_loop_new (NULL, FALSE);
    if (data->loop)
       g_main_loop_run (data->loop);

    if (data->loop)
      g_main_loop_unref (data->loop);

    if (io_stdin)
       g_io_channel_unref (io_stdin);

    if (data->pa->s) {
        pa_stream_unref(data->pa->s);
        data->pa->s = NULL;
    }

    if (data->pa->c) {
        pa_context_disconnect(data->pa->c);
        pa_context_unref(data->pa->c);
    }

    if (data->pa->ml) {
        pa_threaded_mainloop_stop(data->pa->ml);
        pa_threaded_mainloop_free(data->pa->ml);
    }

    if (data->fp) {
        fclose(data->fp);
        data->fp = NULL;
    }

    if (data->pa) {
        free(data->pa);
        data->pa = NULL;
    }

    if (data) {
        free(data);
       data = NULL;
    }
    pa_log("quiting app \n");
    return 0;
}/*end of main*/
