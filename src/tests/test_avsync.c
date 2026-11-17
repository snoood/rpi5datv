/*
 * rpi5datv - unit tests for the A/V synchronisation logic (avsync.c)
 *
 * Each scenario feeds a stream of decoded video and audio frames (source
 * timestamps, in demux/decode order) through avsync and applies its
 * actions exactly as source.c does: pictures are held until their target
 * output time, audio gets silence inserted / samples dropped. The
 * simulated output then tells us:
 *   - A/V error: for every audio frame, output time it actually starts at
 *     minus the output time the video mapping says it belongs at,
 *   - video continuity: output pictures per second of source video,
 *   - how many jumps / corrections were reported.
 *
 * Build and run: make -C src test
 */
#include "../avsync.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FPS   25.0
#define RATE  48000.0
#define APF   1920          /* audio samples per output frame */
#define AFR   1152          /* MP2 frame: a typical source audio frame */

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------ harness */

typedef struct { char type; double s; int n; } ev_t;     /* 'V' / 'A' / 'L' (loop) */

typedef struct {
    ev_t *ev;
    int nev, cap;
} stream_t;

static void add(stream_t *st, char type, double s, int n)
{
    if (st->nev == st->cap) {
        st->cap = st->cap ? st->cap * 2 : 1024;
        st->ev = realloc(st->ev, sizeof(ev_t) * (size_t)st->cap);
    }
    st->ev[st->nev++] = (ev_t){ type, s, n };
}

/* Interleave video (25 fps) and audio (AFR-sample frames) for source time
 * [t0, t1), each stream with its own timestamp offset, in timestamp order.
 * vdelay delays video delivery (decoder latency) by that many seconds. */
static void gen(stream_t *st, double t0, double t1, double voff, double aoff,
                int with_video, int with_audio, double vdelay)
{
    double tv = t0, ta = t0;
    const double adur = AFR / RATE;
    for (;;) {
        int v_ok = with_video && tv < t1 - 1e-9;
        int a_ok = with_audio && ta < t1 - 1e-9;
        if (!v_ok && !a_ok)
            break;
        if (v_ok && (!a_ok || tv + vdelay <= ta)) {
            add(st, 'V', tv + voff, 0);
            tv += 1.0 / FPS;
        } else {
            add(st, 'A', ta + aoff, AFR);
            ta += adur;
        }
    }
}

typedef struct {
    int64_t vpts, a_in;
    double max_err, max_err_settled;  /* |A/V error|, all / after settle */
    double settle_after;               /* ignore errors before this out time */
    long jumps, fixes, a_jumps;
    long max_burst;                    /* most held pictures emitted at once */
} result_t;

static result_t run(stream_t *st, int has_audio, double settle_after)
{
    avsync_t sy;
    avsync_init(&sy, FPS, RATE);
    result_t r;
    memset(&r, 0, sizeof(r));
    r.settle_after = settle_after;
    int have_last = 0;

    for (int i = 0; i < st->nev; i++) {
        ev_t *e = &st->ev[i];
        if (e->type == 'L') {
            /* end_segment(): hold the last picture once, square up audio */
            if (have_last) {
                r.vpts++;
                if (!has_audio)
                    r.a_in += APF;
            }
            if (has_audio && r.a_in < r.vpts * APF)
                r.a_in = r.vpts * APF;
            while (have_last && r.a_in > r.vpts * APF + APF)
                r.vpts++;
            avsync_new_segment(&sy);
        } else if (e->type == 'V') {
            avsync_video_t v = avsync_video(&sy, e->s, r.vpts);
            r.jumps += v.jumped;
            long burst = 0;
            while (have_last && (double)r.vpts / FPS < v.target - 1e-6) {
                r.vpts++;
                burst++;
                if (!has_audio)
                    r.a_in += APF;
            }
            if (burst > r.max_burst)
                r.max_burst = burst;
            have_last = 1;
        } else {
            avsync_audio_t a = avsync_audio(&sy, e->s, e->n, r.a_in, r.vpts);
            r.a_jumps += a.jumped;
            if (a.insert || a.skip)
                r.fixes++;
            r.a_in += a.insert;
            if (a.skip >= e->n)
                continue;
            /* A/V error of the first sample actually pushed */
            if (sy.have_off) {
                double pushed_s = e->s + (double)a.skip / RATE;
                double should = pushed_s + sy.off + sy.aoff;
                double is = (double)r.a_in / RATE;
                double err = fabs(is - should);
                if (err > r.max_err)
                    r.max_err = err;
                if (is >= r.settle_after && err > r.max_err_settled)
                    r.max_err_settled = err;
            }
            r.a_in += e->n - a.skip;
        }
    }
    free(st->ev);
    st->ev = NULL;
    st->nev = st->cap = 0;
    return r;
}

