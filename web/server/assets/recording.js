'use strict';

class RecordingView {
  constructor() {
    this.status = null; this.receivedAt = 0; this.busy = false; this.querying = false; this.closed = false;
    this.uncertainAck = null; this.notice = ''; this.noticeParams = {}; this.request = null; this.queryFailed = false; this.mutationEpoch = 0;
    this.node = id => document.getElementById(`recording-${id}`);
    this.node('form').addEventListener('submit', event => { event.preventDefault(); this.mutate('start'); });
    this.node('stop').addEventListener('click', () => this.mutate('stop'));
    this.node('refresh').addEventListener('click', () => this.poll());
    this.node('format').addEventListener('change', () => this.optionsChanged());
    this.node('skip').addEventListener('change', () => this.optionsChanged());
    this.node('storage').addEventListener('change', () => this.render());
    this.optionsChanged();
    this.watchdog = setInterval(() => this.render(), 250);
  }
  optionsChanged() {
    const mp4 = this.node('format').value === 'mp4';
    if (mp4) this.node('skip').value = '0';
    if (this.node('skip').value === '1') this.node('interval').value = '2';
    this.render();
  }
  fresh() { return this.status !== null && performance.now() - this.receivedAt <= 1000; }
  accept(status, age = 0) {
    const decimal = value => typeof value === 'string' && /^(0|[1-9][0-9]*)$/.test(value) && BigInt(value) <= 18446744073709551615n;
    if (!status || !['idle', 'starting', 'recording', 'stopping', 'complete', 'partial', 'failed'].includes(status.state) ||
        !['request_id', 'session_id', 'elapsed_ms', 'bytes_written', 'dropped_groups', 'free_bytes'].every(key => decimal(status[key])) ||
        typeof status.can_start !== 'boolean' || typeof status.mp4_available !== 'boolean' ||
        (status.dropped_frames !== undefined && !decimal(status.dropped_frames)) ||
        typeof status.data_complete !== 'boolean' || typeof status.cleanup_complete !== 'boolean') throw new Error('无效录制状态');
    this.status = status; this.receivedAt = performance.now() - age;
    if (this.queryFailed) { this.notice = ''; this.noticeParams = {}; this.queryFailed = false; }
    if (this.uncertainAck !== null && status.request_id !== this.uncertainAck) {
      this.uncertainAck = null; this.notice = '已查询到新的设备确认，请依据下方真实状态操作。'; this.noticeParams = {};
    }
    this.render();
  }
  render() {
    const status = this.status; const fresh = this.fresh(); const locked = !fresh || this.busy || this.uncertainAck !== null;
    const active = status && ['starting', 'recording', 'stopping'].includes(status.state);
    const configure = !locked && status.can_start;
    this.node('format').disabled = !configure;
    this.node('storage').disabled = !configure;
    this.node('mp4-option').disabled = !fresh || !status.mp4_available;
    const mp4 = this.node('format').value === 'mp4';
    this.node('skip').disabled = !configure || mp4;
    this.node('interval').disabled = !configure || this.node('skip').value === '1';
    this.node('start').disabled = !configure || (mp4 && !status.mp4_available);
    this.node('stop').disabled = locked || !status || status.state !== 'recording';
    this.node('form').setAttribute('aria-busy', String(this.busy));
    const labels = { idle: '空闲', starting: '正在启动', recording: '录制中', stopping: '正在停止 · 排空 / 封装中', complete: '录制完成', partial: '部分完成', failed: '录制失败' };
    i18n.text(this.node('state'), fresh ? labels[status.state] : '状态不可用 / 已过期 · 控制已禁用');
    this.node('state').classList.toggle('stale', !fresh);
    let message = this.notice; let params = this.noticeParams;
    if (this.uncertainAck !== null) { message = '命令结果不确定，正在查询设备确认。请勿盲目重试；录制可能仍在运行。'; params = {}; }
    else if (this.busy) { message = '等待设备确认命令；收到确认后继续查询实际录制状态。'; params = {}; }
    else if (fresh && status.state === 'stopping') { message = '设备正在排空写入队列、关闭文件或完成 MP4 封装。请等待最终结果，不要断电。'; params = {}; }
    else if (!fresh && !message) { message = '等待设备实时录制状态；不会使用过期数据执行控制。'; params = {}; }
    i18n.text(this.node('message'), message, params);
    if (!status) return;
    const put = (id, source, params = {}) => { i18n.text(this.node(id), source, params); };
    const milliseconds = BigInt(status.elapsed_ms); const seconds = milliseconds / 1000n;
    put('duration', `${seconds / 3600n}:${String(seconds / 60n % 60n).padStart(2, '0')}:${String(seconds % 60n).padStart(2, '0')}`);
    const bytes = BigInt(status.bytes_written); const gigabytes = bytes >= 1000000000n;
    const unit = gigabytes ? 1000000000n : 1000000n;
    const hundredths = (bytes * 100n + unit / 2n) / unit;
    put('bytes', `${hundredths / 100n}.${String(hundredths % 100n).padStart(2, '0')} ${gigabytes ? 'GB' : 'MB'}`);
    put('free', `${status.free_bytes} B`);
    const frameDrops = status.format === 'mp4' && status.dropped_frames !== undefined;
    put('drops', frameDrops ? status.dropped_frames : status.dropped_groups);
    put('drop-label', frameDrops ? '已丢弃编码帧数' : '已丢弃组数');
    put('session', status.session_id);
    put('destination', status.storage === 'tf' ? '{format} / TF 卡' : '{format} / 运行目录 / eMMC', {format: status.format || '—'});
    put('path', status.path || '设备尚未分配路径');
    put('error', status.error || ''); this.node('error').hidden = !status.error;
    put('outcome', status.outcome || (active ? '尚未产生最终结果' : '—'));
    put('completion', '数据完整：{data} · 清理完成：{cleanup}', {data: status.data_complete ? '是' : '否', cleanup: status.cleanup_complete ? '是' : '否'});
    put('scope', status.health_scope || '录制会话');
    put('mp4-help', fresh && !status.mp4_available ? '当前设备不支持 MP4：需要四路相机及 H.264 编码。可选择 ROS bag。' : 'MP4 使用四路 H.264 视频；不支持隔帧保存。');
  }
  async poll() {
    if (this.closed || this.querying) return;
    if (this.busy) { clearTimeout(this.timer); this.timer = setTimeout(() => this.poll(), 400); return; }
    clearTimeout(this.timer); this.querying = true;
    const epoch = this.mutationEpoch;
    const sentAt = performance.now();
    try {
      const response = await fetch('/api/v1/recording', { signal: AbortSignal.timeout(1500) });
      const body = await response.json();
      if (!response.ok) throw new Error(body.error?.message || '设备录制状态不可用');
      if (!this.closed && epoch === this.mutationEpoch && !this.busy) this.accept(body, Number(response.headers.get('X-Recording-Status-Age-Ms') || 0) + performance.now() - sentAt);
    } catch (error) {
      if (!this.closed && epoch === this.mutationEpoch && !this.busy) {
        this.receivedAt = -Infinity; this.queryFailed = true;
        this.notice = '查询失败：{error}。控制已禁用。'; this.noticeParams = {error: error.message}; this.render();
      }
    } finally {
      this.querying = false;
      if (!this.closed) this.timer = setTimeout(() => this.poll(), 400);
    }
  }
  async mutate(action) {
    this.render();
    if (this.busy || !this.fresh() || this.uncertainAck !== null || this.node(action).disabled) return;
    const beforeAck = this.status.request_id;
    const payload = { action, expected_session_id: this.status.session_id };
    if (action === 'start') {
      if (!this.node('form').checkValidity()) {
        this.node('advanced').open = true;
        requestAnimationFrame(() => this.node('form').reportValidity());
        return;
      }
      Object.assign(payload, { format: this.node('format').value, storage: this.node('storage').value,
        frame_skip: Number(this.node('skip').value), drop_min_interval: Number(this.node('interval').value) });
      if (payload.frame_skip === 1 && payload.drop_min_interval !== 2) return;
    }
    this.busy = true; this.notice = ''; this.noticeParams = {}; this.mutationEpoch++; this.render();
    const abort = new AbortController(); this.request = abort;
    const timeout = setTimeout(() => abort.abort(), 4500);
    try {
      const response = await fetch('/api/v1/recording', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload), signal: abort.signal });
      const body = await response.json();
      if (!response.ok) {
        if (body.error?.code === 'recording_uncertain') this.uncertainAck = beforeAck;
        throw new Error(body.error?.message || '设备拒绝录制命令');
      }
      this.uncertainAck = beforeAck;
      if (!this.closed) {
        this.accept(body, 1001);
        this.notice = '设备已确认命令；以下状态与最终结果均来自设备，不代表文件已经保存完成。'; this.noticeParams = {};
      }
    } catch (error) {
      if (!this.closed) {
        if (error.name === 'AbortError' || error instanceof TypeError || error instanceof SyntaxError) this.uncertainAck = beforeAck;
        this.notice = '命令未确认成功：{error}。请查询实际状态，不要盲目重试。'; this.noticeParams = {error: error.message};
      }
    } finally {
      clearTimeout(timeout); this.request = null; this.busy = false;
      if (!this.closed) { this.render(); this.poll(); }
    }
  }
  close() {
    this.closed = true; clearTimeout(this.timer); clearInterval(this.watchdog);
    // Aborting an HTTP wait does not send a stop command to the device.
    this.request?.abort();
  }
}
