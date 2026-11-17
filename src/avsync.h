/*
 * rpi5datv - audio/video synchronisation for file and live input
 *
 * Pure decision logic (no FFmpeg), so it can be unit tested on its own
 * (tests/test_avsync.c). source.c feeds it the timestamps of decoded
 * frames and applies the actions it returns.
 *
 * Model: video is the master clock. Output time = source time + off,
 * with off set from the first video frame and re-set ("re-based") at a
 * loop or a timestamp discontinuity, so output always continues from
 * where it is. Audio is checked against the same mapping on every frame
 * and corrected (silence inserted / samples dropped) when it is more
 * than AVSYNC_A_TOL_S out.
 */
#ifndef RPI5DATV_AVSYNC_H
#define RPI5DATV_AVSYNC_H

#include <stdint.h>

/* Video timestamp jumps that mean "discontinuity" rather than a normal
 * gap: backwards beyond jitter, or forwards by more than this unless the
 * audio confirms the time really passed (slideshows, still sections). */
#define AVSYNC_V_JUMP_BACK  0.5
#define AVSYNC_V_JUMP_FWD   3.0
/* Audio is corrected when more than A_TOL off the video mapping. Audio
 * more than A_MAX off is in a discontinuity the video hasn't reached
 * yet (passed through as is), except for the first audio of a segment
 * starting late, which is filled with silence; if it stays that far off
 * for A_MAX of audio, the jump is audio-only and is adopted. */
#define AVSYNC_A_TOL_S      0.040
#define AVSYNC_A_MAX_S      2.0

typedef struct {
    double fps;               /* output frame rate */
    double rate;              /* output audio sample rate */

    int have_off;
    double off;               /* output time = source time + off */
    double last_v;            /* source time of the previous video frame */
    double last_a;            /* source time of the latest audio frame */
    int have_a;
    int seg_audio;            /* audio frames seen in this segment */
    int have_pre;             /* audio arrived before the first picture */
    double pre_s;             /*   its source time */
    int64_t pre_in;           /*   and output sample position */
    double aoff;              /* extra audio offset (audio-only jump) */
    int a_disc;               /* audio is in a discontinuity, passed raw */
    int64_t a_wild;           /* audio samples passed raw so far */

    long n_rebase;            /* statistics: timestamp jumps handled */
    long n_afix;              /* statistics: audio corrections */
} avsync_t;

typedef struct {
    double target;            /* output time (s) this picture belongs at */
    int jumped;               /* 1 = discontinuity, mapping re-based */
    double jump_s;            /* size of the jump (source seconds) */
} avsync_video_t;

typedef struct {
    int64_t insert;           /* silence samples to push before the frame */
    int skip;                 /* samples to drop from the frame's start */
    int jumped;               /* 1 = audio-only jump adopted */
    double error_s;           /* how far out the audio was (+ = behind) */
} avsync_audio_t;

void avsync_init(avsync_t *a, double fps, double rate);

/* Start of a new segment (file loop): the next video frame re-bases the
 * mapping without counting as a jump. */
void avsync_new_segment(avsync_t *a);

/* A decoded picture with source time s (NAN if it has none); vpts is the
 * number of output frames emitted so far. The caller then emits the held
 * picture until vpts / fps >= target, and holds this one. */
avsync_video_t avsync_video(avsync_t *a, double s, int64_t vpts);

/* A decoded audio frame of n samples (after resampling) with source time
 * s (NAN if none); a_in = samples pushed so far, vpts as above. The
 * caller pushes `insert` samples of silence, then the frame minus its
 * first `skip` samples. */
avsync_audio_t avsync_audio(avsync_t *a, double s, int n, int64_t a_in, int64_t vpts);

#endif
