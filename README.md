# RoboBaton 4P non-ROS Demo

[English](README_EN.md) · [中文使用文档](https://4p-docs.readthedocs.io/zh-cn/latest/index.html)

面向 X5 的 SC132 四目相机、RTSP、ICM-42670-P IMU、UART 和 SocketCAN 示例仓库。接线、部署、参数、保存、API 和排障说明统一维护在公开文档仓库 [4P_doc](https://github.com/Hessian-matrix/4P_doc)。

## 仓库内容

- `demo/`：X5 完整运行包，含 `sensor_demo`、`cam_demo`、`mosaic_rtsp_demo`、`imu_reader_demo`、`serial_port_demo` 和 `can_demo`。
- `src/`、`include/`、`lib/`、`config/`：示例源码、公开头文件、匹配的预编译库与默认配置。
- `scripts/`：构建、打包与运行包校验入口。

当前 IMU 使用内核 IIO V4／公共 ABI 6.0：实体 `libicm42670.so.6.0.0`、SONAME `libicm42670.so.6`，九个导出均为 `ICM42670_X5_6.0`。`host` 保留 `CLOCK_MONOTONIC` 主机估计，`tmst` 另提供映射至 `CLOCK_MONOTONIC_RAW` 的 sensor TMST；两者均保留原生 host 时间及全部 burst 证据。保存输出按所选原生时钟每 burst 显式桥接到 realtime，保留桥括号半宽，不认证物理采样或曝光同步精度。设备须由匹配 V4 内核模块预先绑定；旧 V2/V3 即使 host 模式也不兼容。无用户态 SPI、触发—样本关联或自动回退。全部 consumer 必须与新头/库成套重建，不做跨 major 链接。软件版本、文档修订、ABI 与发布状态独立。 独立FSYNC诊断默认开启：YAML `imu.fsync_diagnostics: true`，`sensor_demo`／`imu_reader_demo --imu-fsync-diagnostics on|off`；ROS2启动只读参数`imu.fsync_diagnostics=true`。timing v3／MP4 IMU CSV原样保留`fifo_header,fsync_flags,fsync_sequence,fsync_raw_ns,fsync_uncertainty_ns,fsync_delta_us`；RAW沿诊断不校正正常样本／相机时间，不匹配帧。

本仓库只编译用户侧示例，不编译底层运行库。源码、头文件、库与配置须成套匹配；部署完整运行包并保留符号链接，不单独替换 `.so`。

## 开始使用

先按[首次上电](https://4p-docs.readthedocs.io/zh-cn/latest/getting-started/first-boot.html)和[部署、升级与回滚](https://4p-docs.readthedocs.io/zh-cn/latest/development/deployment-and-upgrade.html)准备设备及运行包，再在 X5 上执行：

```bash
cd /root/demo
./sensor_demo --version

./sensor_demo
```

按 `Ctrl+C` 正常停止。保持 `cam-service` 运行，不同时启动多个相机应用。其他示例及完整参数见 [non-ROS Demo 使用](https://4p-docs.readthedocs.io/zh-cn/latest/usage/non-ros-demo.html)。

最终默认 HO：相机 `observer` + IMU `host`，IMU 默认 `800Hz`。另外三种启动组合保留比较：YAML 顶层 `camera_timestamp_path: observer|official`、`imu_timestamp_mode: host|tmst`，CLI `--camera-timestamp-path`／`--imu-timestamp-mode` 可覆盖；修改后正常停止并重启，无热切换或失败回退。相机 observer 的 `500µs` 与 IMU tmst 的 `200µs` 是各自计算准入边界，official 物理误差未知。默认 YAML 保留 Web 开、RTSP 关、自动曝光开启；显式手动曝光配置与完整合同见 [non-ROS 使用](https://4p-docs.readthedocs.io/zh-cn/latest/usage/non-ros-demo.html)。

运行包也支持 `./start_sensor_demo.sh stop` 正常停止、`./start_sensor_demo.sh restart` 按包内 YAML 配置后台重启，以及 `./start_sensor_demo.sh status` 查看本包进程与自启状态。停止会等待录制和 Web 子进程收尾，超时失败不强杀、不启动第二份程序；默认等待上限 1800 秒，可用 `SENSOR_DEMO_STOP_TIMEOUT_S` 调整。后台日志默认写入 `/userdata/sensor_demo_autostart.log`；重启成功只表示前 2 秒未退出，仍须检查日志和页面。启停管理需要 Linux pidfd 和 Python ≥3.10；不改变开机自启注册，也不操作 `cam-service`。完整用法见 [启停与开机自启动](https://4p-docs.readthedocs.io/zh-cn/latest/usage/non-ros-demo.html#non-ros-autostart)。

### 局域网视频与六轴预览

当前源码的 `sensor_demo` 默认开启 Web、关闭 RTSP。同一可信局域网电脑打开 `http://<设备IP>/`，查看四路 H.264 原生视频、IMU 六轴数值及趋势；页面资源离线内嵌，无需插件。`sensor_web` 由应用启动和回收，不要单独运行。此功能尚未作为新软件版本发布，不代表全部浏览器、双机稳态或 GPU 已验收。

右上角语言按钮用 `CH` / `EN` 表示切换目标：中文页面显示 `EN`，英文页面显示 `CH`；默认中文，浏览器允许本地存储时记住选择。切换覆盖静态文案、动态状态与无障碍提示，不刷新页面、不重建视频/SSE/姿态 Worker，不重置录制选项、设备录制或已接受的零偏。“在线文档 / Online Docs”按当前语言在新标签页打开对应官方文档；只有在线文档需要互联网，设备页面与切换仍使用离线内嵌资源。

所有功能集中在**同一首页**，无左侧页面导航或独立 hash 视图。主工作区包含四路相机、惯性姿态、六轴原始遥测和常驻的“录制与保存”四张卡片。宽度 ≥1440px 时四列并排，相机列最大、遥测列加宽至略宽于姿态列，从相机列两侧空白让出空间，四张外框等高、上下边缘对齐；1100–1439px 时相机/姿态/遥测三列并排，录制卡片在下方独占一行；768–1099px 时相机与录制各占整行，姿态和遥测左右并排；小于 768px 时全部上下堆叠。姿态校准与相对航向重置仍在姿态卡片内。

浏览器视口宽度 ≥1440px 且高度 ≥680px（CSS 像素）时，桌面使用紧凑标题和四项健康摘要，四张卡片等高铺满主工作区，模型与趋势图随可用高度扩展；溢出内容在各面板内滚动。视频网格在相机列内居中。低于任一尺寸门槛的窄屏、短屏与移动端使用正常文档滚动。

六轴趋势固定显示最近 1 秒，无时间窗下拉选项。加速度和角速度分别成图，每张图的 X/Y/Z 共用时间轴与纵坐标；纵轴上下限随当前窗口的三轴数据统一自动调整，并标注数值刻度，单位为标题中的 m/s²、rad/s，横轴为距浏览器当前时刻的秒数。不做均值平滑或扣除重力/零偏，超过 250ms 的接收缺口断线显示；仅改变预览，不改变采样、保存或姿态。桌面面板空间不足时内部滚动。
遥测标题下不再显示趋势说明行；图表收紧面板左侧空白，按实际刻度文字宽度预留纵坐标边距，避免固定宽空槽，同时保留完整刻度。


主工作区下方的“运行诊断”可在首页内展开，查看数据源状态及每路相机实际报告的解码器、帧率（fps）和丢包诊断文本；缺失字段仍为 `unknown`。

相机卡片只显示小字号单行“相机”标题，与分辨率/帧率/编码同排对齐，无 `CAMERAS / 01` 眉标题。四路画面取消独立标题栏与放大按钮，CAM 编号叠放在图像左上角，播放状态在右下角。双击图像放大/还原，键盘聚焦图像后可按 Enter 或空格切换；`Esc` 仅退出单路放大，不用于录制控制。网格与放大均按源比例以 `contain` 完整显示，不裁剪、不拉伸，源画面自带黑边保留；紧凑桌面按网格剩余宽高尽量放大图像。

界面相机名称为 **CAM1～CAM4**，分别对应内部 `camera_id` 0～3；API、录制文件名和数据索引不变。紧凑桌面将四路显示框按源比例放大，并缩小相机卡片左右内边距；四张外框继续等高。相机卡片底部不再显示说明文字。浏览器并排播放仍不用于四目显示同步或 FSYNC 精度验收。

展开或折叠录制详情、高级选项和诊断均不导航或重新载入文档，不重建 WebRTC 连接、SSE 订阅或姿态 Worker，不重置当前选项或浏览器会话中已接受的陀螺零偏。**刷新浏览器仍会将本地选项恢复为页面默认值并清除校准**，这些状态不会持久化。姿态卡片提供校准与相对航向重置，它们只影响浏览器姿态预览；布局不新增 HTTP API、身份认证或硬件控制。

录制“已写入”使用十进制 MB，达到 1 GB 后自动切换为 GB，保留两位小数（1 MB = 1,000,000 B，1 GB = 1,000,000,000 B）；只转换显示，设备字节计数和 API 不变。

```bash
./sensor_demo --no-web --rtsp   # 仅传统 RTSP
./sensor_demo --web --rtsp      # 两种输出共用每路一次编码
./sensor_demo --no-web --no-rtsp # 关闭网络输出，保存仍独立生效
```

YAML 中 `web.enabled`、`rtsp.enabled` 独立；`rtsp.bps/codec` 仍是共享编码参数。Web 视频仅支持 H.264；H.265 不转码，IMU 遥测仍可用。HTTP 使用 TCP 80，WebRTC 使用 UDP/TCP 8189；只放通 HTTP 不够。端口冲突使 Web 降级，不抢占其他服务、不影响采集/保存。

Web 可先于 USB/Wi-Fi 网络就绪启动；网络可达后，使用该连接实际到达的设备 IPv4 地址访问，无需为新增地址重启应用。HTTP Host 必须与连接的本地地址一致，显式端口须匹配配置；不支持域名、反向代理 Host 或用另一网卡地址作为 Host。同源限制不变，新的 WebRTC 会话按当时可用的网卡地址收集候选；已有会话断网后由页面重连。

网页没有身份认证，只用于可信隔离 LAN；HTTP 信令/遥测没有传输真实性保护，不要直接暴露到公网或不可信网络。硬解由浏览器、系统和驱动决定，缺少诊断字段显示 `unknown`，有 GPU 不等于四路硬解。页面并排播放不证明四路显示同步或相机/IMU 同步。

首页常驻的“录制与保存”卡片只有一份表单，默认显示格式、固定存储位置、开始/停止/查询、设备状态、设备计时时长和已写入字节数。默认折叠的原生“高级选项”详情包含图像隔帧与最小丢组间隔；另一默认折叠的“录制详情与使用说明”包含可用空间、丢弃计数、最终结果、会话 ID、实际格式/存储、输出路径、完整性/清理、健康范围及使用边界。实际错误、命令结果不确定和停止排空/封装提示始终位于折叠区外。隐藏的高级值无效时，先自动展开高级选项，再显示浏览器校验提示。沿用原有校验、实时状态/能力检查、命令锁定及设备确认/查询；状态不可用或过期、命令待确认或结果不确定、设备不允许开始时不能开始录制。

在录制卡片中选择格式和固定存储后开始；需要调整隔帧或最小丢组间隔时展开“高级选项”。点击“停止录制”后等待设备排空/封装及最终结果；只有 `published_complete`、`data_complete=true` 和 `cleanup_complete=true` 才表示完整发布。相机、原生 IMU 和预览继续运行，可连续多次录制。网页关闭/断网不停止录制；重新打开页面查询真实状态，命令超时后不盲目重发。运行目录为实际 ELF 目录下的 `recordings`（标准包 `/root/demo/bin/recordings`），TF 为已挂载的 `/mnt/sdcard/recordings`，不接受任意路径。bag/MP4 互斥，MP4 要求四路 H.264。存储、API 与原生数据合同不变；启动参数及 YAML 自动保存保留，也可网页停止，保存不使用网页降频/姿态滤波数据，动态 MP4 健康仅覆盖 `callback_window`。页面无认证，能够访问设备的人可开始/停止写盘。完整合同见[数据保存](https://4p-docs.readthedocs.io/zh-cn/latest/usage/save-data-guide.html)。

页面另提供客户端姿态预览：浏览器 Web Worker 用 ≤50Hz 的姿态专用六轴输入融合四元数，动画板体显示 Roll/Pitch/相对 Yaw；设备端不计算姿态或零偏。设备仅在降频前对每个原生样本执行四级一阶低通，单级极点为 `min(10Hz, 遥测上限×0.2, 原生采样率×0.2)`；默认 800→50Hz 时低频群延迟约 64ms。SSE 新增 `attitude_accel_mps2` / `attitude_gyro_rps`，原始六轴数值/趋势字段、原生采样、保存与公共 ABI 不变。世界 +Z 向上，按当前重力初始化；原始和滤波加速度均在重力带内才修正倾角，带外仅陀螺积分。

首次打开页面或进入新数据源会话后，让**实际设备完全静止**。Worker 会在约 **3 秒**低运动、重力一致的连续遥测窗口后自动初始化三轴陀螺零偏，无需先按按钮；检查最小样本数、连续时间戳、重力/角速度幅值、加速度与陀螺方差，以及窗口前后段的加速度与陀螺均值变化。检测到运动、强振动、数据稀疏或时间缺口会重置候选窗口，检测到运动期间不学习。

姿态积分与零偏均值使用降频前滤波后的输入；静置质量检查仍使用原始六轴，不能用滤波掩盖强振动。时间不递增、序号重置或超过 100ms 原生样本缺口会重新播种滤波器，无效输入使姿态字段不可用；不外推缺失样本。

已有零偏后，只有完整约 3 秒静置窗口的**平均残余角速度向量模长 ≤0.002 rad/s（约 0.11°/s）**时，才保守地自动更新。更新用 **6 秒时间常数**的指数跟踪缓慢跟随零偏变化；接受自动估计不重置航向、不使姿态跳变。状态区提示自动静置估计的等待/采集进度及已估计/跟踪状态；这些状态表示检查结果，不是设备确实静止或绝对航向准确的证明。

已知零偏后的自动跟踪还检查原始角速度在约 1 秒子窗口及末尾未满窗口的平均残余，避免降频前低通的延迟掩盖末尾刚开始的转动；噪声过大时延后更新，而非冻结航向。无效六轴、姿态输入或时间字段会丢弃未完成的自动窗口并中断手动校准，同一会话已接受的零偏保留。

“静置校准零偏”按钮仍可手动重估：先让设备完全静止，点击后继续保持约 **3 秒**，等待页面报告结果。运动、不稳定、振动或数据稀疏可使校准被拒绝，失败不覆盖此前已接受的零偏；请静置并等待遥测稳定后重试。温度变化后可先静置等待自动跟踪，或显式重新校准。

所有四元数积分都先扣除已接受的三轴零偏；**没有 Yaw 静止冻结、硬角速度死区或仅作用于显示的阻尼**。“重置相对航向”重新对齐当前重力而非把倾斜归零，在同一应用会话内保留已接受的零偏。超过 500ms 无有效姿态时冻结模型；重连或时间戳不递增/间隔超过 250ms 后重新对齐。时间不连续会取消尚未完成的候选窗口/手动校准，同一数据源会话内保留已接受的零偏；新数据源会话或刷新页面会清除零偏，之后静置约 3 秒可自动重新估计。

六轴数据无法完全区分零偏与匀速慢转：自动跟踪可能吸收低于上述门限的缓慢残余转动，首次估计或手动校准同样必须确保设备真实静止，不能声称完美运动检测或任意慢转都能保留。校准后 ≥0.005 rad/s（约 0.29°/s）的真实转动仍应可测，不应被当作零偏消除。没有外部航向参考，Yaw 仅为相对航向，不可观测且仍会漂移；不提供绝对或无漂移航向，不用于导航，也不代替完整传感器标定。运动加速度、振动、剩余偏置与遥测抽取均影响预览精度。

部署此更新须重新构建 `sensor_demo`、包含内嵌页面/Worker 的 `sensor_web` helper 和完整运行包，成套替换后正常重启应用；仅刷新浏览器不会更新正在运行的内嵌代码。这些说明不代表已完成板端部署或验收。

这里的部署仅指经授权在用户设备上更新本地运行包，不是新软件版本发布，也不扩大现有验收范围。

H.264 producer 显式关闭与 Main 声明冲突的 8×8 transform，修复部分 Windows 硬解端四路彩色竖条。更新时必须整包替换并重启应用；只刷新网页不会更新已运行的编码器。解码帧数增长、零丢包也不能证明像素正确，需检查实际画面。此修复不要求关闭浏览器硬件加速，也不等于全部 GPU 兼容性验收。

源码构建额外需要固定 Go `1.27.3`，预先配置 module 缓存，再设置 `SENSOR_WEB_GO_EXECUTABLE`；构建不自动下载或切换工具链。完整运行包内已包含 ARM64 `bin/sensor_web` 和第三方许可证，板端无需 Go。

### 标定驱动的 360° 环视

新增独立 `panorama_demo`，不是 `mosaic_rtsp_demo` 的四格拼图，也不接入 `sensor_demo` 首页。先正常停止其他相机应用，保持 `cam-service` 运行，再在新构建的完整运行包中执行：

```bash
./panorama_demo --calibration config/calibration.yaml --port 8080
```

同一可信局域网浏览器打开 `http://<设备IP>:8080/`，查看单张等距柱状环视图、覆盖率、处理耗时与丢组计数；`Ctrl+C` 退出。HTTP 无认证、无加密，禁止暴露公网。程序不采集 IMU、不录制，也不自动启停其他应用。

配置要求 schema 7、`kannala-brandt4` 内参与 `T_camN_from_imu` 外参（wxyz、米），cam0..3 必须对应标定时的同一固定安装和像素方向；示例配置标记 `pending_human_review`，不可作为任意设备的标定。默认输出 1600×600、方位角 360°、仰角 ±60°，中心为 IMU +X，正方位角朝 +Y、正仰角朝 +Z，不做重力调平。`--yaw` 改变中心方位角；`--distance-m 0` 用无穷远旋转模型，正值 ≥0.1m 使用 IMU 原点球面及平移外参。每帧从标定重叠区域稳健估计相机间的亮度比例，对具有可靠重叠的相机求解共同曝光增益，范围为 0.5–2 倍；暗部、饱和及明显色彩不一致的样本不参与，无可靠证据的相机保持原亮度。主体像素按光轴与源边缘质量选择一路；在选路边界附近，按局部颜色差、结构边缘和投影质量搜索连续接缝，优先避开不一致的前景边缘，并对上一处理帧的接缝位置施加软约束。接缝处对平滑的低频照明差做较宽的渐变补偿，在有效重叠和归属边缘淡出；高频细节仍取单路，仅在接缝两侧各 4 个输出像素内对补偿后接近的颜色窄幅融合，避免大范围混合造成前景重影。360° 首尾使用同样的环绕处理。RGB 与 NV12 共用亮度补偿及选路逻辑，不做锐化、图像模糊或光流形变，不改变标定采样坐标。亮度补偿不是相机曝光控制，也不能恢复过曝或欠曝细节；无可靠共同内容、明显色差和强视差时仍可能残留接缝，运动场景不保证无闪跳。近物仍可能在接缝处错位或断裂；盲区保持黑色。`100%` 地平线几何覆盖不代表全天球、有效像素或标定精度验收。

采集固定四路 30fps，只处理最新完整帧组，默认处理上限 `--max-fps 10`，慢处理主动丢组，不承诺板端帧率。拼接最多复用 3 个后台计算线程加调用线程，互斥像素区域并行处理；接缝滤波按连续行访问，标定质量项预计算，不降低输出分辨率。`/frame.jpg` 和 `/status.json` 为只读最新快照；页面请求和显示不是物理同步验收。

主机离线构建仅需 C++17、CMake、LibYAML/JPEG 开发头和静态库，不需要 X5 或 Go：

```bash
cmake -S . -B build_panorama_host -DPANORAMA_OFFLINE_ONLY=ON
cmake --build build_panorama_host -j
./build_panorama_host/panorama_demo --calibration config/calibration.yaml \
  --images /path/to/group --once --output /path/to/new-panorama.jpg
```

输入目录包含同一帧组的 `cam0.jpg`～`cam3.jpg`，必须与标定尺寸/方向一致，程序不按文件名验证同步。去掉 `--once` 可通过同一 Web 页面查看离线静态图；`--output` 拒绝覆盖已有路径。X5 完整构建同样需要目标 LibYAML/JPEG 开发头及静态库，标准打包脚本包含新入口、标定和许可证；板端无需安装这两个库。算法基于标定投影、亮度补偿与接缝融合，未实现深度重建或自动重标定。JPEG 部分基于 Independent JPEG Group；第三方许可见 `licenses/panorama/THIRD_PARTY_NOTICES.txt`。


## 文档入口

- [源码编译与运行包校验](https://4p-docs.readthedocs.io/zh-cn/latest/development/open-source-build.html)
- [数据保存](https://4p-docs.readthedocs.io/zh-cn/latest/usage/save-data-guide.html)
- [数据合同](https://4p-docs.readthedocs.io/zh-cn/latest/development/data-contracts.html) · [API 参考](https://4p-docs.readthedocs.io/zh-cn/latest/development/api-reference.html)
- [硬件连接与安全](https://4p-docs.readthedocs.io/zh-cn/latest/getting-started/hardware-and-safety.html)
- [故障排查](https://4p-docs.readthedocs.io/zh-cn/latest/troubleshooting.html)
- [版本、已知限制与支持](https://4p-docs.readthedocs.io/zh-cn/latest/release-and-support.html)

许可证及第三方组件说明见本仓库 [LICENSE](LICENSE) 和公开发布说明。
