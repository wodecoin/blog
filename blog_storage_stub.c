/**
 * @file    blog_storage_stub.c
 * @brief   Minimal storage backend template for BLOG ops.
 @author  yangfan
 * @date    2026-10-18
 *
 * @details
 * Replace the function bodies with FlashDB, raw flash, SPI flash, EEPROM,
 * RAM or any other storage implementation. BLOG core remains unchanged.
 */

#include "blog.h"

/** Example backend private context. */
typedef struct {
  uint32_t reserved;
} blog_storage_stub_t;

/**
 * @brief Store one BLOG record.
 */
static int storage_write(void *priv, const blog_record_t *record) {
  blog_storage_stub_t *ctx = (blog_storage_stub_t *)priv;

  (void)ctx;
  (void)record;

  /* Replace with a real write; do not report success for an unwritten log. */
  return -1;
}

/**
 * @brief Iterate persisted BLOG records.
 */
static int storage_read(void *priv, blog_read_cb_t callback, void *user) {
  blog_storage_stub_t *ctx = (blog_storage_stub_t *)priv;

  (void)ctx;
  (void)callback;
  (void)user;

  /*
   * TODO: for each persisted record:
   *
   *   if (callback(&record, user) != 0) {
   *       break;
   *   }
   */
  return 0;
}

/**
 * @brief Clear persisted BLOG records.
 */
static int storage_clear(void *priv) {
  blog_storage_stub_t *ctx = (blog_storage_stub_t *)priv;

  (void)ctx;

  /* TODO: clear storage. */
  return 0;
}

/** BLOG storage operation table. */
const blog_ops_t g_blog_storage_ops = {
    .write = storage_write,
    .read = storage_read,
    .clear = storage_clear,
};
