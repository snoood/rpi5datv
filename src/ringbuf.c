#include "ringbuf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int ringbuf_init(ringbuf_t *r, size_t size)
{
    memset(r, 0, sizeof(*r));
    r->buf = malloc(size);
    if (!r->buf)
        return -1;
    r->size = size;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    return 0;
}

void ringbuf_free(ringbuf_t *r)
{
    free(r->buf);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    r->buf = NULL;
}

int ringbuf_write(ringbuf_t *r, const uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->lock);
    while (len) {
        while (r->fill == r->size && !r->closed)
            pthread_cond_wait(&r->cond, &r->lock);
        if (r->closed) {
            pthread_mutex_unlock(&r->lock);
            return -1;
        }
        size_t space = r->size - r->fill;
        size_t chunk = len < space ? len : space;
        size_t first = r->size - r->head;
        if (first > chunk)
            first = chunk;
        memcpy(r->buf + r->head, data, first);
        memcpy(r->buf, data + first, chunk - first);
        r->head = (r->head + chunk) % r->size;
        r->fill += chunk;
        data += chunk;
        len -= chunk;
        pthread_cond_broadcast(&r->cond);
    }
    pthread_mutex_unlock(&r->lock);
    return 0;
}

int ringbuf_read_exact(ringbuf_t *r, uint8_t *data, size_t len)
{
    pthread_mutex_lock(&r->lock);
    if (r->fill < len) {
        /* closed: a trailing partial packet can never complete */
        int ret = r->closed ? -1 : 0;
        pthread_mutex_unlock(&r->lock);
        return ret;
    }
    size_t first = r->size - r->tail;
    if (first > len)
        first = len;
    memcpy(data, r->buf + r->tail, first);
    memcpy(data + first, r->buf, len - first);
    r->tail = (r->tail + len) % r->size;
    r->fill -= len;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
    return (int)len;
}

size_t ringbuf_fill(ringbuf_t *r)
{
    pthread_mutex_lock(&r->lock);
    size_t f = r->fill;
    pthread_mutex_unlock(&r->lock);
    return f;
}

void ringbuf_wait_fill(ringbuf_t *r, size_t level, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&r->lock);
    while (r->fill < level && !r->closed) {
        if (pthread_cond_timedwait(&r->cond, &r->lock, &ts) == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&r->lock);
}

void ringbuf_close(ringbuf_t *r)
{
    pthread_mutex_lock(&r->lock);
    r->closed = 1;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}
