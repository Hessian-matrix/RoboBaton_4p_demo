'use strict';

const dashboard = (() => {
  i18n.init();
  document.querySelector('.skip-link').addEventListener('click', event => {
    event.preventDefault();
    document.getElementById('main-content').focus();
  });
  const decoderRows = new Map();
  return {
    update(state, status) {
      const put = (id, value, params) => i18n.text(document.getElementById(id), value, params);
      const enabled = state.cameras.filter(camera => camera.config.enabled);
      const connected = enabled.filter(camera => camera.pc?.connectionState === 'connected' && camera.lastFrames > 0).length;
      put('summary-cameras', '{connected} / {total} 路', { connected, total: enabled.length });
      const captureLabels = {flowing: '采集中', idle: '空闲', degraded: '降级', stale: '数据过期', disabled: '未启用'};
      put('summary-capture', status ? captureLabels[status.capture] || status.capture : '设备连接中断');
      const fresh = state.lastIMU > 0 && performance.now() - state.lastIMU < 500;
      put('summary-imu', fresh ? '实时遥测' : '数据过期 / 等待');
      document.getElementById('summary-imu').classList.toggle('stale', !fresh);
      put('summary-rate', '{rate} Hz 原生 / ≤{limit} Hz 预览', { rate: state.info.source_rate_hz, limit: state.info.telemetry_limit_hz });
      put('summary-temperature', fresh && Number.isFinite(state.latest?.temperature_c) ? `${state.latest.temperature_c.toFixed(1)} °C` : '—');
      for (const camera of state.cameras) {
        let row = decoderRows.get(camera.config.camera_id);
        if (!row) {
          const root = document.createElement('article'); root.className = 'decoder-row';
          const title = document.createElement('h3'); title.textContent = `CAM${camera.config.camera_id + 1}`;
          const health = document.createElement('p'); const detail = document.createElement('p'); detail.className = 'mono';
          root.append(title, health, detail); document.getElementById('decoder-status').append(root);
          row = {health, detail}; decoderRows.set(camera.config.camera_id, row);
        }
        i18n.copyText(row.health, camera.status);
        i18n.copyText(row.detail, camera.diag);
      }
    },
  };
})();
