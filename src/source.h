/*
 * rpi5datv - programme source: video file or test card, encoded to
 * H.264 (+ MPEG-1 Layer II audio) and multiplexed into a constant-rate
 * DVB transport stream that is written into a ring buffer.
 *
 * Replaces avc2ts of the original project (which needed the Pi 3's
 * VideoCore OpenMAX H.264 encoder via vchiq; the Pi 5 has no hardware
 * H.264 encoder, so libx264 on the Cortex-A76 cores is used instead).
 */
#ifndef RPI5DATV_SOURCE_H
#define RPI5DATV_SOURCE_H

#include "ringbuf.h"

typedef struct {
    const char *input;        /* video file, ignored if testcard */
    int testcard;
    int loop;
    int width, height;        /* 0 = choose from bit rate */
    int audio;                /* 1 = include audio if bit rate allows */
    double ts_bitrate;        /* exact mux rate, bits/s */
    const char *service_name;
    const char *provider_name;
    const char *info_line;    /* shown on the test card */
    int verbose;
    volatile int *stop;
    ringbuf_t *ring;

    /* filled in by source_plan() */
    int video_bitrate;
    int audio_bitrate;        /* 0 = no audio */
    int audio_channels;
} source_cfg_t;

/* Decide resolution and bit rate split. */
void source_plan(source_cfg_t *cfg);

/* Runs until EOF (no loop), *stop, or error. Closes the ring on exit.
 * Returns 0 on success. Meant to be run in its own thread. */
int source_run(source_cfg_t *cfg);

#endif
