/*
 * rpi5datv - audio/video synchronisation, see avsync.h
 */
#include "avsync.h"

#include <math.h>
#include <string.h>

void avsync_init(avsync_t *a, double fps, double rate)
{
    memset(a, 0, sizeof(*a));
    a->fps = fps;
    a->rate = rate;
}

void avsync_new_segment(avsync_t *a)
{
    a->have_off = 0;
    a->have_pre = 0;
    a->seg_audio = 0;
}

static void clear_audio_disc(avsync_t *a)
{
    a->aoff = 0;
    a->a_disc = 0;
    a->a_wild = 0;
}

avsync_video_t avsync_video(avsync_t *a, double s, int64_t vpts)
{
    avsync_video_t r = { 0, 0, 0 };
    if (isnan(s))
        s = a->have_off ? a->last_v + 1.0 / a->fps : 0;

    const double out_now = (double)vpts / a->fps;
    if (!a->have_off) {
        /* First picture of a segment. If audio already went out ahead of
         * it, keep that audio where it is and map the picture by its
         * timestamp (it is shown from now until its successor). */
        if (a->have_pre)
            a->off = (double)a->pre_in / a->rate - a->pre_s;
        else
            a->off = out_now - s;
        a->have_off = 1;
        clear_audio_disc(a);
    } else if (s < a->last_v - AVSYNC_V_JUMP_BACK ||
               (s > a->last_v + AVSYNC_V_JUMP_FWD &&
                /* A long gap in the video is genuine (slideshow, still
                 * section) if continuous audio shows that time really
                 * passed. Audio may be well past this picture: a threaded
                 * video decoder releases frames a few frames late. */
                !(a->have_a && !a->a_disc && a->last_a > s - AVSYNC_V_JUMP_FWD))) {
        /* Discontinuity (sender restarted, stream switched...): continue
         * from where the output is now, one frame after the held picture,
         * instead of freezing or bursting held frames. */
        r.jumped = 1;
        r.jump_s = s - a->last_v;
        a->off = out_now + 1.0 / a->fps - s;
        clear_audio_disc(a);
        a->n_rebase++;
    }
    a->last_v = s;
    r.target = s + a->off;
    return r;
}

avsync_audio_t avsync_audio(avsync_t *a, double s, int n, int64_t a_in, int64_t vpts)
{
    (void)vpts;
    avsync_audio_t r = { 0, 0, 0, 0 };
    if (isnan(s))
        return r;
    a->last_a = s;
    a->have_a = 1;
    const int first = a->seg_audio++ == 0;
    if (!a->have_off) {
        /* No picture yet: let it through and remember where it went. */
        if (!a->have_pre) {
            a->have_pre = 1;
            a->pre_s = s;
            a->pre_in = a_in;
        }
        return r;
    }

    /* Where this audio belongs on the output timeline vs where the audio
     * actually is (samples pushed so far). */
    const double want = (s + a->off + a->aoff) * a->rate;
    const double diff = want - (double)a_in;
    r.error_s = diff / a->rate;

    if (fabs(diff) > AVSYNC_A_MAX_S * a->rate && !(first && diff > 0)) {
        /* Discontinuity the video hasn't reached yet (or old audio still
         * draining after the video re-based): pass through. If it lasts,
         * the jump is audio-only: adopt it, continuing from here. */
        a->a_disc = 1;
        a->a_wild += n;
        if (a->a_wild > (int64_t)(AVSYNC_A_MAX_S * a->rate)) {
            a->aoff -= diff / a->rate;
            a->a_disc = 0;
            a->a_wild = 0;
            a->n_rebase++;
            r.jumped = 1;
        }
        return r;
    }
    a->a_disc = 0;
    a->a_wild = 0;
    if (diff > AVSYNC_A_TOL_S * a->rate) {
        r.insert = (int64_t)llround(diff);          /* audio missing */
        a->n_afix++;
    } else if (diff < -AVSYNC_A_TOL_S * a->rate) {
        long d = (long)llround(-diff);              /* audio ahead */
        r.skip = d < n ? (int)d : n;
        a->n_afix++;
    }
    return r;
}
