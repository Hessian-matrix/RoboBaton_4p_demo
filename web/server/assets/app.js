'use strict';
const $ = id => document.getElementById(id);
const state = { info: null, cameras: [], events: null, points: [], lastIMU: 0, session: null, sequence: null, latest: null, closed: false, attitude: null, recording: null };
const labels = { idle: '空闲', disabled: '未启用', unsupported: '不支持此编码', 'waiting-keyframe': '等待关键帧', flowing: '传输中', degraded: '降级', stale: '数据过期' };
function text(node, value, params) { i18n.text(node, value, params); }
async function receiveOffer(pc, sourceProfile) {
  const offer = await pc.createOffer();
  if (!sourceProfile) return offer;
  if (!navigator.mediaCapabilities?.decodingInfo) throw new Error('浏览器无法确认源 H.264 接收能力');
  const info = state.info;
  const capability = await navigator.mediaCapabilities.decodingInfo({ type: 'webrtc', video: {
    contentType: `video/H264;packetization-mode=1;profile-level-id=${sourceProfile}`,
    width: info.width, height: info.height, bitrate: info.bitrate_bps, framerate: info.fps
  } });
  if (!capability.supported) throw Object.assign(new Error('浏览器不支持源 H.264 {profile}'), { translation: { source: '浏览器不支持源 H.264 {profile}', params: { profile: sourceProfile } } });
  // Only raise a native offer's matching profile after checking the actual source.
  // The encoded SPS and server-side receiver validation remain unchanged.
  let matched = false;
  offer.sdp = offer.sdp.replace(/^a=fmtp:\d+ [^\r\n]+/gm, line => {
    const profile = /(?:^|[; ])profile-level-id=([0-9a-f]{6})(?:;|$)/i.exec(line);
    if (!profile || profile[1].slice(0, 4).toLowerCase() !== sourceProfile.slice(0, 4) || !/(?:^|[; ])packetization-mode=1(?:;|$)/.test(line)) return line;
    matched = true; return line.replace(/profile-level-id=[0-9a-f]{6}/i, `profile-level-id=${sourceProfile}`);
  });
  if (!matched) throw Object.assign(new Error('浏览器没有源 H.264 {profile} 的原生接收格式'), { translation: { source: '浏览器没有源 H.264 {profile} 的原生接收格式', params: { profile: sourceProfile } } });
  return offer;
}
class Camera {
  constructor(config) {
    this.config = config; this.sourceProfile = null; this.pc = null; this.abort = null; this.location = null; this.timer = null; this.failures = 0; this.active = false; this.attempt = 0; this.lastFrames = 0; this.progressAt = 0;
    this.root = document.createElement('article'); this.root.className = 'camera';
    const title = document.createElement('h3'); title.className = 'camera-label'; text(title, `CAM${config.camera_id + 1}`);
    this.status = document.createElement('span'); this.status.className = 'state'; this.status.setAttribute('role', 'status');
    this.video = document.createElement('video'); this.video.autoplay = true; this.video.muted = true; this.video.playsInline = true;
    this.surface = document.createElement('div'); this.surface.className = 'camera-image'; this.surface.tabIndex = 0;
    this.surface.setAttribute('role', 'button'); this.surface.setAttribute('aria-pressed', 'false');
    i18n.attribute(this.surface, 'aria-label', 'CAM{camera} 实时视频，双击或按 Enter / 空格放大、还原，Esc 返回网格', { camera: config.camera_id + 1 });
    i18n.attribute(this.surface, 'title', '双击放大 / 还原 · Esc 返回网格');
    this.surface.ondblclick = () => this.setExpanded(!this.root.classList.contains('expanded'));
    this.surface.onkeydown = event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); if (!event.repeat) this.setExpanded(!this.root.classList.contains('expanded')); } };
    this.play = document.createElement('button'); this.play.className = 'play'; this.play.hidden = true; text(this.play, '点击播放'); this.play.onclick = () => this.startPlayback();
    this.diag = document.createElement('div'); this.diag.className = 'diagnostics'; text(this.diag, 'decoder: unknown');
    this.surface.append(this.video, title); this.root.append(this.surface, this.status, this.play, this.diag); $('cameras').append(this.root);
    if (!config.enabled) { text(this.status, '未启用'); return; }
    if (state.info.codec !== 'h264') { text(this.status, 'Web 视频仅支持 H.264'); return; }
    this.active = true; this.connect();
  }
  setExpanded(enlarged) {
    if (enlarged) state.cameras.forEach(camera => { if (camera !== this) camera.setExpanded(false); });
    this.root.classList.toggle('expanded', enlarged); this.surface.setAttribute('aria-pressed', String(enlarged));
    if (enlarged) this.surface.focus({ preventScroll: true });
  }
  cleanup() {
    this.attempt++; clearTimeout(this.timer); this.timer = null;
    if (this.abort) this.abort.abort(); this.abort = null;
    if (this.pc) { this.pc.onconnectionstatechange = null; this.pc.close(); } this.pc = null;
    if (this.location) fetch(this.location, { method: 'DELETE', keepalive: true }).catch(() => {});
    this.location = null; this.video.srcObject = null; this.play.hidden = true;
  }
  retry(reason) {
    if (!this.active || state.closed || this.timer) return;
    this.cleanup();
    if (typeof reason === 'string') text(this.status, reason); else text(this.status, reason.source, reason.params);
    const delay = Math.min(3000, 250 * (2 ** Math.min(this.failures++, 4))) + Math.random() * 200;
    this.timer = setTimeout(() => { this.timer = null; this.connect(); }, delay);
  }
  async connect() {
    if (!this.active || state.closed) return;
    const token = ++this.attempt; this.progressAt = performance.now(); this.lastFrames = 0;
    text(this.status, '建立视频会话'); const pc = new RTCPeerConnection({ iceServers: [] }); this.pc = pc;
    const abort = new AbortController(); this.abort = abort;
    const deadline = setTimeout(() => abort.abort(), 15000);
    try {
      pc.addTransceiver('video', { direction: 'recvonly' });
      pc.ontrack = event => { if (token !== this.attempt) return; this.video.srcObject = event.streams[0] || new MediaStream([event.track]); this.startPlayback(); };
      pc.onconnectionstatechange = () => {
        if (token !== this.attempt) return;
        if (pc.connectionState === 'connected') { this.failures = 0; text(this.status, '等待解码首帧'); }
        if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) this.retry('视频断开，重连中');
      };
      await pc.setLocalDescription(await receiveOffer(pc, this.sourceProfile));
      if (pc.iceGatheringState !== 'complete') await new Promise((resolve, reject) => {
        const changed = () => { if (pc.iceGatheringState === 'complete') { finish(); resolve(); } };
        const cancelled = () => { finish(); reject(new Error('ICE 超时')); };
        const finish = () => { pc.removeEventListener('icegatheringstatechange', changed); abort.signal.removeEventListener('abort', cancelled); };
        pc.addEventListener('icegatheringstatechange', changed); abort.signal.addEventListener('abort', cancelled, { once: true });
        if (abort.signal.aborted) cancelled(); else changed();
      });
      const response = await fetch(this.config.sessions, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ type: 'offer', sdp: pc.localDescription.sdp }), signal: abort.signal });
      if (!response.ok) {
        const { error } = await response.json();
        if (response.status === 422 && error.code === 'negotiation' && /^[0-9a-f]{6}$/.test(error.source_profile_level_id)) this.sourceProfile = error.source_profile_level_id;
        throw new Error(error.message);
      }
      const location = response.headers.get('Location');
      if (!location || !/^\/api\/v1\/video-sessions\/[a-f0-9]{32}$/.test(location)) throw new Error('无效会话地址');
      if (token !== this.attempt) { fetch(location, { method: 'DELETE', keepalive: true }).catch(() => {}); return; }
      this.location = location; const answer = await response.json(); await pc.setRemoteDescription({ type: answer.type, sdp: answer.sdp });
    } catch (error) { if (token === this.attempt) this.retry({ source: '连接失败：{error}', params: { error: error.translation || error.message } }); }
    finally { clearTimeout(deadline); }
  }
  async startPlayback() { try { await this.video.play(); this.play.hidden = true; } catch { this.play.hidden = false; text(this.status, '等待点击播放'); } }
  async stats() {
    const pc = this.pc; if (!pc) return;
    if (['failed', 'disconnected', 'closed'].includes(pc.connectionState)) { this.retry('视频断开，重连中'); return; }
    if (pc.connectionState !== 'connected') return;
    try { const reports = await pc.getStats(); if (this.pc !== pc) return;
      reports.forEach(report => { if (report.type !== 'inbound-rtp' || report.kind !== 'video') return;
        const frames = report.framesDecoded || 0; const now = performance.now();
        if (frames > this.lastFrames) { this.progressAt = now; this.lastFrames = frames; text(this.status, '播放中'); }
        const decoder = report.decoderImplementation || 'unknown';
        text(this.diag, `decoded ${frames} · ${report.framesPerSecond ?? '—'} fps · lost ${report.packetsLost ?? '—'} · decoder: ${decoder}`);
      });
      if (this.pc === pc && performance.now() - this.progressAt > 4000) this.retry('视频无进展，重建会话');
    } catch { if (this.pc === pc) this.retry('诊断连接失效，重建会话'); }
  }
}
function setupValues(id) {
  for (const axis of ['X', 'Y', 'Z']) { const group = document.createElement('div'); const dt = document.createElement('dt'); const dd = document.createElement('dd'); text(dt, axis); text(dd, '—'); group.append(dt, dd); $(id).append(group); }
}
const IMU_CHART_WINDOW_MS = 1000;
function imuMessage(message) {
  if (message.schema_version !== 2 || !Array.isArray(message.accel_mps2) || !Array.isArray(message.gyro_rps)) return;
  if (state.session !== message.session_id) { state.session = message.session_id; state.points = []; state.sequence = null; }
  let sequence; try { sequence = BigInt(message.sample_sequence); } catch { return; }
  if (state.sequence !== null && sequence <= state.sequence) return;
  state.sequence = sequence; state.lastIMU = performance.now(); state.latest = message;
  state.attitude.sample(message);
  state.points.push({ time: state.lastIMU, accel: message.accel_mps2, gyro: message.gyro_rps });
  while (state.points.length > 500 || state.points.length && state.lastIMU - state.points[0].time > IMU_CHART_WINDOW_MS) state.points.shift();
}
function openIMU() {
  const events = new EventSource('/api/v1/imu/events'); state.events = events;
  events.addEventListener('imu', event => { try { imuMessage(JSON.parse(event.data)); } catch { text($('imu-state'), '无效遥测'); } });
  events.onopen = () => { if (state.attitude.session !== null) state.attitude.reset(); };
  events.onerror = () => { text($('imu-state'), '遥测断开，重连中'); };
}
function drawChart(id, field, now) {
  const canvas = $(id); const width = canvas.clientWidth; const height = canvas.clientHeight; const ratio = window.devicePixelRatio || 1;
  if (width <= 0 || height <= 0) return;
  if (canvas.width !== Math.round(width * ratio) || canvas.height !== Math.round(height * ratio)) { canvas.width = Math.round(width * ratio); canvas.height = Math.round(height * ratio); }
  const ctx = canvas.getContext('2d'); ctx.setTransform(ratio, 0, 0, ratio, 0, 0); ctx.clearRect(0, 0, width, height);
  const points = state.points.filter(p => p.time >= now - IMU_CHART_WINDOW_MS && p.time <= now);
  const css = getComputedStyle(document.documentElement); const right = width - 8; const top = 12; const bottom = height - 28;
  ctx.font = '10px ui-monospace, monospace'; ctx.lineWidth = 1;
  ctx.fillStyle = css.getPropertyValue('--muted');
  let min = Infinity; let max = -Infinity;
  for (const point of points) for (const value of point[field]) if (Number.isFinite(value)) { min = Math.min(min, value); max = Math.max(max, value); }
  if (!Number.isFinite(min)) { ctx.textAlign = 'left'; ctx.textBaseline = 'middle'; ctx.fillText(i18n.t('等待样本'), 8, (top + bottom) / 2); return; }
  const center = (min + max) / 2; const span = Math.max(max - min, field === 'accel' ? 0.02 : 0.002) * 1.25;
  min = center - span / 2; max = center + span / 2;
  const digits = Math.max(0, Math.min(4, 1 - Math.floor(Math.log10(span))));
  const ticks = Array.from({ length: 5 }, (_, tick) => {
    let value = max - span * tick / 4; if (Math.abs(value) < 0.5 * 10 ** -digits) value = 0;
    return Math.abs(value) >= 10000 ? value.toExponential(1) : value.toFixed(digits);
  });
  const left = Math.ceil(Math.max(...ticks.map(label => ctx.measureText(label).width))) + 6;
  ctx.textBaseline = 'bottom';
  for (let tick = 0; tick <= 2; tick++) {
    ctx.textAlign = tick === 0 ? 'left' : tick === 2 ? 'right' : 'center';
    ctx.fillText(`${-(1 - tick / 2)} s`, left + (right - left) * tick / 2, height);
  }
  ctx.textBaseline = 'middle';
  for (let tick = 0; tick <= 4; tick++) {
    const y = top + (bottom - top) * tick / 4;
    ctx.strokeStyle = css.getPropertyValue('--line'); ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(right, y); ctx.stroke();
    ctx.fillStyle = css.getPropertyValue('--muted'); ctx.textAlign = 'right';
    ctx.fillText(ticks[tick], left - 6, y);
  }
  ctx.strokeStyle = css.getPropertyValue('--line'); ctx.beginPath(); ctx.moveTo(left, top); ctx.lineTo(left, bottom); ctx.stroke();
  ctx.save(); ctx.beginPath(); ctx.rect(left, top, right - left, bottom - top); ctx.clip();
  ['--x', '--y', '--z'].forEach((color, axis) => {
    ctx.strokeStyle = css.getPropertyValue(color); ctx.lineWidth = 1.25; ctx.beginPath(); let previous = null;
    for (const point of points) {
      const value = point[field][axis]; if (!Number.isFinite(value)) { previous = null; continue; }
      const x = left + (right - left) * (1 - (now - point.time) / IMU_CHART_WINDOW_MS); const y = top + (bottom - top) * (1 - (value - min) / span);
      if (previous !== null && point.time - previous <= 250) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      previous = point.time;
    }
    ctx.stroke();
  });
  ctx.restore();
}
let lastDraw = 0;
function render(now) {
  if (state.closed) return;
  state.attitude.render(now);
  if (now - lastDraw >= 50) {
    lastDraw = now; const fresh = state.lastIMU > 0 && now - state.lastIMU < 500; $('imu-state').classList.toggle('stale', !fresh);
    text($('imu-state'), fresh ? '实时遥测' : '数据过期 / 等待新样本');
    if (state.latest) { const sample = state.latest; for (const [id, values] of [['accel-values', sample.accel_mps2], ['gyro-values', sample.gyro_rps]]) [...$(id).querySelectorAll('dd')].forEach((node, i) => text(node, Number.isFinite(values[i]) ? values[i].toFixed(3) : '异常'));
      text($('imu-details'), '{rate} Hz 原生 / ≤{limit} Hz 预览 · sequence {sequence} · 温度 {temperature} °C · 时钟 {mode} · burst {evidence}', { rate: sample.source_rate_hz, limit: sample.telemetry_limit_hz, sequence: sample.sample_sequence, temperature: Number.isFinite(sample.temperature_c) ? sample.temperature_c.toFixed(1) : '异常', mode: sample.timestamp_mode, evidence: sample.evidence_id });
    }
    drawChart('accel-chart', 'accel', now); drawChart('gyro-chart', 'gyro', now);
  }
  requestAnimationFrame(render);
}
async function poll() {
  if (state.closed) return;
  try { const response = await fetch('/api/v1/status', { signal: AbortSignal.timeout(1500) }); if (!response.ok) throw new Error('设备状态不可用'); const status = await response.json();
    if (status.application_session_id !== state.info.application_session_id) { location.reload(); return; }
    text($('connection'), '设备已连接'); text($('source-status'), '采集 {capture} · 编码 {encoding} · 保存 {saving} · RTSP {rtsp} · IMU {telemetry}', { capture: labels[status.capture] || status.capture, encoding: labels[status.encoding] || status.encoding, saving: labels[status.saving] || status.saving, rtsp: labels[status.rtsp] || status.rtsp, telemetry: labels[status.telemetry] || status.telemetry });
    dashboard.update(state, status);
  } catch { text($('connection'), '设备连接中断'); dashboard.update(state, null); }
  await Promise.all(state.cameras.map(camera => camera.stats())); setTimeout(poll, 1000);
}
function closePage() { state.closed = true; if (state.events) state.events.close(); state.attitude?.close(); state.recording?.close(); state.cameras.forEach(camera => camera.cleanup()); }
window.addEventListener('pagehide', closePage); window.addEventListener('pageshow', event => { if (event.persisted) location.reload(); });
document.addEventListener('keydown', event => {
  if (event.key === 'Escape') state.cameras.forEach(camera => camera.setExpanded(false));
});
(async () => { try { setupValues('accel-values'); setupValues('gyro-values'); const response = await fetch('/api/v1/info', { signal: AbortSignal.timeout(3000) }); if (!response.ok) throw new Error('无法读取设备信息'); state.info = await response.json();
  text($('composition'), `${state.info.version} · Pion ${state.info.pion_version}`); text($('video-config'), `${state.info.width}×${state.info.height} / ${state.info.fps} fps / ${state.info.codec.toUpperCase()}`);
  state.attitude = new AttitudeView();
  state.recording = new RecordingView(); state.recording.poll();
  state.cameras = state.info.cameras.map(config => new Camera(config)); openIMU(); requestAnimationFrame(render); poll();
} catch (error) { $('fatal').hidden = false; text($('fatal'), error.message); text($('connection'), '设备不可用'); } })();
