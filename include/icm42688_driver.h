#ifndef ICM42688_X5_DRIVER_H
#define ICM42688_X5_DRIVER_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(ICM42688_X5_BUILDING_LIBRARY)
#define ICM42688_X5_API __declspec(dllexport)
#else
#define ICM42688_X5_API __declspec(dllimport)
#endif
#else
#define ICM42688_X5_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ICM42688_ABI_VERSION_MAJOR 2U
#define ICM42688_ABI_VERSION_MINOR 0U

typedef struct icm42688_handle icm42688_handle_t;

typedef enum icm42688_status {
  ICM42688_STATUS_OK = 0,
  ICM42688_STATUS_INVALID_ARGUMENT = -1,
  ICM42688_STATUS_INVALID_STATE = -2,
  ICM42688_STATUS_IO_ERROR = -3,
  ICM42688_STATUS_INTERNAL_ERROR = -4
} icm42688_status_t;

typedef enum icm42688_read_mode {
  ICM42688_READ_MODE_FIFO = 0,
  ICM42688_READ_MODE_DIRECT = 1
} icm42688_read_mode_t;

typedef struct icm42688_config {
  uint32_t struct_size;
  uint32_t sample_rate_hz;
  uint32_t fifo_watermark_samples;
  /* 固定为 uint32_t；取值使用 icm42688_read_mode_t 常量，禁止 enum 编译选项改变结构布局。 */
  uint32_t read_mode;
  uint32_t reserved[8];
} icm42688_config_t;

#define ICM42688_CONFIG_INIT \
  { sizeof(icm42688_config_t), 1000U, 8U, ICM42688_READ_MODE_FIFO, {0U} }

typedef struct icm42688_raw_sample {
  int16_t temperature;
  int16_t accel[3];
  int16_t gyro[3];
  int16_t reserved;
} icm42688_raw_sample_t;

typedef struct icm42688_sample {
  uint32_t struct_size;
  uint32_t reserved0;
  /* Direct INT1 mode uses CLOCK_MONOTONIC_RAW; FIFO mode uses the same host clock domain. */
  uint64_t host_timestamp_ns;
  double temperature_c;
  double accel_mps2[3];
  double gyro_rps[3];
  icm42688_raw_sample_t raw;
  uint32_t reserved[8];
} icm42688_sample_t;

/*
 * The callback is invoked serially by the acquisition thread; sample is borrowed for the callback.
 * set_callback may run concurrently; samples admitted after it returns use the new callback.
 * C++ callbacks must catch their own exceptions. stop/destroy waits for the acquisition thread and
 * therefore must not be called from the callback; user_data remains valid until stop returns.
 */
typedef void (*icm42688_sample_callback_t)(const icm42688_sample_t *sample,
                                           void *user_data);

ICM42688_X5_API int icm42688_create(const icm42688_config_t *config,
                                    icm42688_handle_t **out_handle);
ICM42688_X5_API int icm42688_set_callback(icm42688_handle_t *handle,
                                          icm42688_sample_callback_t callback,
                                          void *user_data);
ICM42688_X5_API int icm42688_start(icm42688_handle_t *handle);
ICM42688_X5_API int icm42688_stop(icm42688_handle_t *handle);
ICM42688_X5_API int icm42688_is_running(const icm42688_handle_t *handle);
ICM42688_X5_API void icm42688_destroy(icm42688_handle_t *handle);
/* The returned pointer refers to process-static read-only storage and must not be freed. */
ICM42688_X5_API const char *icm42688_status_message(int status);

#ifdef __cplusplus
}
#endif

#endif
