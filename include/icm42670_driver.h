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

#define ICM42670_ABI_VERSION_MAJOR 5U
#define ICM42670_ABI_VERSION_MINOR 0U

typedef struct icm42670_handle icm42670_handle_t;
typedef enum icm42670_status {
  ICM42670_STATUS_OK = 0,
  ICM42670_STATUS_INVALID_ARGUMENT = -1,
  ICM42670_STATUS_INVALID_STATE = -2,
  ICM42670_STATUS_IO_ERROR = -3,
  ICM42670_STATUS_INTERNAL_ERROR = -4
} icm42670_status_t;
typedef enum icm42670_sample_drop_policy {
  ICM42670_SAMPLE_DROP_POLICY_ALLOW_COUNTED = 0,
  ICM42670_SAMPLE_DROP_POLICY_STRICT = 1
} icm42670_sample_drop_policy_t;

#define ICM42670_TIMESTAMP_MODE_HOST 0U
#define ICM42670_TIMESTAMP_MODE_TMST 1U

/* Native paired six-axis ODR; buffer capacity is a power of two in [256,8192].
 * The kernel owns SPI/INT1. No userspace SPI backend or module loading occurs. */
typedef struct icm42670_config {
  uint32_t struct_size;
  uint32_t sample_rate_hz;
  uint32_t fifo_watermark_samples;
  uint32_t buffer_capacity;
  uint32_t sample_drop_policy;
  uint32_t timestamp_mode;
  uint32_t reserved[2];
} icm42670_config_t;
#define ICM42670_CONFIG_INIT \
  { sizeof(icm42670_config_t), 800U, 1U, 4096U, ICM42670_SAMPLE_DROP_POLICY_ALLOW_COUNTED, ICM42670_TIMESTAMP_MODE_HOST, {0U} }

typedef struct icm42670_raw_sample {
  int16_t temperature;
  int16_t accel[3];
  int16_t gyro[3];
  int16_t reserved;
} icm42670_raw_sample_t;

#define ICM42670_TIMESTAMP_SOURCE_IRQ 1U
#define ICM42670_TIMESTAMP_SOURCE_SERVICE 2U
#define ICM42670_PACKET_ACCEL_VALID 1U
#define ICM42670_PACKET_GYRO_VALID 2U
#define ICM42670_PACKET_TEMP_VALID 4U
#define ICM42670_BURST_OK 1U
#define ICM42670_BURST_STOP_TAIL 2U

/* sample_monotonic_ns always retains the kernel CLOCK_MONOTONIC host estimate.
 * TMST mode additionally maps FIFO low16 through bounded UI transition anchors
 * to sample_raw_ns (CLOCK_MONOTONIC_RAW). Host mode leaves sample_raw_ns zero
 * and timestamp_uncertainty_ns UINT32_MAX. Neither path claims UTC precision. */
typedef struct icm42670_sample {
  uint32_t struct_size;
  uint32_t session_generation;
  uint64_t sample_monotonic_ns;
  uint64_t sample_sequence;
  uint32_t burst_flags;
  uint32_t fifo_tmst;
  uint64_t evidence_id;
  uint64_t burst_complete_raw_ns;
  uint64_t observation_monotonic_ns;
  uint64_t estimated_period_ns;
  uint32_t timestamp_source;
  uint32_t packet_flags;
  double temperature_c;
  double accel_mps2[3];
  double gyro_rps[3];
  icm42670_raw_sample_t raw;
  uint64_t sample_raw_ns;
  uint32_t timestamp_uncertainty_ns;
  uint32_t timestamp_mode;
} icm42670_sample_t;

/* A terminal diagnostic is queryable after stop even if stop reports failure.
 * transport_complete certifies consumed/ACKed STOP cohort only. A nonzero
 * fatal or invalid payload still prevents a strict successful session.
 * unknown_hardware_tail excludes physical packets after the STOP fence. */
typedef struct icm42670_runtime_health {
  uint32_t struct_size;
  uint32_t session_generation;
  uint64_t published_samples;
  uint64_t consumed_scans;
  uint64_t records_consumed;
  uint64_t records_acked;
  uint64_t last_packet_sequence;
  uint64_t irq_wakeup_count;
  uint64_t timer_service_count;
  uint32_t fatal_reason;
  uint32_t fatal_errno;
  uint32_t fifo_overflow_count;
  uint32_t invalid_payload_count;
  uint32_t scan_push_failure_count;
  uint32_t evidence_full_count;
  uint32_t timestamp_service_burst_count;
  uint32_t tail_packet_count;
  uint32_t unknown_hardware_tail;
  uint32_t quiesce_errno;
  uint32_t transport_complete;
  uint32_t reserved;
} icm42670_runtime_health_t;
#define ICM42670_RUNTIME_HEALTH_INIT \
  { sizeof(icm42670_runtime_health_t), 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, \
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U }

/* Serial worker callback; the sample is borrowed until return. START requires
 * a callback. Registration is immutable through capture and STOP drain until
 * the worker is joined; set_callback returns INVALID_STATE during that time,
 * including from a callback. Callback must not throw or call stop/destroy.
 * user_data must stay alive until stop returns. */
typedef void (*icm42670_sample_callback_t)(const icm42670_sample_t*, void*);
ICM42670_X5_API int icm42670_create(const icm42670_config_t*, icm42670_handle_t**);
ICM42670_X5_API int icm42670_set_callback(icm42670_handle_t*, icm42670_sample_callback_t, void*);
ICM42670_X5_API int icm42670_start(icm42670_handle_t*);
ICM42670_X5_API int icm42670_stop(icm42670_handle_t*);
ICM42670_X5_API int icm42670_is_running(const icm42670_handle_t*);
ICM42670_X5_API int icm42670_get_runtime_health(const icm42670_handle_t*, icm42670_runtime_health_t*);
ICM42670_X5_API void icm42670_destroy(icm42670_handle_t*);
/* Process-static read-only product SemVer, independent of ABI. */
ICM42670_X5_API const char* icm42670_get_version(void);
ICM42670_X5_API const char* icm42670_status_message(int);

#ifdef __cplusplus
}
#endif
#endif