static void report(const char *name, const result_t *r)
{
    printf("  %-34s out %6.2f s video / %6.2f s audio, max A/V err %5.1f ms "
           "(settled %5.1f), jumps %ld+%ld, fixes %ld, max burst %ld\n",
           name, r->vpts / FPS, r->a_in / RATE, r->max_err * 1000,
           r->max_err_settled * 1000, r->jumps, r->a_jumps, r->fixes, r->max_burst);
}

/* Final output positions of video and audio should agree to within an
 * output frame plus a source audio frame. */
static void check_aligned(const result_t *r)
{
    double d = fabs(r->vpts / FPS - r->a_in / RATE);
    /* allow: the last picture still held (one frame), audio up to the
     * correction tolerance off, plus one source audio frame */
    CHECK(d < 1.0 / FPS + AVSYNC_A_TOL_S + AFR / RATE + 0.001,
          "video/audio output lengths differ by %.0f ms", d * 1000);
}

#define TOL (AVSYNC_A_TOL_S + AFR / RATE)   /* worst error before a fix */

/* ---------------------------------------------------------- scenarios */

static void t_clean(void)
{
    stream_t st = { 0 };
    gen(&st, 1.4, 61.4, 0, 0, 1, 1, 0);
    result_t r = run(&st, 1, 0);
    report("clean 60 s", &r);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "unexpected jump");
    CHECK(r.fixes == 0, "unexpected audio corrections: %ld", r.fixes);
    CHECK(r.max_err < 0.002, "A/V error %.1f ms", r.max_err * 1000);
    CHECK(fabs(r.vpts / FPS - 60.0) < 0.05, "video length %.2f s", r.vpts / FPS);
    check_aligned(&r);
}

static void t_audio_hole(void)
{
    /* 0.6 s of audio lost at 10 s (e.g. dropped UDP packets) */
    stream_t st = { 0 }, tmp = { 0 };
    gen(&tmp, 0, 30, 0, 0, 1, 1, 0);
    for (int i = 0; i < tmp.nev; i++)
        if (!(tmp.ev[i].type == 'A' && tmp.ev[i].s >= 10.0 && tmp.ev[i].s < 10.6))
            add(&st, tmp.ev[i].type, tmp.ev[i].s, tmp.ev[i].n);
    free(tmp.ev);
    result_t r = run(&st, 1, 0);
    report("0.6 s audio hole", &r);
    CHECK(r.fixes == 1, "expected 1 correction, got %ld", r.fixes);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "unexpected jump");
    CHECK(r.max_err < TOL, "A/V error %.1f ms", r.max_err * 1000);
    check_aligned(&r);
}

static void t_short_dropouts(void)
{
    /* many tiny losses below the tolerance: no corrections, error bounded */
    stream_t st = { 0 }, tmp = { 0 };
    gen(&tmp, 0, 30, 0, 0, 1, 1, 0);
    int k = 0;
    for (int i = 0; i < tmp.nev; i++) {
        if (tmp.ev[i].type == 'A' && (k++ % 50) == 25)
            continue;                    /* lose one 24 ms frame per ~1.2 s */
        add(&st, tmp.ev[i].type, tmp.ev[i].s, tmp.ev[i].n);
    }
    free(tmp.ev);
    result_t r = run(&st, 1, 0);
    report("1 audio frame lost every 1.2 s", &r);
    CHECK(r.max_err < TOL, "A/V error %.1f ms", r.max_err * 1000);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "unexpected jump");
    check_aligned(&r);
}

