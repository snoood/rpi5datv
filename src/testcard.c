/*
 * rpi5datv - built-in test card generator
 *
 * 75% colour bars, a grey scale with a moving marker (so a frozen
 * picture is obvious), a centre circle, and a caption band with the
 * service name, provider, transmission parameters and a running clock.
 */
#include "testcard.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- 5x7 font: rows top to bottom, bit 4 = leftmost pixel ---------- */
typedef struct { char c; uint8_t rows[7]; } glyph_t;

static const glyph_t font[] = {
    {' ', {0, 0, 0, 0, 0, 0, 0}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {'\'', {0x0C, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'*', {0x00, 0x04, 0x15, 0x0E, 0x15, 0x04, 0x00}},
    {'@', {0x0E, 0x11, 0x01, 0x0D, 0x15, 0x15, 0x0E}},
    {'&', {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}},
};

static const uint8_t *glyph_rows(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++)
        if (font[i].c == c)
            return font[i].rows;
    return glyph_rows('?');
}

int testcard_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    return n ? (n * 6 - 1) * scale : 0;
}

static inline void put_px(uint8_t *rgb, int w, int h, int x, int y,
                          uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    uint8_t *p = rgb + ((size_t)y * w + x) * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

static void fill_rect(uint8_t *rgb, int w, int h, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++) {
        uint8_t *p = rgb + ((size_t)y * w + x0) * 3;
        for (int x = x0; x < x1; x++) {
            *p++ = r;
            *p++ = g;
            *p++ = b;
        }
    }
}

void testcard_text(uint8_t *rgb, int w, int h, int x, int y, int scale,
                   const char *s, uint8_t r, uint8_t g, uint8_t b)
{
    for (; *s; s++, x += 6 * scale) {
        const uint8_t *rows = glyph_rows(*s);
        for (int ry = 0; ry < 7; ry++)
            for (int rx = 0; rx < 5; rx++)
                if (rows[ry] & (0x10 >> rx))
                    fill_rect(rgb, w, h, x + rx * scale, y + ry * scale,
                              x + (rx + 1) * scale, y + (ry + 1) * scale, r, g, b);
    }
}

/* Largest scale (<= max) at which s fits in width avail. */
static int fit_scale(const char *s, int avail, int max)
{
    int sc = max;
    while (sc > 1 && testcard_text_width(s, sc) > avail)
        sc--;
    return sc;
}

static void centred_text(uint8_t *rgb, int w, int h, int y, int scale,
                         const char *s, uint8_t r, uint8_t g, uint8_t b)
{
    int tw = testcard_text_width(s, scale);
    testcard_text(rgb, w, h, (w - tw) / 2, y, scale, s, r, g, b);
}

void testcard_render(const testcard_t *tc, long frame, uint8_t *rgb)
{
    const int w = tc->width, h = tc->height;

    /* background */
    fill_rect(rgb, w, h, 0, 0, w, h, 96, 96, 96);

    /* 75% colour bars: white yellow cyan green magenta red blue black */
    static const uint8_t bars[8][3] = {
        {191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
        {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {0, 0, 0},
    };
    const int bars_bottom = h * 52 / 100;
    for (int i = 0; i < 8; i++)
        fill_rect(rgb, w, h, w * i / 8, 0, w * (i + 1) / 8, bars_bottom,
                  bars[i][0], bars[i][1], bars[i][2]);

    /* grey scale, 8 steps */
    const int grey_top = bars_bottom, grey_bottom = h * 64 / 100;
    for (int i = 0; i < 8; i++) {
        uint8_t v = (uint8_t)(i * 255 / 7);
        fill_rect(rgb, w, h, w * i / 8, grey_top, w * (i + 1) / 8, grey_bottom, v, v, v);
    }

    /* moving marker across the grey scale: one sweep every 4 s */
    {
        int mw = w / 32 > 4 ? w / 32 : 4;
        long period = 100;
        long ph = frame % (2 * period);
        long pos = ph < period ? ph : 2 * period - ph;
        int x = (int)((w - mw) * pos / period);
        fill_rect(rgb, w, h, x, grey_top, x + mw, grey_bottom, 255, 64, 0);
    }

    /* centre circle */
    {
        int cx = w / 2, cy = bars_bottom / 2;
        int rad = bars_bottom * 2 / 5;
        int th = h / 144 > 1 ? h / 144 : 1;
        for (int y = cy - rad - th; y <= cy + rad + th; y++)
            for (int x = cx - rad - th; x <= cx + rad + th; x++) {
                double d = sqrt((double)(x - cx) * (x - cx) + (double)(y - cy) * (y - cy));
                if (fabs(d - rad) <= th)
                    put_px(rgb, w, h, x, y, 255, 255, 255);
            }
        /* cross hair */
        fill_rect(rgb, w, h, cx - rad, cy - th / 2, cx + rad, cy - th / 2 + th, 255, 255, 255);
        fill_rect(rgb, w, h, cx - th / 2, cy - rad, cx - th / 2 + th, cy + rad, 255, 255, 255);
    }

    /* caption band */
    const int band_top = grey_bottom;
    fill_rect(rgb, w, h, 0, band_top, w, h, 0, 0, 0);
    const int band_h = h - band_top;
    const int margin = w / 32;
    const int avail = w - 2 * margin;

    char clockline[96];
    {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        snprintf(clockline, sizeof(clockline), "%02d:%02d:%02d   FRAME %06ld",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, frame);
    }

    const char *title = (tc->title && *tc->title) ? tc->title : "RPI5DATV";
    /* vertical budget: title 9*s + three small lines of 10*(s/2) ~= 24*s */
    int s_title = fit_scale(title, avail, band_h / 27 > 1 ? band_h / 27 : 1);
    int s_small = fit_scale(tc->info ? tc->info : "", avail, s_title > 2 ? s_title / 2 : 1);
    if (s_small < 1)
        s_small = 1;

    int y = band_top + band_h / 14;
    centred_text(rgb, w, h, y, s_title, title, 255, 255, 255);
    y += 9 * s_title;
    if (tc->subtitle && *tc->subtitle) {
        centred_text(rgb, w, h, y, s_small, tc->subtitle, 255, 220, 0);
        y += 10 * s_small;
    }
    if (tc->info && *tc->info) {
        centred_text(rgb, w, h, y, s_small, tc->info, 160, 200, 255);
        y += 10 * s_small;
    }
    centred_text(rgb, w, h, y, s_small, clockline, 200, 200, 200);
}
