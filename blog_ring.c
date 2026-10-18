/**
 * @file    blog_ring.c
 * @brief   Minimal ring buffer used by BLOG.
 * @author  yangfan
 * @date    2026-10-18
 *
 * @details
 * Only three operations are exposed:
 * - push: append a record, overwriting the oldest one when full;
 * - peek: inspect the oldest record without consuming it;
 * - pop : consume a previously peeked record after storage succeeds.
 */

#include "blog.h"

#define BLOG_RING_MASK (BLOG_CACHE_NUM - 1u)

/**
 * @brief Convert monotonically increasing position to physical slot index.
 *
 * @param pos Monotonic read/write position.
 * @return Physical array index.
 */
static uint32_t blog_ring_index(uint32_t pos) { return pos & BLOG_RING_MASK; }

/**
 * @brief Push one record into the ring buffer.
 *
 * @param ring   Ring buffer object.
 * @param record Record to copy into the buffer.
 *
 * @return true if the oldest record was overwritten, false otherwise.
 *
 * @note BLOG_CACHE_NUM must be a power of two.
 */
bool blog_ring_push(blog_ring_t *ring, const blog_record_t *record) {
  bool overwritten = false;

  if ((ring->wr - ring->rd) >= BLOG_CACHE_NUM) {
    /* Full: discard the oldest pending record. */
    ring->rd++;
    overwritten = true;
  }

  ring->buf[blog_ring_index(ring->wr)] = *record;
  ring->wr++;

  return overwritten;
}

/**
 * @brief Peek the oldest record without consuming it.
 *
 * @param ring   Ring buffer object.
 * @param record Output copy of the oldest record.
 * @param ticket Output read-position ticket for a later safe pop.
 *
 * @return true when a record is available, false when the buffer is empty.
 */
bool blog_ring_peek(const blog_ring_t *ring, blog_record_t *record,
                    uint32_t *ticket) {
  if (ring->rd == ring->wr) {
    return false;
  }

  *ticket = ring->rd;
  *record = ring->buf[blog_ring_index(ring->rd)];

  return true;
}

/**
 * @brief Consume a previously peeked record.
 *
 * @param ring   Ring buffer object.
 * @param ticket Ticket returned by blog_ring_peek().
 *
 * @return true if the record was consumed, false if it was already
 *         overwritten or the ring became empty.
 */
bool blog_ring_pop(blog_ring_t *ring, uint32_t ticket) {
  if ((ring->rd != ticket) || (ring->rd == ring->wr)) {
    return false;
  }

  ring->rd++;
  return true;
}
