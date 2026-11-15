/*
 * rpi5datv - blocking byte ring buffer between the encoder thread
 * (producer of transport stream) and the modulator (consumer).
 */
#ifndef RPI5DATV_RINGBUF_H
#define RPI5DATV_RINGBUF_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t size;
    size_t head, tail;      /* head = write index, tail = read index */
    size_t fill;
    int closed;             /* producer finished or shutting down */
    pthread_mutex_t lock;
    pthread_cond_t cond;
} ringbuf_t;

int ringbuf_init(ringbuf_t *r, size_t size);
void ringbuf_free(ringbuf_t *r);

/* Blocks while full. Returns -1 if the ring was closed. */
int ringbuf_write(ringbuf_t *r, const uint8_t *data, size_t len);

/*
 * Non-blocking read of exactly len bytes. Returns len, or 0 if not
 * enough data is available (nothing consumed), or -1 if closed and empty.
 */
int ringbuf_read_exact(ringbuf_t *r, uint8_t *data, size_t len);

size_t ringbuf_fill(ringbuf_t *r);
/* Wait until fill >= level, closed, or timeout_ms elapsed. */
void ringbuf_wait_fill(ringbuf_t *r, size_t level, int timeout_ms);
void ringbuf_close(ringbuf_t *r);

#endif