static void t_audio_overlap(void)
{
    /* audio frames repeated (overlapping timestamps): audio runs ahead */
    stream_t st = { 0 }, tmp = { 0 };
    gen(&tmp, 0, 20, 0, 0, 1, 1, 0);
    for (int i = 0; i < tmp.nev; i++) {
        add(&st, tmp.ev[i].type, tmp.ev[i].s, tmp.ev[i].n);
        if (tmp.ev[i].type == 'A' && tmp.ev[i].s >= 8.0 && tmp.ev[i].s < 8.5)
            add(&st, 'A', tmp.ev[i].s, tmp.ev[i].n);      /* duplicate */
    }
    free(tmp.ev);
    result_t r = run(&st, 1, 0);
    report("0.5 s of audio duplicated", &r);
    CHECK(r.fixes >= 1, "expected corrections");
    CHECK(r.max_err < TOL, "A/V error %.1f ms", r.max_err * 1000);
    check_aligned(&r);
}

static void t_jump_back(int audio_first)
{
    /* sender restart: timestamps go from 10 s back to 0 s. With
     * audio_first, two frames of the new audio arrive before any of the
     * new video (the old video is still in the decoder). */
    stream_t st = { 0 }, rest = { 0 };
    gen(&st, 0, 10, 0, 0, 1, 1, 0);
    gen(&rest, 0, 10, 0, 0, 1, 1, 0);
    int moved = 0;
    if (audio_first) {
        add(&st, 'A', 0.0, AFR);
        add(&st, 'A', AFR / RATE, AFR);
    }
    for (int i = 0; i < rest.nev; i++) {
        if (audio_first && rest.ev[i].type == 'A' && moved < 2) {
            moved++;
            continue;
        }
        add(&st, rest.ev[i].type, rest.ev[i].s, rest.ev[i].n);
    }
    free(rest.ev);
    result_t r = run(&st, 1, 10.5);
    report(audio_first ? "jump back 10 s (audio first)" : "jump back 10 s", &r);
    CHECK(r.jumps == 1, "expected 1 video jump, got %ld", r.jumps);
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.2, "video length %.2f s (froze?)", r.vpts / FPS);
    CHECK(r.max_burst <= 2, "held-picture burst of %ld frames", r.max_burst);
    CHECK(r.max_err_settled < TOL, "A/V error after jump %.1f ms", r.max_err_settled * 1000);
    check_aligned(&r);
}

static void t_jump_forward_video_first(void)
{
    /* timestamps skip ahead 30 s, video sees it first */
    stream_t st = { 0 };
    gen(&st, 0, 10, 0, 0, 1, 1, 0);
    add(&st, 'V', 40.0, 0);
    stream_t rest = { 0 };
    gen(&rest, 40.0, 50.0, 0, 0, 1, 1, 0);
    int vskip = 0;
    for (int i = 0; i < rest.nev; i++) {
        if (rest.ev[i].type == 'V' && !vskip) { vskip = 1; continue; }
        add(&st, rest.ev[i].type, rest.ev[i].s, rest.ev[i].n);
    }
    free(rest.ev);
    result_t r = run(&st, 1, 10.5);
    report("jump forward 30 s (video first)", &r);
    CHECK(r.jumps == 1, "expected 1 video jump, got %ld", r.jumps);
    CHECK(r.max_burst <= 2, "held-picture burst of %ld frames", r.max_burst);
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.2, "video length %.2f s", r.vpts / FPS);
    CHECK(r.max_err_settled < TOL, "A/V error after jump %.1f ms", r.max_err_settled * 1000);
    check_aligned(&r);
}

static void t_jump_forward_audio_first(void)
{
    /* timestamps skip ahead 30 s, audio sees it first: the audio is in a
     * discontinuity so it doesn't confirm the video gap; re-based */
    stream_t st = { 0 };
    gen(&st, 0, 10, 0, 0, 1, 1, 0);
    add(&st, 'A', 40.0, AFR);
    stream_t rest = { 0 };
    gen(&rest, 40.0, 50.0, 0, 0, 1, 1, 0);
    int askip = 0;
    for (int i = 0; i < rest.nev; i++) {
        if (rest.ev[i].type == 'A' && !askip) { askip = 1; continue; }
        add(&st, rest.ev[i].type, rest.ev[i].s, rest.ev[i].n);
    }
    free(rest.ev);
    result_t r = run(&st, 1, 10.5);
    report("jump forward 30 s (audio first)", &r);
    CHECK(r.jumps == 1, "expected 1 video jump, got %ld", r.jumps);
    CHECK(r.max_burst <= 2, "held-picture burst of %ld frames", r.max_burst);
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.2, "video length %.2f s", r.vpts / FPS);
    CHECK(r.max_err_settled < TOL, "A/V error after jump %.1f ms", r.max_err_settled * 1000);
    check_aligned(&r);
}

