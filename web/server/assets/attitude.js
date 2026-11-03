'use strict';

class AttitudeView {
  constructor() {
    this.worker = null; this.result = null; this.drawn = null; this.at = 0;
    this.epoch = 0; this.busy = false; this.pending = null; this.session = null;
    this.error = false; this.frameAt = 0;
    this.reducedMotion = window.matchMedia('(prefers-reduced-motion: reduce)');
    this.status = document.getElementById('attitude-state');
    this.button = document.getElementById('attitude-reset');
    this.calibrateButton = document.getElementById('attitude-calibrate');
    this.calibrationStatus = document.getElementById('attitude-calibration');
    this.model = document.getElementById('attitude-model');
    this.values = ['roll','pitch','yaw'].map(id => document.getElementById(`attitude-${id}`));
    this.faces = []; this.axes = [];
    const svg = (name, attributes) => {
      const node = document.createElementNS('http://www.w3.org/2000/svg', name);
      for (const [key,value] of Object.entries(attributes)) node.setAttribute(key, value);
      return node;
    };
    this.faceLayer = document.getElementById('attitude-faces');
    for (const name of ['bottom','top','back','front','left','right']) {
      const node = svg('polygon', {class:`model-face model-${name}`});
      this.faces.push(node); this.faceLayer.append(node);
    }
    const axisLayer = document.getElementById('attitude-axes');
    for (const name of ['X','Y','Z']) {
      const line = svg('line', {class:`model-axis axis-${name.toLowerCase()}`});
      const label = svg('text', {class:`model-axis-label axis-${name.toLowerCase()}`});
      label.textContent = `+${name}`; axisLayer.append(line,label); this.axes.push({line,label});
    }
    this.button.onclick = () => this.reset();
    this.calibrateButton.onclick = () => {
      if (!this.worker || !this.result || performance.now()-this.at >= 500) return;
      this.calibrateButton.disabled = true;
      i18n.text(this.calibrationStatus, '请保持设备静止，正在采集约 3 秒');
      this.worker.postMessage({type:'calibrate'});
    };
    this.draw([1,0,0,0]);
    try {
      this.worker = new Worker('/attitude-worker.js');
      this.worker.onmessage = event => this.receive(event.data);
      this.worker.onerror = () => this.fail();
      this.worker.onmessageerror = () => this.fail();
    } catch { this.fail(); }
  }
  fail() {
    this.error = true; this.pending = null; this.busy = false; this.result = null;
    if (this.worker) this.worker.terminate(); this.worker = null;
    i18n.text(this.status, '浏览器姿态计算不可用'); this.status.classList.add('stale');
    this.button.disabled = true; this.model.classList.add('unavailable');
    this.calibrateButton.disabled = true;
    i18n.text(this.calibrationStatus, '浏览器姿态计算不可用');
  }
  reset() {
    if (!this.worker) return;
    this.epoch++; this.result = null; this.drawn = null; this.pending = null;
    this.busy = false; this.at = 0; this.frameAt = 0;
    this.worker?.postMessage({type:'reset'});
    for (const value of this.values) i18n.text(value, '—');
    i18n.text(this.status, '等待有效重力样本'); this.model.classList.add('unavailable');
    this.calibrateButton.disabled = true;
    i18n.text(this.calibrationStatus, '等待实时样本；同一采集会话保留已校准零偏');
  }
  sample(message) {
    if (!this.worker) return;
    if (this.session !== message.session_id) { this.session = message.session_id; this.reset(); }
    if (this.at && performance.now()-this.at > 500 && this.result) this.reset();
    const sample = {type:'sample', epoch:this.epoch, session_id:message.session_id,
      sample_sequence:message.sample_sequence, timestamp_ns:message.sample_timestamp_ns,
      accel_mps2:message.accel_mps2, gyro_rps:message.gyro_rps,
      attitude_accel_mps2:message.attitude_accel_mps2, attitude_gyro_rps:message.attitude_gyro_rps};
    // One sample in flight plus one replaceable latest sample bounds worker lag.
    if (this.busy) this.pending = sample;
    else { this.busy = true; this.worker.postMessage(sample); }
  }
  receive(message) {
    if (message.epoch !== this.epoch) return;
    if (message.type === 'ready') {
      this.busy = false;
      if (this.pending) {
        const pending = this.pending; this.pending = null; this.busy = true;
        this.worker.postMessage(pending);
      }
    } else if (message.type === 'attitude') {
      this.result = message; this.at = performance.now();
      if (!this.drawn || message.status !== 'tracking') this.drawn = message.quaternion.slice();
    }
  }
  render(now) {
    if (this.error) return;
    const fresh = this.result && now-this.at < 500;
    this.button.disabled = !fresh;
    this.calibrateButton.disabled = !fresh || this.result.calibration_state === 'collecting';
    this.model.classList.toggle('unavailable', !fresh);
    this.status.classList.toggle('stale', !fresh);
    if (!fresh) {
      i18n.text(this.status, this.result ? '数据过期 · 模型已冻结' : '等待有效重力样本');
      i18n.text(this.calibrationStatus, '遥测中断；未完成的校准需重新开始');
      return;
    }
    i18n.text(this.status, this.result.accel_correction ? '重力辅助融合 · 客户端计算' : '运动加速度较大 · 仅陀螺积分');
    const calibration = this.result.calibration_state;
    const bias = this.result.gyro_bias_rps;
    const biasSource = bias ? (this.result.bias_tracking ? '零偏已扣除 [{bias}] rad/s · 静置零偏跟踪' : '零偏已扣除 [{bias}] rad/s · 运动/等待静置窗口') : '请静置约 3 秒 · 自动估计零偏 {progress}%';
    const biasParams = bias ? {bias: bias.map(v=>v.toFixed(4)).join(', ')} : {progress: Math.round((this.result.bias_progress || 0)*100)};
    if (calibration === 'collecting') i18n.text(this.calibrationStatus, '请保持设备静止 · {progress}%', {progress: Math.round(this.result.calibration_progress*100)});
    else {
      const prefix = calibration === 'rejected' ? '校准拒绝：运动、振动或样本不足；请静置重试。' : calibration === 'interrupted' ? '校准中断，请静置重试。' : '';
      i18n.text(this.calibrationStatus, '{prefix}{bias}', {prefix, bias: {source: biasSource, params: biasParams}});
    }
    ['roll_deg','pitch_deg','yaw_deg'].forEach((key,i) => {
      i18n.text(this.values[i], '{angle}°', {angle: this.result[key].toFixed(1)});
    });
    const target = this.result.quaternion;
    const dt = this.frameAt ? Math.min((now-this.frameAt)/1000,0.1) : 0.1; this.frameAt = now;
    const blend = this.reducedMotion.matches ? 1 : 1-Math.exp(-dt/0.07);
    const sign = this.drawn.reduce((sum,v,i) => sum+v*target[i],0) < 0 ? -1 : 1;
    for (let i=0;i<4;i++) this.drawn[i] += (sign*target[i]-this.drawn[i])*blend;
    const norm = Math.hypot(...this.drawn);
    for (let i=0;i<4;i++) this.drawn[i] /= norm;
    this.draw(this.drawn);
  }
  draw(q) {
    const [w,x,y,z] = q;
    const rotate = ([a,b,c]) => [
      (1-2*(y*y+z*z))*a+2*(x*y-w*z)*b+2*(x*z+w*y)*c,
      2*(x*y+w*z)*a+(1-2*(x*x+z*z))*b+2*(y*z-w*x)*c,
      2*(x*z-w*y)*a+2*(y*z+w*x)*b+(1-2*(x*x+y*y))*c];
    // Fixed isometric observer: device +X points lower-right, +Y upper-right,
    // world +Z points up. Projection never remaps sensor axes.
    const project = point => {
      const [a,b,c] = rotate(point);
      return {x:240+76*(0.82*a+0.57*b), y:160+76*(0.33*a-0.47*b-0.82*c), depth:0.47*a-0.67*b+0.57*c};
    };
    const corners = [[-1.4,-0.85,-0.16],[1.4,-0.85,-0.16],[1.4,0.85,-0.16],[-1.4,0.85,-0.16],
      [-1.4,-0.85,0.16],[1.4,-0.85,0.16],[1.4,0.85,0.16],[-1.4,0.85,0.16]].map(project);
    const indices = [[0,1,2,3],[4,5,6,7],[3,2,6,7],[0,1,5,4],[0,3,7,4],[1,2,6,5]];
    const ordered = indices.map((face,i) => ({face,i,depth:face.reduce((sum,k)=>sum+corners[k].depth,0)}));
    ordered.sort((a,b)=>a.depth-b.depth);
    for (const {face,i} of ordered) {
      this.faces[i].setAttribute('points',face.map(k=>`${corners[k].x.toFixed(2)},${corners[k].y.toFixed(2)}`).join(' '));
      this.faceLayer.append(this.faces[i]);
    }
    [[1.95,0,0],[0,1.65,0],[0,0,1.45]].forEach((point,i) => {
      const end=project(point), axis=this.axes[i];
      axis.line.setAttribute('x1',240); axis.line.setAttribute('y1',160);
      axis.line.setAttribute('x2',end.x); axis.line.setAttribute('y2',end.y);
      axis.label.setAttribute('x',end.x+7); axis.label.setAttribute('y',end.y-5);
    });
  }
  close() { this.worker?.terminate(); this.worker=null; this.pending=null; }
}
