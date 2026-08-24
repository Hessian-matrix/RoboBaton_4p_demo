# 相机标定文件回退 / Camera calibration file fallback

此目录属于 non-ROS demo 运行包的可选输入，只提供格式说明，不包含标定系数。
This directory is an optional input for the non-ROS demo runtime package and contains format guidance only, not calibration coefficients.

## 固定物理映射 / Fixed physical mapping

运行时使用以下固定地址，不扫描另一 EEPROM 地址，也不依据“唯一响应”猜测身份：
The runtime uses this fixed map. It does not scan the other EEPROM address or guess identity from a unique response:

| camera | device-tree VCON bus | sensor | EEPROM |
|---|---:|---:|---:|
| `cam0` | `4` | `0x33` | `0x52` |
| `cam1` | `4` | `0x32` | `0x53` |
| `cam2` | `6` | `0x33` | `0x52` |
| `cam3` | `6` | `0x32` | `0x53` |

`SC132_EEPROM_CANDIDATES` 只能收紧允许列表，不能改变这张物理映射表。
`SC132_EEPROM_CANDIDATES` may only restrict the allow-list; it cannot override this physical map.

## 文件格式 / File formats

- `camN.bin`：必须正好 256 字节，为 `RB4PCAL1` EEPROM 记录格式；记录中的 EEPROM 地址和 sensor 地址必须与 `camN` 的固定映射一致，并包含有效 CRC32。
  `camN.bin` must be exactly 256 bytes in the `RB4PCAL1` EEPROM record format. Its EEPROM and sensor identities must match the fixed map for `camN`, and its CRC32 must be valid.
- `camN.yaml`：必须包含 `camera_id: camN`、`resolution: [1280, 1088]` 和 `validation_status: PASS`。支持 `camera_model: ds`，或 `camera_model: pinhole` 与 `distortion_model: equidistant`。
  `camN.yaml` must contain `camera_id: camN`, `resolution: [1280, 1088]`, and `validation_status: PASS`. Supported models are `camera_model: ds`, or `camera_model: pinhole` with `distortion_model: equidistant`.
- `camN/selected_model.yaml` 只有在 `selection_status: UNDECIDED` 时有效，并且 `camN/models/ds.yaml` 与 `camN/models/kb4.yaml` 必须各自满足同样的 camera ID、分辨率和 `validation_status: PASS` 元数据。UNDECIDED 不选择模型，只保留两个候选。
  `camN/selected_model.yaml` is valid only with `selection_status: UNDECIDED`; both `camN/models/ds.yaml` and `camN/models/kb4.yaml` must independently satisfy the same camera ID, resolution, and `validation_status: PASS` metadata. UNDECIDED selects neither model and retains both candidates.

只有对应映射 EEPROM 明确 non-ACK 时才使用文件回退。EEPROM 数据无效、I/O/总线错误、文件无效或缺失均不绑定标定，并输出 `CAMERA_CALIBRATION_RESULT`。
A file fallback is used only when the mapped EEPROM explicitly non-ACKs. Invalid EEPROM data, I/O/bus errors, invalid files, and missing files never bind calibration and are reported as `CAMERA_CALIBRATION_RESULT`.

当前系数只描述 canonical `rotate=0` 的 `1280x1088` 像素；任何非零 rotation 都会在 device-tree/I2C 访问前返回 `INVALID`，不进行绑定。
The current coefficients describe canonical `rotate=0`, `1280x1088` pixels only. Any non-zero rotation returns `INVALID` before device-tree/I2C access and creates no binding.

默认目录为 `${DEMO_DIR}/config/camera_calibration`，可用 `SC132_CALIBRATION_DIR` 覆盖。`SC132_DEVICE_TREE_ROOT` 仅用于平台路径适配。
The default directory is `${DEMO_DIR}/config/camera_calibration`; override it with `SC132_CALIBRATION_DIR`. `SC132_DEVICE_TREE_ROOT` is available only to adapt the device-tree path.