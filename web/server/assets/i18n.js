'use strict';

const i18n = (() => {
  const english = {
    'RoboBaton 4P · 设备总览': 'RoboBaton 4P · Device Overview',
    '跳至内容': 'Skip to content',
    '传感器工作台': 'Sensor Workspace',
    '连接设备中': 'Connecting to device',
    '设备总览': 'Device Overview',
    '四路影像 · 惯性遥测 · 设备录制': 'Four cameras · IMU telemetry · Device recording',
    '实时设备摘要': 'Live device summary',
    '相机连接': 'Camera connections',
    '等待连接': 'Waiting for connection',
    '独立 WebRTC 会话': 'Independent WebRTC sessions',
    '源采集': 'Source capture',
    '等待状态': 'Waiting for status',
    '不代表显示同步': 'Does not imply synchronized display',
    'IMU 数据': 'IMU data',
    '等待样本': 'Waiting for samples',
    '原生 / 预览': 'Native / preview',
    '传感器温度': 'Sensor temperature',
    '温变后建议重校准': 'Recalibrate after temperature changes',
    '相机': 'Cameras',
    '惯性姿态': 'Inertial Attitude',
    '等待有效重力样本': 'Waiting for valid gravity samples',
    'IMU 设备坐标系姿态模型': 'IMU device-frame attitude model',
    '简化板体随六轴融合姿态旋转，标记设备正 X、Y、Z 轴。角度数值见下方。': 'A simplified board rotates with the six-axis fused attitude, marking the positive device X, Y, and Z axes. Angles are shown below.',
    '设备坐标系 · 世界 +Z 向上': 'Device frame · World +Z points upward',
    '横滚 Roll · X': 'Roll · X',
    '俯仰 Pitch · Y': 'Pitch · Y',
    '相对 Yaw · Z': 'Relative Yaw · Z',
    '静置校准零偏': 'Calibrate Gyro Bias',
    '重置相对航向': 'Reset Relative Heading',
    '等待实时样本': 'Waiting for live samples',
    '静置约 3 秒自动估计零偏。': 'Keep still for about 3 seconds to estimate gyro bias automatically.',
    '姿态与校准边界': 'Attitude and calibration limits',
    '静置约 3 秒后自动估计零偏，之后仅在低运动窗口保守跟踪；也可手动重校。极慢匀速转动可能被误认为零偏。重置保留零偏，刷新后重新估计。': 'Bias is estimated after about 3 seconds at rest, then conservatively tracked only during low-motion windows; manual recalibration is also available. Very slow constant rotation may be mistaken for bias. Reset preserves bias; reloading estimates it again.',
    '六轴融合由浏览器 Worker 计算。无磁力计，Yaw 无绝对北向且仍会漂移；重力辅助横滚/俯仰。自动静置判断不是运动真值，初始匀速转动或校准后低于约 0.11°/秒的转动仍可能被当作零偏。没有航向锁定或角速度死区。重置按当前重力对齐，不将倾斜归零。': 'Six-axis fusion runs in a browser Worker. Without a magnetometer, yaw has no absolute north reference and still drifts; gravity assists roll/pitch. Automatic rest detection is not motion ground truth: initial constant rotation or rotation below about 0.11°/s after calibration may be treated as bias. There is no heading lock or angular-rate dead zone. Reset aligns to current gravity without zeroing tilt.',
    '模型仅为坐标示意，不是产品外形。运动加速度、振动与 ≤50 Hz 抽取影响精度，不用于导航或标定。断流冻结，重连或时间戳跳变后重新对齐；新采集会话清除零偏并等待重新静置。': 'The model illustrates coordinates, not the product shape. Motion acceleration, vibration, and ≤50 Hz decimation affect accuracy; do not use it for navigation or calibration. It freezes on stream loss and realigns after reconnection or timestamp jumps. A new capture session clears bias and waits for rest again.',
    '六轴原始遥测': 'Raw Six-Axis Telemetry',
    '等待数据': 'Waiting for data',
    '加速度': 'Acceleration',
    '角速度': 'Angular velocity',
    '最近一秒加速度 X、Y、Z 共用坐标实时趋势，纵轴自动缩放': 'Last-second acceleration X, Y, Z traces on shared axes with automatic vertical scaling',
    '最近一秒角速度 X、Y、Z 共用坐标实时趋势，纵轴自动缩放': 'Last-second angular velocity X, Y, Z traces on shared axes with automatic vertical scaling',
    '等待最新样本': 'Waiting for the latest sample',
    '录制与保存': 'Recording and Saving',
    '等待实时状态 · 控制已禁用': 'Waiting for live status · Controls disabled',
    '启动选项': 'Recording options',
    '保存格式': 'Format',
    'MP4 · 四路 H.264': 'MP4 · Four H.264 cameras',
    '固定保存位置': 'Storage location',
    '运行目录 / eMMC': 'Runtime directory / eMMC',
    'TF 卡': 'TF card',
    '高级选项': 'Advanced options',
    '图像隔帧': 'Frame skipping',
    '0 · 保存每组': '0 · Save every group',
    '1 · 交替保存（仅 bag）': '1 · Alternate groups (bag only)',
    '最小丢组间隔': 'Minimum drop interval',
    '间隔为 2–4294967295 的整数；隔帧为 1 时固定为 2。仅用于 ROS bag。': 'The interval is an integer from 2 to 4294967295; fixed at 2 when frame skipping is 1. Applies to ROS bag only.',
    'MP4 使用四路 H.264 视频；不支持隔帧保存。': 'MP4 saves four H.264 videos; frame skipping is not supported.',
    '开始录制': 'Start Recording',
    '停止录制': 'Stop Recording',
    '查询状态': 'Query Status',
    '等待设备实时录制状态。': 'Waiting for live device recording status.',
    '时长（设备计时）': 'Duration (device clock)',
    '已写入': 'Written',
    '录制详情与使用说明': 'Recording details and guidance',
    '可用空间': 'Free space',
    '已丢弃组数': 'Dropped groups',
    '最终结果': 'Final outcome',
    '会话 ID': 'Session ID',
    '格式 / 存储': 'Format / storage',
    '输出路径（设备端）': 'Output path (on device)',
    '完整性 / 清理': 'Completeness / cleanup',
    '健康状态范围': 'Health scope',
    '仅限可信局域网。此页面没有身份认证，能访问设备的人可启动或停止录制。请勿暴露到公网。': 'Trusted LAN only. This page has no authentication; anyone who can reach the device can start or stop recording. Do not expose it to the public Internet.',
    '路径由设备分配：运行目录为 /root/demo/save_data，TF 卡为 /mnt/sdcard/recordings，不接受自定义路径。': 'Paths are allocated by the device: /root/demo/save_data for runtime storage, or /mnt/sdcard/recordings on the TF card. Custom paths are not accepted.',
    '同一时刻只能运行一个 bag 或 MP4 会话。录制保存原生 IMU，预览抽取、滤波和浏览器姿态校准不改变保存数据。MP4 保存四路 H.264 视频，不是屏幕录制。': 'Only one bag or MP4 session can run at a time. Recording saves native IMU samples; preview decimation, filtering, and browser attitude calibration do not alter saved data. MP4 saves four H.264 videos, not a screen capture.',
    '关闭网页或断网不会停止设备录制。停止后仍需排空和封装；只有设备返回最终结果才可判断完成。命令超时后请查询状态，不要盲目重复发送，也不要在停止过程中断电。': 'Closing the page or losing the network does not stop device recording. Stopping still requires draining and finalization; completion is determined only by the device final result. After a command timeout, query status instead of blindly resending. Do not power off during finalization.',
    '运行诊断': 'Diagnostics',
    '数据链路 · 浏览器接收与解码': 'Data pipeline · Browser reception and decoding',
    '设备数据链路': 'Device data pipeline',
    '等待设备状态': 'Waiting for device status',
    '各路浏览器接收': 'Per-camera browser reception',
    '如何理解这些数据': 'Interpreting these metrics',
    '解码实现以浏览器返回字段为准；缺失显示 unknown。有 GPU 不保证四路硬解，请结合浏览器诊断页与操作系统 Video Decode 引擎确认。通知缺口不等于样本丢失。': 'Decoder implementation comes from browser-reported fields; missing values show unknown. A GPU does not guarantee hardware decoding for all four streams. Check browser diagnostics and the operating system Video Decode engine. Notification gaps do not mean sample loss.',
    '页面不请求摄像头或麦克风权限。仅限可信局域网：页面无身份认证，局域网访问者可预览并控制设备录制；HTTP 不提供传输真实性保护，切勿暴露到公网。': 'The page does not request camera or microphone access. Trusted LAN only: there is no authentication, and LAN visitors can preview and control device recording. HTTP does not authenticate transport; do not expose it to the public Internet.',
    'RoboBaton 4P · 视频 / IMU / 保存彼此独立': 'RoboBaton 4P · Independent video / IMU / recording',
    '可信局域网 · 离线资源': 'Trusted LAN · Offline assets',
    '切换到英文': 'Switch to English',
    '在线文档': 'Online Docs',
    '在线文档（新标签页打开）': 'Online documentation (opens in a new tab)',
    '空闲': 'Idle', '未启用': 'Disabled', '不支持此编码': 'Unsupported codec',
    '等待关键帧': 'Waiting for keyframe', '传输中': 'Flowing', '降级': 'Degraded', '数据过期': 'Stale',
    '浏览器无法确认源 H.264 接收能力': 'The browser cannot confirm support for the source H.264 stream',
    '浏览器不支持源 H.264 {profile}': 'The browser does not support source H.264 {profile}',
    '浏览器没有源 H.264 {profile} 的原生接收格式': 'The browser has no native receive format for source H.264 {profile}',
    'CAM{camera} 实时视频，双击或按 Enter / 空格放大、还原，Esc 返回网格': 'CAM{camera} live video. Double-click or press Enter / Space to expand or restore; Esc returns to the grid',
    '双击放大 / 还原 · Esc 返回网格': 'Double-click to expand / restore · Esc returns to grid',
    '点击播放': 'Click to Play',
    'Web 视频仅支持 H.264': 'Web video supports H.264 only',
    '建立视频会话': 'Establishing video session',
    '等待解码首帧': 'Waiting for first decoded frame',
    '视频断开，重连中': 'Video disconnected, reconnecting',
    'ICE 超时': 'ICE timeout',
    '无效会话地址': 'Invalid session address',
    '连接失败：{error}': 'Connection failed: {error}',
    '等待点击播放': 'Waiting for click to play',
    '播放中': 'Playing',
    '视频无进展，重建会话': 'Video stalled, rebuilding session',
    '诊断连接失效，重建会话': 'Diagnostics connection lost, rebuilding session',
    '无效遥测': 'Invalid telemetry',
    '遥测断开，重连中': 'Telemetry disconnected, reconnecting',
    '实时遥测': 'Live telemetry',
    '数据过期 / 等待新样本': 'Stale / waiting for new samples',
    '异常': 'Invalid',
    '{rate} Hz 原生 / ≤{limit} Hz 预览 · sequence {sequence} · 温度 {temperature} °C · 时钟 {mode} · burst {evidence}': '{rate} Hz native / ≤{limit} Hz preview · sequence {sequence} · Temperature {temperature} °C · Clock {mode} · Burst {evidence}',
    '设备状态不可用': 'Device status unavailable',
    '设备已连接': 'Device connected',
    '采集 {capture} · 编码 {encoding} · 保存 {saving} · RTSP {rtsp} · IMU {telemetry}': 'Capture {capture} · Encoding {encoding} · Saving {saving} · RTSP {rtsp} · IMU {telemetry}',
    '设备连接中断': 'Device disconnected',
    '无法读取设备信息': 'Unable to read device information',
    '设备不可用': 'Device unavailable',
    '{connected} / {total} 路': '{connected} / {total} cameras',
    '采集中': 'Capturing',
    '数据过期 / 等待': 'Stale / waiting',
    '{rate} Hz 原生 / ≤{limit} Hz 预览': '{rate} Hz native / ≤{limit} Hz preview',
    "请保持设备静止，正在采集约 3 秒": "Keep the device still; collecting samples for about 3 seconds",
    "浏览器姿态计算不可用": "Browser attitude estimation unavailable",
    "等待实时样本；同一采集会话保留已校准零偏": "Waiting for live samples; calibrated bias is retained within the same capture session",
    "数据过期 · 模型已冻结": "Data stale · Model frozen",
    "遥测中断；未完成的校准需重新开始": "Telemetry interrupted; incomplete calibration must be restarted",
    "重力辅助融合 · 客户端计算": "Gravity-aided fusion · Client-side estimation",
    "运动加速度较大 · 仅陀螺积分": "High motion acceleration · Gyroscope integration only",
    "零偏已扣除 [{bias}] rad/s · 静置零偏跟踪": "Bias removed [{bias}] rad/s · Stationary bias tracking",
    "零偏已扣除 [{bias}] rad/s · 运动/等待静置窗口": "Bias removed [{bias}] rad/s · Moving / Waiting for a stationary window",
    "请静置约 3 秒 · 自动估计零偏 {progress}%": "Keep still for about 3 seconds · Automatic bias estimation {progress}%",
    "请保持设备静止 · {progress}%": "Keep the device still · {progress}%",
    "校准拒绝：运动、振动或样本不足；请静置重试。": "Calibration rejected: motion, vibration, or insufficient samples; keep still and retry. ",
    "校准中断，请静置重试。": "Calibration interrupted; keep still and retry. ",
    "无效录制状态": "Invalid recording status",
    "已查询到新的设备确认，请依据下方真实状态操作。": "A new device acknowledgement was received. Use the actual status below to decide what to do.",
    "正在启动": "Starting",
    "录制中": "Recording",
    "正在停止 · 排空 / 封装中": "Stopping · Draining / Finalizing",
    "录制完成": "Recording complete",
    "部分完成": "Partially complete",
    "录制失败": "Recording failed",
    "状态不可用 / 已过期 · 控制已禁用": "Status unavailable / stale · Controls disabled",
    "命令结果不确定，正在查询设备确认。请勿盲目重试；录制可能仍在运行。": "Command outcome uncertain; querying the device for confirmation. Do not blindly retry; recording may still be running.",
    "等待设备确认命令；收到确认后继续查询实际录制状态。": "Waiting for the device to acknowledge the command; actual recording status will then be queried.",
    "设备正在排空写入队列、关闭文件或完成 MP4 封装。请等待最终结果，不要断电。": "The device is draining write queues, closing files, or finalizing MP4 files. Wait for the final result; do not disconnect power.",
    "等待设备实时录制状态；不会使用过期数据执行控制。": "Waiting for live recording status from the device; stale data will not be used to control recording.",
    "已丢弃编码帧数": "Dropped encoded frames",
    "{format} / TF 卡": "{format} / TF card",
    "{format} / 运行目录 / eMMC": "{format} / Runtime directory / eMMC",
    "设备尚未分配路径": "The device has not assigned a path yet",
    "尚未产生最终结果": "No final result yet",
    "数据完整：{data} · 清理完成：{cleanup}": "Data complete: {data} · Cleanup complete: {cleanup}",
    "是": "Yes",
    "否": "No",
    "录制会话": "Recording session",
    "当前设备不支持 MP4：需要四路相机及 H.264 编码。可选择 ROS bag。": "MP4 is unavailable on this device: four cameras and H.264 encoding are required. Select ROS bag instead.",
    "设备录制状态不可用": "Device recording status unavailable",
    "查询失败：{error}。控制已禁用。": "Status query failed: {error}. Controls disabled.",
    "设备拒绝录制命令": "The device rejected the recording command",
    "设备已确认命令；以下状态与最终结果均来自设备，不代表文件已经保存完成。": "The device acknowledged the command; the status and final result below come from the device and do not mean the files have finished saving.",
    "命令未确认成功：{error}。请查询实际状态，不要盲目重试。": "Command success was not confirmed: {error}. Query the actual status; do not blindly retry.",
  };
  const bindings = new Map();
  let language = 'zh-CN';
  function t(source, params = {}) {
    if (source && typeof source === 'object') return t(source.source, source.params);
    const template = language === 'en' && Object.hasOwn(english, source) ? english[source] : source;
    return String(template).replace(/\{(\w+)\}/g, (match, key) => Object.hasOwn(params, key) ? t(params[key]) : match);
  }
  function text(node, source, params = {}) {
    bindings.set(node, { source, params });
    node.textContent = t(source, params);
  }
  function copyText(node, source) {
    const binding = bindings.get(source);
    text(node, binding.source, binding.params);
  }
  function attribute(node, name, source, params = {}) {
    node.setAttribute(name, t(source, params));
    let attributes = attributeBindings.get(node);
    if (!attributes) { attributes = new Map(); attributeBindings.set(node, attributes); }
    attributes.set(name, { source, params });
  }
  const attributeBindings = new Map();
  function setLanguage(value) {
    language = value === 'en' ? 'en' : 'zh-CN';
    document.documentElement.lang = language;
    for (const [node, binding] of bindings) node.textContent = t(binding.source, binding.params);
    for (const [node, attributes] of attributeBindings) for (const [name, binding] of attributes) node.setAttribute(name, t(binding.source, binding.params));
    const toggle = document.getElementById('language-toggle');
    toggle.textContent = language === 'en' ? 'CH' : 'EN';
    toggle.lang = language === 'en' ? 'zh-CN' : 'en';
    toggle.setAttribute('aria-label', language === 'en' ? 'Switch to Chinese' : '切换到英文');
    const docs = document.getElementById('online-docs');
    docs.href = language === 'en' ? 'https://4p-doc-en.readthedocs.io/en/4pv2.main/' : 'https://4p-docs.readthedocs.io/zh-cn/4pv2.main/';
    docs.hreflang = language;
    try { localStorage.setItem('4p-language', language); } catch { /* Language switching also works when browser storage is blocked. */ }
  }
  function init() {
    // Bind existing text nodes, not parent elements, so controls and live state survive a language change.
    const walker = document.createTreeWalker(document.documentElement, NodeFilter.SHOW_TEXT);
    const nodes = [];
    while (walker.nextNode()) if (/[\u3400-\u9fff]/.test(walker.currentNode.textContent) && !['SCRIPT', 'STYLE'].includes(walker.currentNode.parentElement?.tagName)) nodes.push(walker.currentNode);
    for (const node of nodes) {
      const source = node.textContent.trim();
      const prefix = node.textContent.match(/^\s*/)[0]; const suffix = node.textContent.match(/\s*$/)[0];
      text(node, '{prefix}{content}{suffix}', { prefix, content: source, suffix });
    }
    for (const node of document.querySelectorAll('[aria-label], [title]')) for (const name of ['aria-label', 'title']) {
      const source = node.getAttribute(name);
      if (source && /[\u3400-\u9fff]/.test(source)) attribute(node, name, source);
    }
    let saved; try { saved = localStorage.getItem('4p-language'); } catch { /* Default to Chinese without persistent storage. */ }
    document.getElementById('language-toggle').addEventListener('click', () => setLanguage(language === 'en' ? 'zh-CN' : 'en'));
    setLanguage(saved);
  }
  return { t, text, copyText, attribute, init };
})();
