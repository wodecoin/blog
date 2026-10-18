# BLOG Core V1

轻量级嵌入式二进制日志核心。

目标：业务侧保持接近 `printf` 的使用体验，同时减少 MCU 侧字符串和日志存储开销。

## 1. 特性

- `BLOG_D / BLOG_I / BLOG_W / BLOG_E` 四级日志接口
- TAG 对应自动生成的 `uint16_t` Log ID
- `printf` 格式字符串仅用于 PC 侧日志字典，不要求存入 MCU 日志记录
- C11 `_Generic` 自动识别 1 / 2 / 4 Byte 参数宽度
- 参数以紧凑二进制形式保存
- 内置固定容量 RingBuffer
- RingBuffer 满时覆盖最老日志，优先保留最新日志
- `peek + ticket + pop` 避免慢存储期间误删新日志
- 可选异步批量写入及乒乓缓冲，适配 DMA / 异步存储
- `ctx + ops + priv` 方式解耦具体存储后端
- Core 不依赖 FlashDB，可接 FlashDB、裸 Flash、SPI Flash、RAM 等

## 2. 文件结构

```text
blog_core_v1/
├── blog.h
├── blog.c
├── blog_ring.c
├── blog_port.c
├── blog_storage_stub.c
├── blog_ids.h
└── README.md
```

各文件职责：

- `blog.h`：公开 API、数据结构、`_Generic` 参数识别、BLOG 宏
- `blog.c`：BLOG Core，负责参数打包、缓存、后台写入、读取和清理
- `blog_ring.c`：RingBuffer，仅实现 `push / peek / pop`
- `blog_port.c`：平台适配，例如 RTX5 Tick 和 Cortex-M 临界区
- `blog_storage_stub.c`：Storage OPS 示例模板
- `blog_ids.h`：Log ID 示例，正式版本建议由脚本自动生成

## 3. 整体数据流

```text
BLOG_I/W/E/D
    |
    v
blog_push()
    |
    v
RAM RingBuffer
    +--> blog_process() ----------------> ops->write()
    |
    `--> blog_process_async() ---------> ops->write_batch_async()
          (BLOG_ENABLE_ASYNC == 1)
```

默认配置是同步模式：RingBuffer 接收业务日志，任务调用 `blog_process()`，
逐条交给 `ops->write()`，不分配批次缓冲区。若需异步批量写入，可在包含
`blog.h` 前定义 `BLOG_ENABLE_ASYNC` 为 `1`，并实现
`ops->write_batch_async()`。异步模式下，RingBuffer 仍然接收业务日志，任务收到
信号量后调用 `blog_process_async()`。启用乒乓缓冲时，两个批次缓冲区可以交替准备；
关闭时只使用一个批次缓冲区。同一时刻只提交一个批次，避免对不支持并发写的
Flash / DMA 后端同时发起两次操作。已提交
的缓冲区由后端持有，直到 DMA / 存储完成并调用完成回调。后端随后应唤醒
日志任务，使下一个批次可以提交。

启用异步模式和乒乓缓冲的配置示例：

```c
#define BLOG_ENABLE_ASYNC 1u
#define BLOG_ENABLE_PINGPONG 1u /* 0: 单批次缓冲；1: 双批次缓冲 */
#include "blog.h"
```

也可以在编译器的预处理宏设置中定义这些开关。默认
`BLOG_ENABLE_ASYNC == 0`，批次缓冲区、异步接口和相关实现均不编译进模块。
若定义 `BLOG_ENABLE_ASYNC == 1` 而不设置 `BLOG_ENABLE_PINGPONG`，默认使用双缓冲。
单缓冲模式仍支持异步批量写入，只是写入期间不能提前准备下一批。由于该开关会
改变公开结构体和 `blog_ops_t` 的布局，整个工程所有包含 `blog.h` 的源文件必须
使用一致的宏配置；建议在项目的全局编译设置中定义。

读取方向：

```text
AT / Application
    |
    v
blog_read()
    |
    v
ops->read()
    |
    v
Storage Backend
```

## 4. 业务使用方式

```c
uint16_t handle = 1;
uint8_t  reason = 8;

BLOG_W(
    BLE_DISCONNECT,
    "disconnect handle=%u reason=%u",
    handle,
    reason);
```

业务代码不需要写：

```text
U8(reason)
U16(handle)
%hhu
%hu
0xA312
```

`_Generic` 会自动识别：

```text
handle : uint16_t -> 2 Byte
reason : uint8_t  -> 1 Byte
```

最终生成的二进制参数类似：

```text
ID   : A312
DESC : 01
DATA : 01 00 08 BD
```

## 5. 参数 Descriptor

每个参数使用 2 bit 描述实际宽度：

```text
00 -> 1 Byte
01 -> 2 Byte
10 -> 4 Byte
11 -> Reserved
```

最多 4 个参数，因此一个 `uint8_t desc` 即可描述全部参数宽度。

参数的“存储宽度”和“显示格式”职责分离：

```text
C 实参类型
    |
    v
