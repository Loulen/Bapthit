#ifndef SCORE_QUEUE_H
#define SCORE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_QUEUE_CAPACITY 64

typedef struct {
    int     id;
    int     score;
    int64_t ts_epoch;  // unix seconds, 0 if unknown
} score_item_t;

/** Initialize the singleton queue. Must be called once at boot. */
void score_queue_init(void);

/**
 * Push a score. If the queue is full, drops the oldest to make room
 * and logs a warning. Always succeeds.
 */
void score_queue_push(const score_item_t *item);

/** Peek the oldest item without removing it. Returns false if empty. */
bool score_queue_peek(score_item_t *out);

/** Remove the oldest item. No-op if empty. */
void score_queue_pop(void);

/** Number of items currently buffered. */
int score_queue_count(void);

#ifdef __cplusplus
}
#endif

#endif
