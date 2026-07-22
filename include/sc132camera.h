#ifndef SC132_CAMERA_H
#define SC132_CAMERA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC132_ABI_VERSION_MAJOR 2U
#define SC132_ABI_VERSION_MINOR 0U

#define SC132_STATUS_OK ((int32_t)0)
#define SC132_STATUS_INVALID_ARGUMENT ((int32_t)-1)
#define SC132_STATUS_INVALID_STATE ((int32_t)-2)
#define SC132_STATUS_STARTUP_FAILED ((int32_t)-3)

#define SC132_FRAME_SET_MAX_CAMERAS 4U
#define SC132_NATIVE_OUTPUT_WIDTH 1280U
#define SC132_NATIVE_OUTPUT_HEIGHT 1088U
/* 30 帧四路同一曝光帧实测存在约 1.06 毫秒的视频链路时间戳相位差，默认 2 毫秒可避免误丢同帧且仍远小于单帧周期。 */
#define SC132_FRAME_SET_DEFAULT_MAX_SKEW_NS 2000000ULL

typedef struct sc132_frame sc132_frame_t;

typedef struct sc132_frame_info {
  uint32_t struct_size;
  uint32_t camera_id;
  uint64_t sequence;
  uint32_t frame_id;
  uint32_t reserved0;
  uint64_t timestamp_ns;
  const void *y_data;
  const void *uv_data;
  uint64_t y_phys;
  uint64_t uv_phys;
  uint64_t y_size;
  uint64_t uv_size;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t vstride;
  uint32_t reserved[8];
} sc132_frame_info_t;

typedef struct sc132_frame_set_item {
  sc132_frame_t *frame;
  uint32_t camera_id;
  uint32_t frame_id;
  uint64_t sequence;
  uint64_t timestamp_ns;
  uint32_t width;
  uint32_t height;
} sc132_frame_set_item_t;

typedef struct sc132_frame_set {
  uint32_t struct_size;
  uint32_t camera_count;
  uint64_t group_id;
  uint64_t group_timestamp_ns;
  uint64_t max_skew_ns;
  sc132_frame_set_item_t items[SC132_FRAME_SET_MAX_CAMERAS];
  uint32_t reserved[8];
} sc132_frame_set_t;

/*
 * 明确 DMA 帧的只读访问、时间域和引用生命周期，防止 release 后继续保存裸指针。
 * timestamp_ns 单位为 ns，优先使用 sensor/VIO 随帧时间；缺失时退回系统出帧时间，两者不保证与墙上时钟同域。
 * frame_set 及 item 数组仅在回调期间有效；items[i].frame 是 borrowed reference。
 * 消费者不得直接对库持有的 callback reference 调用 sc132_frame_release；若要跨 callback
 * 保存 frame，必须在回调内先 sc132_frame_retain，并在最终使用完成后自行 sc132_frame_release。
 * user_data 必须保持有效直到 blocking sc132_stop 返回；仅 request_stop 返回不代表 callback 已退出。
 * C++ 消费者的 callback 必须 noexcept，并在内部捕获所有异常；异常不得跨越本 C ABI。
 * frame_info 中的 Y/UV 地址只读，且只在对应 frame 引用有效期间有效。
 */
typedef void (*sc132_frame_set_callback_t)(const sc132_frame_set_t *frame_set,
                                            void *user_data);

typedef struct sc132_frame_set_config {
  uint32_t struct_size;
  sc132_frame_set_callback_t callback;
  void *user_data;
  uint32_t camera_count;
  uint32_t width;
  uint32_t height;
  uint32_t timeout_ms;
  uint64_t max_skew_ns;
  uint32_t reserved[8];
} sc132_frame_set_config_t;

#define SC132_FRAME_SET_CONFIG_INIT \
  { sizeof(sc132_frame_set_config_t), NULL, NULL, 4U, SC132_NATIVE_OUTPUT_WIDTH, SC132_NATIVE_OUTPUT_HEIGHT, 100U, SC132_FRAME_SET_DEFAULT_MAX_SKEW_NS, {0U} }

int32_t sc132_set_fps(uint32_t fps);
int32_t sc132_set_output_rotation(uint32_t rotate_clockwise_degrees);
int32_t sc132_start_frame_set(const sc132_frame_set_config_t *config,
                              uint32_t camera_mask);
int32_t sc132_frame_retain(sc132_frame_t *frame);
void sc132_frame_release(sc132_frame_t *frame);
int32_t sc132_frame_get_info(const sc132_frame_t *frame,
                             sc132_frame_info_t *out_info);
/*
 * 两阶段关闭避免阻塞回调与 retained frame 相互等待。
 * request_stop 只线性化停止、禁止新采集/新 callback admission 并广播唤醒；它不 drain/release frame，
 * 不调用 vendor API，也不 join trigger、worker、dispatcher 或等待 callback，因而可从任意线程快速返回。
 * idle 状态调用 request_stop 也会锁存 STOPPING；必须再由非回调线程调用 blocking sc132_stop
 * 完成对应 stop generation，之后 start/config 才重新开放。
 * 普通 lifecycle owner 随后调用 blocking sc132_stop；它负责 drain pending/queued frame、等待 inflight
 * callback、join 库创建的线程并关闭 I2C。若 vendor worker 永久不退出，sc132_stop 允许持续阻塞，
 * 且在真实 quiescence 前生命周期保持 STOPPING，不会伪装成 STOPPED。
 * 极少数 internal pthread_join 失败时，sc132_stop 会保留 thread ownership、I2C 与 callback/config，
 * 释放 cleanup owner 但继续保持 STOPPING；外部非回调线程必须 retry sc132_stop，成功前禁止 start/unload。
 * sc132_start_frame_set 返回 STARTUP_FAILED 后必须由外部非回调线程调用 sc132_stop 完成 quiescence，
 * 才能卸载本库；在此之前后续 start/config 均返回 INVALID_STATE。库不会创建 detached cleanup guard。
 * stop 从 dispatcher 回调线程调用时只发布 request_stop 并立即返回，必须由外部非回调线程再次
 * 调用 stop 完成唯一 cleanup owner 的 drain/join，避免 dispatcher 确定性 self-deadlock。
 */
void sc132_request_stop(void);
void sc132_stop(void);

#ifdef __cplusplus
}
#endif

#endif