_Generic
    |
    v
DESC
决定存几 Byte
```

```text
printf format
    |
    v
PC log dictionary
    |
    v
决定按 %d / %u / %X 等方式显示
```

## 6. RingBuffer

RingBuffer 对 BLOG 只暴露 3 个动作：

```c
bool blog_ring_push(...);
bool blog_ring_peek(...);
bool blog_ring_pop(...);
```

### push

写入最新日志。

如果 RingBuffer 已满，会自动移动 `rd`，覆盖最老日志：

```text
A B C D   + E

满
 |
 v
丢 A

B C D E
```

BLOG 会通过 `ctx->drop` 统计发生过多少次覆盖。

### peek

读取当前最老日志，但不删除。

```text
peek = 看一眼，不消费
```

这样可以先尝试：

```c
ops->write(...);
```

如果存储失败，日志仍留在 RingBuffer 中，下次可以重试。

### pop

只有在 Storage 写成功后才调用。

```text
peek
  |
  v
ops->write
  |
  +-- fail --> 保留
  |
  `-- ok ----> pop
```

### ticket

`peek()` 同时返回一个 `ticket`，本质上是当时的 `rd`。

如果慢速 `ops->write()` 执行期间 RingBuffer 被写满，并且该日志已经被覆盖，则 `pop(ticket)` 不会误删后面的新日志。

## 7. 异步批量写入与乒乓缓冲

当存储驱动使用 DMA 或异步写接口时，可选择 `write_batch_async` 和
`blog_process_async()`，不需要在业务日志宏中执行存储操作：

```text
业务 / idle 唤醒
       |
       v
日志任务调用 blog_process_async()
       |
       +--> A: DMA 正在写，后端持有
       `--> B: 准备下一批，等待 A 完成后提交
```

每个缓冲区最多容纳 `BLOG_BATCH_SIZE` 条记录；仅在定义
`BLOG_ENABLE_ASYNC == 1` 时提供异步批量写入。异步模式默认启用双缓冲
（`BLOG_ENABLE_PINGPONG == 1`，即 `BLOG_BATCH_NUM == 2`）。核心同一时刻只允许
一个批次处于 DMA 写入状态；启用双缓冲时，另一个缓冲区可先装好下一批。
成功提交后，后端必须在异步操作完全结束时
恰好调用一次传入的 `done(user, result)` 回调。回调返回前，后端不得继续
访问该批记录。若提交函数立即返回非零，则表示没有接受该批次，也不得调用
完成回调；该批数据会留在缓冲区，下次 `blog_process_async()` 可重试。

DMA 完成回调之后，后端应释放 / 发送日志任务的信号量。`blog_clear()` 在
仍有 DMA 写入时返回 `BLOG_ERR_BUSY`，避免清理过程中后端继续写入旧批次。

同一个上下文应选择一种处理方式：同步后端配 `blog_process()`，或异步批量
后端配 `blog_process_async()`，不要混合调用。异步双缓冲和 RingBuffer 都是
有限容量；若生产速度长期超过存储速度，RingBuffer 满时仍会按既有策略覆盖
旧日志并增加 `drop`。

典型任务结构如下，idle hook 只发信号量，存储操作留在日志任务执行：

```c
for (;;) {
    osSemaphoreAcquire(log_sem, osWaitForever);
    int ret = blog_process_async(&g_blog);
    if (ret != 0) {
        /* Record/report the backend submission error and arrange a retry. */
    }
}
```

DMA 完成中断中，后端先调用传入的 `done(user, result)`，再释放
`log_sem`，这样失败批次可以重试，成功后也能继续提交下一批。

## 8. ctx + ops

BLOG 使用简单的 C 面向对象方式：

```text
blog_ctx_t
    |
    +-- ring
    +-- drop
    +-- ops
    `-- priv
```

其中：

- `ctx`：保存 BLOG 实例状态
- `ops`：定义存储行为
- `priv`：指向具体存储后端私有上下文

Storage OPS：

```c
typedef struct
{
    int (*write)(
        void *priv,
        const blog_record_t *record);

    int (*read)(
        void *priv,
        blog_read_cb_t callback,
        void *user);

    int (*clear)(
        void *priv);

} blog_ops_t;
```

因此 BLOG Core 不关心底层实际使用：