static void t_slideshow(void)
{
    /* one picture every 5 s, continuous audio, decoder releasing pictures
     * up to 3 frames' worth of picture time late */
    stream_t st = { 0 };
    double ta = 0;
    for (int k = 0; k < 6; k++) {
        double tv = 5.0 * k;
        while (ta < tv + 0.12) {          /* audio runs ahead of the picture */
            add(&st, 'A', ta, AFR);
            ta += AFR / RATE;
        }
        add(&st, 'V', tv, 0);
    }
    while (ta < 30.0) {
        add(&st, 'A', ta, AFR);
        ta += AFR / RATE;
    }
    result_t r = run(&st, 1, 0);
    report("slideshow (1 picture / 5 s)", &r);
    CHECK(r.jumps == 0, "slideshow gaps taken as jumps: %ld", r.jumps);
    CHECK(fabs(r.vpts / FPS - 25.0) < 0.1, "pictures not held: video %.2f s", r.vpts / FPS);
    CHECK(r.max_err_settled < TOL, "A/V error %.1f ms", r.max_err_settled * 1000);
}

static void t_no_audio_gap(void)
{
    /* video only, 5 s gap: no audio to confirm, so re-based (live stall) */
    stream_t st = { 0 };
    gen(&st, 0, 10, 0, 0, 1, 0, 0);
    gen(&st, 15, 25, 0, 0, 1, 0, 0);
    result_t r = run(&st, 0, 0);
    report("video only, 5 s gap", &r);
    CHECK(r.jumps == 1, "expected 1 jump, got %ld", r.jumps);
    CHECK(r.max_burst <= 2, "held-picture burst of %ld frames", r.max_burst);
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.2, "video length %.2f s", r.vpts / FPS);
}

static void t_no_audio_short_gap(void)
{
    /* video only, 2 s gap (below threshold): held, not re-based */
    stream_t st = { 0 };
    gen(&st, 0, 10, 0, 0, 1, 0, 0);
    gen(&st, 12, 20, 0, 0, 1, 0, 0);
    result_t r = run(&st, 0, 0);
    report("video only, 2 s gap", &r);
    CHECK(r.jumps == 0, "unexpected jump");
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.1, "video length %.2f s", r.vpts / FPS);
}

static void t_audio_only_jump(void)
{
    /* audio timestamps jump +10 s at 10 s, video carries on: adopted
     * after A_MAX of audio, then back in sync */
    stream_t st = { 0 }, tmp = { 0 };
    gen(&tmp, 0, 30, 0, 0, 1, 1, 0);
    for (int i = 0; i < tmp.nev; i++) {
        double s = tmp.ev[i].s;
        if (tmp.ev[i].type == 'A' && s >= 10.0)
            s += 10.0;
        add(&st, tmp.ev[i].type, s, tmp.ev[i].n);
    }
    free(tmp.ev);
    result_t r = run(&st, 1, 13.0);
    report("audio-only jump +10 s", &r);
    CHECK(r.a_jumps == 1, "expected 1 audio jump, got %ld", r.a_jumps);
    CHECK(r.jumps == 0, "video should not jump");
    CHECK(r.max_err_settled < TOL, "A/V error after adopt %.1f ms", r.max_err_settled * 1000);
    check_aligned(&r);
}

static void t_late_audio(void)
{
    /* file whose audio starts 3 s after the video: silence fills it */
    stream_t st = { 0 };
    gen(&st, 0, 3, 0, 0, 1, 0, 0);
    gen(&st, 3, 20, 0, 0, 1, 1, 0);
    result_t r = run(&st, 1, 0);
    report("audio starts 3 s late", &r);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "late audio taken as a jump");
    CHECK(r.fixes == 1, "expected 1 silence fill, got %ld", r.fixes);
    CHECK(r.max_err < TOL, "A/V error %.1f ms", r.max_err * 1000);
    check_aligned(&r);
}

