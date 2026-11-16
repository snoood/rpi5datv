/*
 * rpi5datv - DVB-S digital television transmitter for the Raspberry Pi 5 / 500
 *
 * One executable: video file or test card -> H.264/MP2 -> DVB transport
 * stream -> DVB-S channel coding -> QPSK -> RF on a GPIO pin via RP1 PIO.
 *
 * Derived in spirit from rpidatv by Evariste Courjaud F5OEO (GPL),
 * channel coding after Brian Jordan G4EWJ's dvbsenco. GPL v2 or later.
 *
 * Transmitting needs an appropriate licence (e.g. amateur radio) and
 * proper filtering: the raw GPIO output is rich in harmonics and images.
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/log.h>

#include "dvbs.h"
#include "modulator.h"
#include "pio_out.h"
#include "ringbuf.h"
#include "source.h"

#define PROGRAM_VERSION "1.0.0"

static volatile int g_stop;
static ringbuf_t g_ring;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct {
    /* programme */
    const char *input;
    int testcard;
    int loop;
    int audio;
    int width, height;
    const char *service;
    const char *provider;
    /* transmission */
    double symbol_rate;      /* symbols/s */
    dvbs_fec_t fec;
    double freq_hz;
    int rf_gpio;
    int ptt_gpio;
    int power;
    int pio_div;
    double pio_max_rate;
    int xfer_kb;
    /* file outputs (testing without a Pi) */
    const char *ts_out;
    const char *iq_out;
    int iq_sps;
    const char *rf_out;
    double duration;
    int plan_only;
    int verbose;
} options_t;

static void usage(FILE *f)
{
    fprintf(f,
"rpi5datv %s - DVB-S transmitter for Raspberry Pi 5 / 500 (RP1 PIO)\n"
"\n"
"Usage: rpi5datv (-i FILE | -t) -f MHZ [options]\n"
"\n"
"Programme:\n"
"  -i, --input FILE       video file to transmit (MP4/H.264+AAC preferred;\n"
"                         anything FFmpeg can decode will work)\n"
"  -t, --testcard         transmit the built-in test card (overrides -i)\n"
"  -l, --loop             loop the video file\n"
"  -n, --service NAME     DVB service name (default RPI5DATV)\n"
"  -p, --provider NAME    DVB service provider name (default RPI5DATV)\n"
"  -a, --no-audio         do not send an audio track\n"
"  -W, --size WxH         picture size (default: chosen from bit rate,\n"
"                         1024x576 when there is room, smaller 16:9 otherwise)\n"
"\n"
"Transmission:\n"
"  -s, --symbol-rate KS   symbol rate in kS/s, 125..4000 (default 333)\n"
"  -c, --fec RATE         1/2, 2/3, 3/4, 5/6 or 7/8 (default 1/2)\n"
"  -f, --frequency MHZ    RF output frequency in MHz (required for RF)\n"
"  -x, --gpio N           GPIO (BCM numbering) for the RF output (default 12,\n"
"                         header pin 32). Any of GPIO 0..27 may be used.\n"
"  -P, --ptt-gpio N       GPIO driven high while transmitting (default none)\n"
"  -d, --power 0..3       GPIO drive strength 2/4/8/12 mA (default 3)\n"
"      --pio-div N        force the PIO clock divider (fs = 200 MHz / N)\n"
"      --pio-max-rate MBS max PIO stream rate in MB/s for auto divider (default 9)\n"
"      --xfer-kb N        PIO DMA buffer size in KiB (default 32)\n"
"      --plan             print the frequency plan and exit\n"
"\n"
"Test outputs (no Pi needed, no RF):\n"
"      --ts-out FILE      also write the transmitted transport stream\n"
"      --iq-out FILE      write complex float32 baseband QPSK (rectangular)\n"
"      --iq-sps N         samples per symbol for --iq-out (default 4)\n"
"      --rf-out FILE      write the raw PIO bit stream (packed u32, LSB first)\n"
"      --duration SEC     stop after SEC seconds of transmitted signal\n"
"\n"
"  -v, --verbose\n"
"  -h, --help\n"
"\n"
"Example:\n"
"  rpi5datv -t -s 333 -c 1/2 -f 437 -n G0ABC -p \"Portsdown\"\n"
"  rpi5datv -i film.mp4 -l -s 1000 -c 3/4 -f 437 -x 12\n",
        PROGRAM_VERSION);
}