```text
FlashDB
Raw Flash
SPI Flash
RAM
EEPROM
```

## 8. 初始化

先实现自己的 Storage OPS：

```c
static const blog_ops_t g_ops =
{
    .write = storage_write,
    .read  = storage_read,
    .clear = storage_clear,
};
```

然后初始化：

```c
blog_init(
    &g_blog,
    &g_ops,
    &storage_ctx);
```

## 9. 后台处理

不要在业务线程或 ISR 中直接执行慢速 Storage 操作。

在已有低优先级线程中周期调用：

```c
void background_task(void *arg)
{
    for (;;)
    {
        blog_process(&g_blog);
        osDelay(10);
    }
}
```

流程：

```text
业务 / ISR
    |
    v
BLOG_xxx
    |
    v
RAM RingBuffer
    |
    `---- 快速返回

后台任务
    |
    v
blog_process
    |
    v
ops->write
```

## 10. 平台接口

BLOG Core 需要平台提供：

```c
uint32_t blog_port_timestamp(void);
uint32_t blog_port_lock(void);
void blog_port_unlock(uint32_t key);
```

RTX5 + Cortex-M 示例已经放在 `blog_port.c`。

临界区只用于保护 RingBuffer 的 `rd / wr` 和相关状态。

Storage 写入不能放在临界区内。

## 11. 读取日志

读取由 Storage Backend 实现遍历，再通过 callback 返回：

```c
static int dump_cb(
    const blog_record_t *record,
    void *user)
{
    /* 输出 ID / DESC / DATA */
    return 0;
}
```

调用：

```c
blog_read(
    &g_blog,
    dump_cb,
    NULL);
```

可用于后续：

```text
AT+LOG=DUMP
```

PC 再根据 `Log ID + log_dict.json` 恢复原始 `printf` 日志。

## 12. 自动 Log ID

`blog_ids.h` 当前只是占位示例。

正式工程建议使用 `gen_blog.py` 扫描：

```c
BLOG_W(
    BLE_DISCONNECT,
    "disconnect handle=%u reason=%u",
    ...);
```

生成：

```c
#define BLOG_ID_BLE_DISCONNECT 0xA312
```

以及 PC 字典：

```json
{
    "0xA312": {
        "tag": "BLE_DISCONNECT",
        "level": "WARN",
        "fmt": "disconnect handle=%u reason=%u"
    }
}
```

推荐 ID 根据稳定 TAG 生成，并在生成阶段检查 16 bit ID 碰撞。

## 13. V1 限制

V1 建议只支持：

```text
参数数量 : 当前配置 0 ~ 2（最多可配置到 4）
参数宽度 : 1 / 2 / 4 Byte
整数类型 : uint8_t / int8_t
           uint16_t / int16_t
           uint32_t / int32_t
```

格式字符串主要支持：

```text
%d
%i
%u
%x
%X
%c
```

以及常规宽度和补零，例如：

```text
%02X
%04X
%08X
```

V1 暂不支持：

```text
%s
float / double
64 bit integer
pointer
动态 bytes
```

## 14. `_Generic` 注意事项

`_Generic` 判断的是 C 表达式的最终类型。

例如：

```c
uint8_t value;
```

直接传：

```c
BLOG_I(TEST, "v=%u", value);
```

按 1 Byte 保存。

但：

```c
BLOG_I(TEST, "v=%u", value + 1);
```

由于 C integer promotion，`value + 1` 的类型是 `int`，因此按 4 Byte 保存。

这属于正常的 C 语言类型规则。

## 15. 配置建议

当前默认：

```c
#define BLOG_ARG_MAX    2u
#define BLOG_CACHE_NUM  16u
```

每条 RAM Record 最大约 16 Byte。

16 个 Slot 大约占用：

```text
16 x 16 = 256 Byte RAM
```

RingBuffer 满时覆盖最老日志，因此 16 个 Slot 均可使用。
若修改 `BLOG_ARG_MAX`，3 或 4 参数的日志调用会在编译期检查配置是否足够。
同步调用 `blog_process()` 与 `blog_clear()` 时由 `storage_busy` 阻止两者并发操作；
遇到已有存储操作时返回 `BLOG_ERR_BUSY`。

## 16. 后续扩展

V1 跑稳定后，可以逐步增加：

- 自动 `gen_blog.py`
- `log_dict.json`
- FlashDB Storage OPS
- AT Dump
- PC Parser
- `%s`
- 64 bit 参数
- Build ID / Dictionary CRC
- 日志级别编译裁剪
- CP / MP 双核独立 BLOG Context

建议先保持 Core 不变，所有扩展尽量放在 `ops` 和 PC 工具侧。