static void t_early_audio(void)
{
    /* audio starts 1 s before the first picture: trimmed to the video */
    stream_t st = { 0 };
    gen(&st, 0, 1, 0, 0, 0, 1, 0);
    gen(&st, 1, 20, 0, 0, 1, 1, 0);
    result_t r = run(&st, 1, 0);
    report("audio starts 1 s early", &r);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "unexpected jump");
    CHECK(r.fixes == 0, "early audio trimmed: %ld corrections", r.fixes);
    CHECK(fabs(r.vpts / FPS - 20.0) < 0.1, "first picture not held: video %.2f s", r.vpts / FPS);
    CHECK(r.max_err_settled < TOL, "A/V error %.1f ms", r.max_err_settled * 1000);
    check_aligned(&r);
}

static void t_loop(void)
{
    /* -l: three passes of a 10 s file, timestamps restart each pass */
    stream_t st = { 0 };
    for (int p = 0; p < 3; p++) {
        gen(&st, 1.4, 11.4, 0, 0, 1, 1, 0);
        add(&st, 'L', 0, 0);
    }
    result_t r = run(&st, 1, 0);
    report("loop x3", &r);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "loops counted as jumps");
    CHECK(fabs(r.vpts / FPS - 30.0) < 0.2, "video length %.2f s", r.vpts / FPS);
    CHECK(r.max_err < TOL, "A/V error %.1f ms", r.max_err * 1000);
    check_aligned(&r);
}

static void t_no_timestamps(void)
{
    /* video without timestamps: each picture belongs one frame after the
     * previous one */
    avsync_t sy;
    avsync_init(&sy, FPS, RATE);
    int64_t vpts = 0;
    int ok = 1, have_last = 0;
    for (int i = 0; i < 100; i++) {
        avsync_video_t v = avsync_video(&sy, NAN, vpts);
        if (fabs(v.target - i / FPS) > 1e-9 || v.jumped)
            ok = 0;
        while (have_last && vpts / FPS < v.target - 1e-6)
            vpts++;
        have_last = 1;
    }
    avsync_audio_t a = avsync_audio(&sy, NAN, AFR, 0, vpts);
    printf("  %-34s %s (%lld frames)\n", "missing timestamps", ok ? "ok" : "bad",
           (long long)vpts);
    CHECK(ok, "pictures without timestamps not spaced one frame apart");
    CHECK(vpts == 99, "expected 99 frames emitted, got %lld", (long long)vpts);
    CHECK(!a.insert && !a.skip && !a.jumped, "audio without timestamp was corrected");
}

static void t_jitter(void)
{
    /* +/-20 ms random jitter on audio timestamps: no corrections */
    stream_t st = { 0 };
    gen(&st, 0, 60, 0, 0, 1, 1, 0);
    srand(1);
    for (int i = 0; i < st.nev; i++)
        if (st.ev[i].type == 'A')
            st.ev[i].s += ((rand() / (double)RAND_MAX) - 0.5) * 0.040;
    result_t r = run(&st, 1, 0);
    report("audio timestamp jitter +/-20 ms", &r);
    CHECK(r.fixes == 0, "jitter caused %ld corrections", r.fixes);
    CHECK(r.jumps == 0 && r.a_jumps == 0, "unexpected jump");
}

int main(void)
{
    printf("A/V sync (avsync.c):\n");
    t_clean();
    t_audio_hole();
    t_short_dropouts();
    t_audio_overlap();
    t_jitter();
    t_jump_back(0);
    t_jump_back(1);
    t_jump_forward_video_first();
    t_jump_forward_audio_first();
    t_slideshow();
    t_no_audio_gap();
    t_no_audio_short_gap();
    t_audio_only_jump();
    t_late_audio();
    t_early_audio();
    t_loop();
    t_no_timestamps();
    if (failures)
        printf("A/V SYNC TESTS: %d FAILURE(S)\n", failures);
    else
        printf("A/V SYNC TESTS PASSED\n");
    return failures ? 1 : 0;
}
