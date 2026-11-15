/*
 * rpi5datv - built-in test card generator (RGB24)
 */
#ifndef RPI5DATV_TESTCARD_H
#define RPI5DATV_TESTCARD_H

#include <stdint.h>

typedef struct {
    int width, height;
    const char *title;      /* large text, normally the service name */
    const char *subtitle;   /* provider */
    const char *info;       /* transmission parameters */
} testcard_t;

/* Render frame number 'frame' into rgb (width*height*3 bytes, stride width*3). */
void testcard_render(const testcard_t *tc, long frame, uint8_t *rgb);

/* 5x7 bitmap text, exposed for reuse. scale >= 1. */
void testcard_text(uint8_t *rgb, int w, int h, int x, int y, int scale,
                   const char *s, uint8_t r, uint8_t g, uint8_t b);
int testcard_text_width(const char *s, int scale);

#endif
