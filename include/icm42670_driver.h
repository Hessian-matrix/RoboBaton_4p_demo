#ifndef ICM42670_X5_DRIVER_H
#define ICM42670_X5_DRIVER_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(ICM42670_X5_BUILDING_LIBRARY)
#define ICM42670_X5_API __declspec(dllexport)
#else
#define ICM42670_X5_API __declspec(dllimport)
#endif
#else
#define ICM42670_X5_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define ICM42670_ABI_VERSION_MAJOR 2U
#define ICM42670_ABI_VERSION_MINOR 1U

typedef struct icm42670_handle icm42670_handle_t;

typedef enum icm42670_status {
  ICM42670_STATUS_OK = 0,
  ICM42670_STATUS_INVALID_ARGUMENT = -1,
  ICM42670_STATUS_INVALID_STATE = -2,
  ICM42670_STATUS_IO_ERROR = -3,
  ICM42670_STATUS_INTERNAL_ERROR = -4
} icm42670_status_t;

typedef enum icm42670_read_mode {
  ICM42670_READ_MODE_SENSOR_TIMESTAMP_FIFO = 0
} icm42670_read_mode_t;

typedef enum icm42670_sample_drop_policy {
  ICM42670_SAMPLE_DROP_POLICY_ALLOW_COUNTED = 0,
  ICM42670_SAMPLE_DROP_POLICY_STRICT = 1
} icm42670_sample_drop_policy_t;

#define ICM42670_CONFIG_SAMPLE_DROP_POLICY_INDEX 0U
/* Set reserved[1] to 1 for rising-edge hardware FSYNC capture; default is off. */
#define ICM42670_CONFIG_FSYNC_ENABLE_INDEX 1U
#define ICM42670_HEALTH_FSYNC_CAPTURED_INDEX 0U
#define ICM42670_HEALTH_FSYNC_DROPPED_INDEX 1U

typedef struct icm42670_config {
  uint32_t struct_size;
  uint32_t sample_rate_hz;
  uint32_t fifo_watermark_samples;
  /* C ABI 以 uint32_t 固定 read_mode 存储宽度；当前仅接受 ICM42670_READ_MODE_SENSOR_TIMESTAMP_FIFO。 */
  uint32_t read_mode;
  uint32_t reserved[8];
} icm42670_config_t;

#define ICM42670_CONFIG_INIT \
  { sizeof(icm42670_config_t), 800U, 1U, ICM42670_READ_MODE_SENSOR_TIMESTAMP_FIFO, {0U} }

typedef struct icm42670_raw_sample {
  int16_t temperature;
  int16_t accel[3];
  int16_t gyro[3];
  int16_t reserved;
} icm42670_raw_sample_t;

typedef struct icm42670_runtime_health {
  uint32_t struct_size;
  uint32_t session_generation;
  uint64_t published_samples;
  uint32_t gpio_event_gap_count;
  uint32_t fifo_overflow_count;
  uint32_t mapper_failure_count;
  uint32_t uncertainty_over_200_drop_count;
  uint32_t max_consecutive_timing_drop_count;
  uint32_t invalid_packet_drop_count;
  uint32_t reserved[2];
} icm42670_runtime_health_t;

#define ICM42670_RUNTIME_HEALTH_INIT \
  { sizeof(icm42670_runtime_health_t), 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, {0U} }

typedef struct icm42670_sample {
  uint32_t struct_size;
  /* reserved0 reports observed FIFO timestamp low16 rollovers in this session. */
  uint32_t reserved0;
  /* Paired INT1 edge in CLOCK_REALTIME system ns; not the sample timestamp. */
  uint64_t host_timestamp_ns;
  double temperature_c;
  double accel_mps2[3];
  double gyro_rps[3];
  icm42670_raw_sample_t raw;
  /* FIFO TMST→RAW→CLOCK_REALTIME sample time; follows wall-clock steps, including backward. */
  uint64_t sample_timestamp_ns;
  uint64_t sample_sequence;
  uint32_t timestamp_uncertainty_us;
  uint32_t gpio_event_gap_count;
  uint32_t fifo_overflow_count;
  uint32_t mapper_failure_count;
} icm42670_sample_t;

/*
 * The callback is invoked serially by the acquisition thread; sample is borrowed for the callback.
 * set_callback may run concurrently; samples admitted after it returns use the new callback.
 * C++ callbacks must catch their own exceptions. stop/destroy waits for the acquisition thread and
 * therefore must not be called from the callback; user_data remains valid until stop returns.
 */
typedef void (*icm42670_sample_callback_t)(const icm42670_sample_t *sample,
                                           void *user_data);

/* Same admitted sample, delivered after its sample callback on the same worker.
 * RAW fields are for association only; public timestamps remain CLOCK_REALTIME.
 * uncertainty_ns bounds the reconstructed edge, including FIFO reconstruction.
 * Sequence counts every hardware tag, including events rejected by timing policy.
 * Lifetime, exception and stop rules are identical to the sample callback.
 */
typedef struct icm42670_fsync_event {
  uint32_t struct_size;
  uint32_t session_generation;
  uint64_t fsync_sequence;
  uint64_t sample_sequence;
  uint64_t sample_timestamp_ns;
  uint64_t fsync_timestamp_ns;
  uint64_t sample_raw_timestamp_ns;
  uint64_t fsync_raw_timestamp_ns;
  uint64_t fsync_to_sample_ns;
  uint64_t uncertainty_ns;
} icm42670_fsync_event_t;
typedef void (*icm42670_fsync_callback_t)(const icm42670_fsync_event_t *event,
                                         void *user_data);
ICM42670_X5_API int icm42670_set_fsync_callback(icm42670_handle_t *handle,
                                               icm42670_fsync_callback_t callback,
                                               void *user_data);

ICM42670_X5_API int icm42670_create(const icm42670_config_t *config,
                                    icm42670_handle_t **out_handle);
ICM42670_X5_API int icm42670_set_callback(icm42670_handle_t *handle,
                                          icm42670_sample_callback_t callback,
                                          void *user_data);
ICM42670_X5_API int icm42670_start(icm42670_handle_t *handle);
ICM42670_X5_API int icm42670_stop(icm42670_handle_t *handle);
ICM42670_X5_API int icm42670_is_running(const icm42670_handle_t *handle);
/* Valid only after a successful stop for the most recent start generation. */
ICM42670_X5_API int icm42670_get_runtime_health(
    const icm42670_handle_t *handle, icm42670_runtime_health_t *out_health);
ICM42670_X5_API void icm42670_destroy(icm42670_handle_t *handle);
/* Product release SemVer; returned storage is process-static and read-only. */
ICM42670_X5_API const char *icm42670_get_version(void);
/* The returned pointer refers to process-static read-only storage and must not be freed. */
ICM42670_X5_API const char *icm42670_status_message(int status);

#ifdef __cplusplus
}
#endif

#endif
