#ifndef ROBOBATON_CAMERA_CALIBRATION_H_
#define ROBOBATON_CAMERA_CALIBRATION_H_

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(CAMERA_CALIBRATION_BUILDING_LIBRARY)
#define CAMERA_CALIBRATION_API __declspec(dllexport)
#else
#define CAMERA_CALIBRATION_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define CAMERA_CALIBRATION_API __attribute__((visibility("default")))
#else
#define CAMERA_CALIBRATION_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_CALIBRATION_ABI_VERSION_MAJOR 1U
#define CAMERA_CALIBRATION_ABI_VERSION_MINOR 0U
#define CAMERA_CALIBRATION_CAMERA_COUNT 4U
#define CAMERA_CALIBRATION_IMAGE_WIDTH 1280U
#define CAMERA_CALIBRATION_IMAGE_HEIGHT 1088U
#define CAMERA_CALIBRATION_RECORD_SIZE 256U
#define CAMERA_CALIBRATION_MAX_TEXT 128U

#define CAMERA_CALIBRATION_OK 0
#define CAMERA_CALIBRATION_E_INVALID_ARGUMENT (-1)
#define CAMERA_CALIBRATION_E_UNSUPPORTED (-2)
#define CAMERA_CALIBRATION_E_INTERNAL (-3)

#define CAMERA_CALIBRATION_SOURCE_NONE 0U
#define CAMERA_CALIBRATION_SOURCE_EEPROM 1U
#define CAMERA_CALIBRATION_SOURCE_FILE 2U

#define CAMERA_CALIBRATION_STATUS_PASS 0U
#define CAMERA_CALIBRATION_STATUS_ABSENT 1U
#define CAMERA_CALIBRATION_STATUS_INVALID 2U
#define CAMERA_CALIBRATION_STATUS_IO_ERROR 3U
#define CAMERA_CALIBRATION_STATUS_MISSING 4U

#define CAMERA_CALIBRATION_MODEL_DOUBLE_SPHERE 0x01U
#define CAMERA_CALIBRATION_MODEL_KB4 0x02U

typedef struct camera_calibration_request_v1 {
  uint32_t struct_size;
  uint32_t camera_id;
  const char *device_tree_root;
  const char *fallback_directory;
  int32_t rotate_degrees;
  uint32_t reserved[7];
} camera_calibration_request_v1;

typedef struct camera_calibration_binding_v1 {
  uint32_t struct_size;
  uint32_t status;
  uint32_t source;
  uint32_t camera_id;
  uint32_t bus;
  uint32_t sensor_i2c_addr;
  uint32_t eeprom_i2c_addr;
  uint32_t image_width;
  uint32_t image_height;
  uint32_t model_mask;
  double double_sphere[6];
  double kb4[8];
  char endpoint[CAMERA_CALIBRATION_MAX_TEXT];
  char path[CAMERA_CALIBRATION_MAX_TEXT];
  char error[CAMERA_CALIBRATION_MAX_TEXT];
  uint32_t reserved[16];
} camera_calibration_binding_v1;

CAMERA_CALIBRATION_API const char *camera_calibration_get_version(void);
CAMERA_CALIBRATION_API int32_t camera_calibration_discover(
    const camera_calibration_request_v1 *request,
    camera_calibration_binding_v1 *result);

#ifdef __cplusplus
}
#endif

#endif  /* ROBOBATON_CAMERA_CALIBRATION_H_ */