static int parse_args(int argc, char **argv, options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->symbol_rate = 333000;
    o->fec = FEC_1_2;
    o->rf_gpio = 12;
    o->ptt_gpio = -1;
    o->power = 3;
    o->pio_max_rate = 9e6;
    o->xfer_kb = 32;
    o->iq_sps = 4;
    o->audio = 1;
    o->service = "RPI5DATV";
    o->provider = "RPI5DATV";

    enum { O_PIODIV = 1000, O_PIOMAX, O_XFER, O_PLAN, O_TSOUT, O_IQOUT, O_IQSPS,
           O_RFOUT, O_DUR };
    static const struct option longopts[] = {
        { "input", required_argument, 0, 'i' },
        { "testcard", no_argument, 0, 't' },
        { "loop", no_argument, 0, 'l' },
        { "service", required_argument, 0, 'n' },
        { "provider", required_argument, 0, 'p' },
        { "no-audio", no_argument, 0, 'a' },
        { "size", required_argument, 0, 'W' },
        { "symbol-rate", required_argument, 0, 's' },
        { "fec", required_argument, 0, 'c' },
        { "frequency", required_argument, 0, 'f' },
        { "gpio", required_argument, 0, 'x' },
        { "ptt-gpio", required_argument, 0, 'P' },
        { "power", required_argument, 0, 'd' },
        { "pio-div", required_argument, 0, O_PIODIV },
        { "pio-max-rate", required_argument, 0, O_PIOMAX },
        { "xfer-kb", required_argument, 0, O_XFER },
        { "plan", no_argument, 0, O_PLAN },
        { "ts-out", required_argument, 0, O_TSOUT },
        { "iq-out", required_argument, 0, O_IQOUT },
        { "iq-sps", required_argument, 0, O_IQSPS },
        { "rf-out", required_argument, 0, O_RFOUT },
        { "duration", required_argument, 0, O_DUR },
        { "verbose", no_argument, 0, 'v' },
        { "help", no_argument, 0, 'h' },
        { 0, 0, 0, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:tln:p:aW:s:c:f:x:P:d:vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'i': o->input = optarg; break;
        case 't': o->testcard = 1; break;
        case 'l': o->loop = 1; break;
        case 'n': o->service = optarg; break;
        case 'p': o->provider = optarg; break;
        case 'a': o->audio = 0; break;
        case 'W':
            if (sscanf(optarg, "%dx%d", &o->width, &o->height) != 2 ||
                o->width < 64 || o->height < 64 || (o->width & 1) || (o->height & 1)) {
                fprintf(stderr, "bad --size '%s' (want even WxH, e.g. 1024x576)\n", optarg);
                return -1;
            }
            break;
        case 's': o->symbol_rate = atof(optarg) * 1000.0; break;
        case 'c':
            if (dvbs_parse_fec(optarg, &o->fec) < 0) {
                fprintf(stderr, "bad FEC '%s' (1/2, 2/3, 3/4, 5/6, 7/8)\n", optarg);
                return -1;
            }
            break;
        case 'f': o->freq_hz = atof(optarg) * 1e6; break;
        case 'x': o->rf_gpio = atoi(optarg); break;
        case 'P': o->ptt_gpio = atoi(optarg); break;
        case 'd': o->power = atoi(optarg); break;
        case O_PIODIV: o->pio_div = atoi(optarg); break;
        case O_PIOMAX: o->pio_max_rate = atof(optarg) * 1e6; break;
        case O_XFER: o->xfer_kb = atoi(optarg); break;
        case O_PLAN: o->plan_only = 1; break;
        case O_TSOUT: o->ts_out = optarg; break;
        case O_IQOUT: o->iq_out = optarg; break;
        case O_IQSPS: o->iq_sps = atoi(optarg); break;
        case O_RFOUT: o->rf_out = optarg; break;
        case O_DUR: o->duration = atof(optarg); break;
        case 'v': o->verbose = 1; break;
        case 'h': usage(stdout); exit(0);
        default: usage(stderr); return -1;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument '%s'\n", argv[optind]);
        return -1;
    }

    if (o->symbol_rate < 125000 || o->symbol_rate > 4000000) {
        fprintf(stderr, "symbol rate must be 125..4000 kS/s\n");
        return -1;
    }
    if (o->power < 0 || o->power > 3) {
        fprintf(stderr, "--power must be 0..3\n");
        return -1;
    }
    if (o->rf_gpio < 0 || o->rf_gpio > 27 || o->ptt_gpio > 27) {
        fprintf(stderr, "GPIO numbers must be 0..27\n");
        return -1;
    }
    if (o->pio_div < 0 || o->pio_div > 65535 || o->xfer_kb < 4 || o->xfer_kb > 4096 ||
        o->iq_sps < 1 || o->iq_sps > 64) {
        fprintf(stderr, "option out of range\n");
        return -1;
    }
    if (!o->plan_only && !o->testcard && !o->input) {
        fprintf(stderr, "need a video file (-i) or the test card (-t)\n");
        return -1;
    }
    if (strlen(o->service) > 64 || strlen(o->provider) > 64) {
        fprintf(stderr, "service/provider names are limited to 64 characters\n");
        return -1;
    }
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void *source_thread(void *arg)
{
    source_cfg_t *cfg = arg;
    intptr_t r = source_run(cfg);
    return (void *)r;
}

static void print_plan(const freqplan_t *fp, double sr)
{
    printf("RF plan: %.6f MHz = %s%d x %.3f MHz %c %.6f MHz%s\n",
           fp->actual_hz / 1e6, fp->image ? "" : "(direct) ", fp->image,
           fp->fs / 1e6, fp->inverted ? '-' : '+', fabs(fp->f0) / 1e6,
           fp->inverted ? " (inverted image, compensated)" : "");
    printf("         PIO divider %d, %.1f Msamples/s, %.2f MB/s to the PIO, "
           "%.1f samples/symbol\n",
           fp->div, fp->fs / 1e6, fp->fs / 8e6, fp->fs / sr);
    printf("         image level %.1f dB relative to a square wave at the "
           "target frequency\n", fp->gain_db);
    printf("         simulated MER %.1f dB\n", fp->mer_db);
    if (fp->spur_warning)
        printf("WARNING: low MER - aliased harmonics of the 1-bit carrier fall in "
               "band. A higher PIO rate (--pio-max-rate 13, i.e. divider 2) or a\n"
               "         lower symbol rate / another frequency will help.\n");
}

int main(int argc, char **argv)
{
    options_t opt;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (parse_args(argc, argv, &opt) < 0)
        return 2;
    av_log_set_level(opt.verbose ? AV_LOG_INFO : AV_LOG_WARNING);

    const int file_mode = opt.iq_out || opt.rf_out || opt.ts_out;
    const int use_pio = !file_mode;
    const int need_rf = use_pio || opt.rf_out || opt.plan_only;
    const double sr = opt.symbol_rate;
    const double ts_rate = dvbs_ts_bitrate(sr, opt.fec);

    freqplan_t fp;
    memset(&fp, 0, sizeof(fp));
    if (need_rf) {
        if (opt.freq_hz <= 0) {
            fprintf(stderr, "need an output frequency (-f MHz)\n");
            return 2;
        }
        char err[256];
        if (freqplan_choose(opt.freq_hz, sr, opt.pio_div, opt.pio_max_rate, &fp,
                            err, sizeof(err)) < 0) {
            fprintf(stderr, "%s\n", err);
            return 2;
        }
    }

    printf("rpi5datv %s: SR %.0f kS/s, FEC %s, TS rate %.0f bit/s\n",
           PROGRAM_VERSION, sr / 1000, dvbs_fec_name(opt.fec), ts_rate);
    if (need_rf)
        print_plan(&fp, sr);
    if (opt.plan_only)
        return 0;

    /* ---- programme source ---- */
    char info[160];
    if (need_rf)
        snprintf(info, sizeof(info), "%.3f MHZ  SR %.0f  FEC %s",
                 fp.actual_hz / 1e6, sr / 1000, dvbs_fec_name(opt.fec));
    else
        snprintf(info, sizeof(info), "SR %.0f  FEC %s", sr / 1000, dvbs_fec_name(opt.fec));

    source_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.input = opt.input;
    scfg.testcard = opt.testcard;
    scfg.loop = opt.loop;
    scfg.width = opt.width;
    scfg.height = opt.height;
    scfg.audio = opt.audio;
    scfg.ts_bitrate = ts_rate;
    scfg.service_name = opt.service;
    scfg.provider_name = opt.provider;
    scfg.info_line = info;
    scfg.verbose = opt.verbose;
    scfg.stop = &g_stop;
    scfg.ring = &g_ring;
    source_plan(&scfg);
    printf("Programme: %s, %dx%d@25, video %d bit/s, audio %s\n",
           opt.testcard ? "test card" : opt.input, scfg.width, scfg.height,
           scfg.video_bitrate,
           scfg.audio_bitrate ? (scfg.audio_channels == 2 ? "MP2 stereo" : "MP2 mono") : "none");
    if (scfg.audio_bitrate)
        printf("           audio %d bit/s\n", scfg.audio_bitrate);

    /* ring: about 1.5 s of transport stream, at least 256 KiB */
    size_t ring_size = (size_t)(ts_rate / 8 * 1.5);
    if (ring_size < 256 * 1024)
        ring_size = 256 * 1024;
    ring_size -= ring_size % TS_PACKET_SIZE;
    if (ringbuf_init(&g_ring, ring_size) < 0) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- outputs ---- */
    FILE *ts_f = NULL, *iq_f = NULL, *rf_f = NULL;
    if (opt.ts_out && !(ts_f = fopen(opt.ts_out, "wb"))) {
        perror(opt.ts_out);
        return 1;
    }
    if (opt.iq_out && !(iq_f = fopen(opt.iq_out, "wb"))) {
        perror(opt.iq_out);
        return 1;
    }
    if (opt.rf_out && !(rf_f = fopen(opt.rf_out, "wb"))) {
        perror(opt.rf_out);
        return 1;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, source_thread, &scfg) != 0) {
        fprintf(stderr, "cannot start encoder thread\n");
        return 1;
    }

    int exit_code = 0;
    int pio_open_ok = 0;
    if (use_pio && !g_stop) {
        pio_out_cfg_t pc = {
            .rf_gpio = opt.rf_gpio,
            .ptt_gpio = opt.ptt_gpio,
            .div = fp.div,
            .drive = opt.power,
            .xfer_bytes = (size_t)opt.xfer_kb * 1024,
            .xfer_count = 8,
        };
        if (pio_out_open(&pc) < 0) {
            g_stop = 1;
            exit_code = 1;
        } else {
            pio_open_ok = 1;
            printf("Transmitting on GPIO %d (Ctrl-C to stop)\n", opt.rf_gpio);
        }
    }

    /* Let the encoder get ahead before RF starts (receivers lock faster
     * on a clean stream than on a burst of null packets). */
    size_t prefill = (size_t)(ts_rate / 8 * 0.5);
    if (prefill > ring_size / 2)
        prefill = ring_size / 2;
    ringbuf_wait_fill(&g_ring, prefill, 15000);

    /* ---- modulator loop ---- */
    dvbs_enc_t enc;
    dvbs_init(&enc, opt.fec);
    dds_t dds;
    if (need_rf)
        dds_init(&dds, &fp, sr);

    uint8_t pkt[TS_PACKET_SIZE];
    uint8_t null_pkt[TS_PACKET_SIZE];
    memset(null_pkt, 0xFF, sizeof(null_pkt));
    null_pkt[0] = 0x47;
    null_pkt[1] = 0x1F;
    null_pkt[2] = 0xFF;
    null_pkt[3] = 0x10;

    uint8_t syms[RS_PACKET_SIZE * 8];
    size_t words_cap = need_rf ? dds_max_words(&dds, RS_PACKET_SIZE * 8) : 0;
    uint32_t *words = words_cap ? malloc(words_cap * sizeof(uint32_t)) : NULL;
    float *iqbuf = iq_f ? malloc(sizeof(float) * 2 * RS_PACKET_SIZE * 8 * opt.iq_sps) : NULL;

    const double amp = M_SQRT1_2;
    static const float sym_i[4] = { 1, -1, -1, 1 };
    static const float sym_q[4] = { 1, 1, -1, -1 };

    uint64_t total_syms = 0, packets = 0, nulls = 0, sync_err = 0;
    const uint64_t max_syms = opt.duration > 0 ? (uint64_t)(opt.duration * sr) : 0;
    double t_status = now_s();
    double onair_status = 0;
    int slow_reports = 0;

    /* The modulator must never stall for longer than the PIO DMA buffers
     * last (8 x 64 KiB = ~60 ms at 8 MB/s). Ask for real-time priority
     * (the installer grants CAP_SYS_NICE); the encoder thread was created
     * earlier and keeps normal priority. */
    if (use_pio) {
        struct sched_param sp = { .sched_priority = 50 };
        int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (e && opt.verbose)
            fprintf(stderr, "note: no real-time priority (%s); run ./install.sh "
                    "or use sudo for best timing\n", strerror(e));
    }

    while (!g_stop) {
        int r = ringbuf_read_exact(&g_ring, pkt, TS_PACKET_SIZE);
        const uint8_t *p = pkt;
        if (r < 0)
            break;                      /* source finished and drained */
        if (r == 0) {
            if (use_pio) {
                p = null_pkt;           /* keep the carrier going */
                nulls++;
            } else {
                ringbuf_wait_fill(&g_ring, TS_PACKET_SIZE, 200);
                continue;
            }
        }
        if (p[0] != 0x47) {
            sync_err++;
            p = null_pkt;
        }
        packets++;

        if (ts_f)
            fwrite(p, 1, TS_PACKET_SIZE, ts_f);

        int ns = dvbs_encode_packet(&enc, p, syms);
        total_syms += (uint64_t)ns;

        if (iq_f) {
            size_t k = 0;
            for (int i = 0; i < ns; i++)
                for (int j = 0; j < opt.iq_sps; j++) {
                    iqbuf[k++] = (float)(amp * sym_i[syms[i]]);
                    iqbuf[k++] = (float)(amp * sym_q[syms[i]]);
                }
            fwrite(iqbuf, sizeof(float), k, iq_f);
        }
        if (need_rf) {
            size_t nw = dds_modulate(&dds, syms, (size_t)ns, words);
            if (rf_f)
                fwrite(words, sizeof(uint32_t), nw, rf_f);
            if (pio_open_ok && nw) {
                int e = pio_out_write(words, nw);
                if (e < 0) {
                    fprintf(stderr, "PIO transfer failed (%d)\n", e);
                    exit_code = 1;
                    break;
                }
            }
        }

        if (max_syms && total_syms >= max_syms)
            break;

        double t = now_s();
        if (t - t_status >= 2.0) {
            /* In PIO mode writes block, so signal time advances exactly
             * with wall time unless the PIO/DMA path cannot keep up. */
            double onair = total_syms / sr;
            double speed = (onair - onair_status) / (t - t_status);
            t_status = t;
            onair_status = onair;
            size_t fill = ringbuf_fill(&g_ring);
            if (opt.verbose || nulls || sync_err)
                fprintf(stderr, "[%.1fs on air] speed x%.3f  buffer %3zu%%  packets %llu  "
                        "underrun-nulls %llu  sync-errors %llu\n",
                        onair, speed, fill * 100 / ring_size,
                        (unsigned long long)packets, (unsigned long long)nulls,
                        (unsigned long long)sync_err);
            if (use_pio && onair > 4.0 && speed < 0.98 && slow_reports++ < 5)
                fprintf(stderr, "WARNING: the PIO stream is running at x%.3f of real "
                        "time - the RF signal has gaps.\n"
                        "         Reduce the stream rate (--pio-max-rate / --pio-div) "
                        "or the CPU load.\n", speed);
        }
    }

    /* ---- shutdown ---- */
    g_stop = 1;
    ringbuf_close(&g_ring);
    void *thr_ret = NULL;
    pthread_join(th, &thr_ret);
    if ((intptr_t)thr_ret != 0)
        exit_code = 1;

    if (pio_open_ok)
        pio_out_close();
    if (ts_f)
        fclose(ts_f);
    if (iq_f)
        fclose(iq_f);
    if (rf_f)
        fclose(rf_f);
    free(words);
    free(iqbuf);
    ringbuf_free(&g_ring);

    printf("Sent %llu packets (%.1f s), %llu underrun nulls\n",
           (unsigned long long)packets, total_syms / sr, (unsigned long long)nulls);
    return exit_code;
}
