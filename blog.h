/**
 * @file    blog.h
 * @brief   Lightweight binary BLOG core interface.
 * @author  yangfan
 * @date    2026-10-18
 */

#ifndef BLOG_H
#define BLOG_H

#include <stdbool.h>
#include <stdint.h>

#include "blog_ids.h"

#define BLOG_ARG_MAX    2u
#define BLOG_DATA_MAX   (BLOG_ARG_MAX * 4u)
#define BLOG_CACHE_NUM  16u
#define BLOG_ERR_BUSY   (-2)

#ifndef BLOG_ENABLE_ASYNC
#define BLOG_ENABLE_ASYNC 0u
#endif

#if (BLOG_ENABLE_ASYNC != 0u) && (BLOG_ENABLE_ASYNC != 1u)
#error "BLOG_ENABLE_ASYNC must be 0 or 1"
#endif

#if BLOG_ENABLE_ASYNC
#ifndef BLOG_BATCH_SIZE
#define BLOG_BATCH_SIZE 4u
#endif

#ifndef BLOG_ENABLE_PINGPONG
#define BLOG_ENABLE_PINGPONG 1u
#endif

#if (BLOG_ENABLE_PINGPONG != 0u) && (BLOG_ENABLE_PINGPONG != 1u)
#error "BLOG_ENABLE_PINGPONG must be 0 or 1"
#endif

#if BLOG_ENABLE_PINGPONG
#define BLOG_BATCH_NUM 2u
#else
#define BLOG_BATCH_NUM 1u
#endif
#endif

_Static_assert((BLOG_CACHE_NUM & (BLOG_CACHE_NUM - 1u)) == 0u,
               "BLOG_CACHE_NUM must be power of 2");

/** @brief One packed BLOG record. */
typedef struct
{
    uint32_t timestamp;
    uint16_t id;
    uint8_t  desc; //2-bit width code per argument: 00=1 byte, 01=2 bytes, 10=4 bytes
    uint8_t  len;
    uint8_t  data[BLOG_DATA_MAX];
} blog_record_t;

/** @brief BLOG RAM ring buffer. */
typedef struct
{
    blog_record_t buf[BLOG_CACHE_NUM];
    uint32_t rd;
    uint32_t wr;
} blog_ring_t;

typedef int (*blog_read_cb_t)(const blog_record_t *record, void *user);
typedef struct blog_ctx blog_ctx_t;
#if BLOG_ENABLE_ASYNC
typedef void (*blog_batch_done_cb_t)(void *user, int result);
#endif

/** @brief Storage backend operations. */
typedef struct
{
    int (*write)(void *priv, const blog_record_t *record);
    int (*read)(void *priv, blog_read_cb_t callback, void *user);
    int (*clear)(void *priv);
#if BLOG_ENABLE_ASYNC
    /*
     * Return 0 only when accepted. The backend must invoke done exactly once
     * after it stops accessing records; on immediate failure it must not call
     * done. A failed completion leaves the batch queued for retry.
     */
    int (*write_batch_async)(void *priv,
                             const blog_record_t *records,
                             uint8_t count,
                             blog_batch_done_cb_t done,
                             void *user);
#endif
} blog_ops_t;

#if BLOG_ENABLE_ASYNC
typedef struct
{
    blog_record_t records[BLOG_BATCH_SIZE];
    uint8_t count;
    uint8_t state;
    uint32_t sequence;
} blog_batch_t;
#endif

/** @brief BLOG runtime context. */
struct blog_ctx
{
    blog_ring_t ring;
#if BLOG_ENABLE_ASYNC
    blog_batch_t batches[BLOG_BATCH_NUM];
    uint32_t batch_sequence;
#endif
    uint8_t storage_busy;
    uint32_t drop;
    const blog_ops_t *ops;
    void *priv;
};

extern blog_ctx_t g_blog;
#define BLOG_ARG_SIZE(x)                         \
    _Generic((x),                                \
        _Bool:              1u,                  \
        char:               1u,                  \
        signed char:        1u,                  \
        unsigned char:      1u,                  \
        short:              2u,                  \
        unsigned short:     2u,                  \
        int:                4u,                  \
        unsigned int:       4u,                  \
        long:               sizeof(long),        \
        unsigned long:      sizeof(unsigned long), \
        default:            0u)

#define BLOG_ARG_CODE(x)                         \
    ((BLOG_ARG_SIZE(x) == 1u) ? 0u :             \
     (BLOG_ARG_SIZE(x) == 2u) ? 1u :             \
     (BLOG_ARG_SIZE(x) == 4u) ? 2u : 3u)

#define BLOG_ARG_CHECK(x)                        \
     _Static_assert(                              \
        BLOG_ARG_SIZE(x) == 1u ||                \
        BLOG_ARG_SIZE(x) == 2u ||                \
        BLOG_ARG_SIZE(x) == 4u,                  \
        "BLOG unsupported argument type")

