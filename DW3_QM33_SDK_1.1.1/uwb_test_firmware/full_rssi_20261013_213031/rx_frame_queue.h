/* Single interrupt producer, single main-loop consumer. Slots are owned until
 * released: reserving does not publish, and peeking does not free a record. */
#ifndef RX_FRAME_QUEUE_H
#define RX_FRAME_QUEUE_H

#include <stdint.h>
#include <string.h>
#include "rx_signal_metrics.h"

#ifndef TEST_RX_QUEUE_SIZE
#define TEST_RX_QUEUE_SIZE 64U
#endif
#if TEST_RX_QUEUE_SIZE < 2 || (TEST_RX_QUEUE_SIZE & (TEST_RX_QUEUE_SIZE - 1)) != 0
#error TEST_RX_QUEUE_SIZE must be a power of two greater than one
#endif
#ifndef RX_QUEUE_BARRIER
#error Define RX_QUEUE_BARRIER() as the platform memory/compiler barrier
#endif

#define RX_RECORD_FRAME_BYTES 32U
typedef struct
{
    uint32_t reception_id;
    uint32_t sequence;
    uint32_t epoch;
    uint16_t frame_len;
    uint8_t frame[RX_RECORD_FRAME_BYTES];
    rx_signal_snapshot_t signal;
} rx_frame_record_t;

typedef struct
{
    volatile uint32_t head;
    volatile uint32_t tail;
    rx_frame_record_t records[TEST_RX_QUEUE_SIZE];
} rx_frame_queue_t;

static void rx_frame_queue_init(rx_frame_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
}

/* Only the ISR calls reserve/commit. A full queue never overwrites a slot. */
static rx_frame_record_t *rx_frame_queue_reserve(rx_frame_queue_t *queue)
{
    uint32_t head = queue->head;
    uint32_t tail = queue->tail;
    RX_QUEUE_BARRIER();
    if ((uint32_t)(head - tail) >= TEST_RX_QUEUE_SIZE) { return NULL; }
    return &queue->records[head & (TEST_RX_QUEUE_SIZE - 1U)];
}

static void rx_frame_queue_commit(rx_frame_queue_t *queue)
{
    RX_QUEUE_BARRIER();
    queue->head++;
}

/* Only main calls peek/release. Keep the slot owned through ALL processing. */
static const rx_frame_record_t *rx_frame_queue_peek(rx_frame_queue_t *queue)
{
    uint32_t tail = queue->tail;
    if (tail == queue->head) { return NULL; }
    RX_QUEUE_BARRIER();
    return &queue->records[tail & (TEST_RX_QUEUE_SIZE - 1U)];
}

static void rx_frame_queue_release(rx_frame_queue_t *queue)
{
    RX_QUEUE_BARRIER();
    queue->tail++;
}

#endif