#define BLOG_NARG_I(_0,_1,_2,_3,_4,N,...) N
#define BLOG_NARG(...) \
    BLOG_NARG_I(_, ##__VA_ARGS__, 4, 3, 2, 1, 0)

#define BLOG_CAT_I(a,b) a##b
#define BLOG_CAT(a,b) BLOG_CAT_I(a,b)

#define BLOG_IMPL_0(tag)                         \
    do {                                         \
        blog_push(&g_blog, BLOG_ID_##tag,        \
                  0u, 0u, NULL);                 \
    } while (0)

#define BLOG_IMPL_1(tag,a0)                      \
    do {                                         \
        BLOG_ARG_CHECK(a0);                      \
        blog_push(&g_blog, BLOG_ID_##tag, 1u,    \
                  BLOG_ARG_CODE(a0),             \
                  (const uint32_t[]){            \
                      (uint32_t)(a0)});           \
    } while (0)

#define BLOG_IMPL_2(tag,a0,a1)                   \
    do {                                         \
        BLOG_ARG_CHECK(a0);                      \
        BLOG_ARG_CHECK(a1);                      \
        blog_push(&g_blog, BLOG_ID_##tag, 2u,    \
                  (BLOG_ARG_CODE(a0) << 0) |     \
                  (BLOG_ARG_CODE(a1) << 2),      \
                  (const uint32_t[]){            \
                      (uint32_t)(a0),            \
                      (uint32_t)(a1)});           \
    } while (0)

#define BLOG_IMPL_3(tag,a0,a1,a2)                \
    do {                                         \
        _Static_assert(BLOG_ARG_MAX >= 3u,       \
                       "BLOG_ARG_MAX is less than 3"); \
        BLOG_ARG_CHECK(a0);                      \
        BLOG_ARG_CHECK(a1);                      \
        BLOG_ARG_CHECK(a2);                      \
        blog_push(&g_blog, BLOG_ID_##tag, 3u,    \
                  (BLOG_ARG_CODE(a0) << 0) |     \
                  (BLOG_ARG_CODE(a1) << 2) |     \
                  (BLOG_ARG_CODE(a2) << 4),      \
                  (const uint32_t[]){            \
                      (uint32_t)(a0),            \
                      (uint32_t)(a1),            \
                      (uint32_t)(a2)});           \
    } while (0)

#define BLOG_IMPL_4(tag,a0,a1,a2,a3)             \
    do {                                         \
        _Static_assert(BLOG_ARG_MAX >= 4u,       \
                       "BLOG_ARG_MAX is less than 4"); \
        BLOG_ARG_CHECK(a0);                      \
        BLOG_ARG_CHECK(a1);                      \
        BLOG_ARG_CHECK(a2);                      \
        BLOG_ARG_CHECK(a3);                      \
        blog_push(&g_blog, BLOG_ID_##tag, 4u,    \
                  (BLOG_ARG_CODE(a0) << 0) |     \
                  (BLOG_ARG_CODE(a1) << 2) |     \
                  (BLOG_ARG_CODE(a2) << 4) |     \
                  (BLOG_ARG_CODE(a3) << 6),      \
                  (const uint32_t[]){            \
                      (uint32_t)(a0),            \
                      (uint32_t)(a1),            \
                      (uint32_t)(a2),            \
                      (uint32_t)(a3)});           \
    } while (0)
// ##__VA_ARGS__：GNU C 扩展写法，用于处理“没有可变参数”时，连同前面的逗号一起去掉
// 这里根据算出来的参数个数，选择不同的宏实现，最终调用 blog_push() 函数将日志记录压入环形缓冲区。
#define BLOG_DISPATCH(tag, ...)                  \
    BLOG_CAT(BLOG_IMPL_, BLOG_NARG(__VA_ARGS__)) (tag, ##__VA_ARGS__)

#define BLOG_D(tag, fmt, ...) BLOG_DISPATCH(tag, ##__VA_ARGS__)
#define BLOG_I(tag, fmt, ...) BLOG_DISPATCH(tag, ##__VA_ARGS__)
#define BLOG_W(tag, fmt, ...) BLOG_DISPATCH(tag, ##__VA_ARGS__)
#define BLOG_E(tag, fmt, ...) BLOG_DISPATCH(tag, ##__VA_ARGS__)

int blog_init(blog_ctx_t *ctx, const blog_ops_t *ops, void *priv);
int blog_push(blog_ctx_t *ctx,
              uint16_t id,
              uint8_t argc,
              uint8_t desc,
              const uint32_t *args);
int blog_process(blog_ctx_t *ctx);
#if BLOG_ENABLE_ASYNC
/* Prepare batches and submit one at a time; backend completion is asynchronous. */
int blog_process_async(blog_ctx_t *ctx);
#endif
int blog_read(blog_ctx_t *ctx, blog_read_cb_t callback, void *user);
int blog_clear(blog_ctx_t *ctx);

// 读取日志丢失计数的接口
static inline uint32_t blog_drop_count(const blog_ctx_t *ctx)
{
    return ctx->drop;
}

bool blog_ring_push(blog_ring_t *ring, const blog_record_t *record);
bool blog_ring_peek(const blog_ring_t *ring,
                    blog_record_t *record,
                    uint32_t *ticket);
bool blog_ring_pop(blog_ring_t *ring, uint32_t ticket);

uint32_t blog_port_timestamp(void);
uint32_t blog_port_lock(void);
void blog_port_unlock(uint32_t key);
#endif /* BLOG_H */
