'use strict';

// Quaternion [w,x,y,z] rotates device vectors into a gravity-referenced world
// with +Z up. A level device reports [0,0,-g]; yaw has no absolute reference.
const GRAVITY = 9.80665;
const MAX_DT_NS = 250000000n;
const GRAVITY_GAIN = 2.5;
const CALIBRATION_NS = 3000000000n;
const BIAS_TRACK_RATE = 0.002;
const BIAS_TRACK_SECONDS = 6;
let gyroBias = [0,0,0];
let biasReady = false;
let calibration = null;
let calibrationState = 'uncalibrated';
let stationaryWindow = null;
let biasTracking = false;
let quaternion = null;
let session = null;
let sequence = null;
let timestamp = null;

function reset() {
  quaternion = null; sequence = null; timestamp = null;
  stationaryWindow = null; biasTracking = false;
  cancelCalibration();
}
function cancelCalibration() {
  if (calibration) calibrationState = 'interrupted';
  calibration = null;
}
function biasWindow() {
  return {start:null, count:0, mean:[0,0,0,0,0,0], m2:[0,0,0,0,0,0], biasMean:[0,0,0],
    accelFirst:[0,0,0], accelLast:[0,0,0], gyroFirst:[0,0,0], gyroLast:[0,0,0],
    firstCount:0, lastCount:0, blockStart:null, blockCount:0, blockSum:[0,0,0]};
}
function beginCalibration() {
  if (!quaternion) return;
  stationaryWindow = null; biasTracking = false;
  calibration = biasWindow(); calibrationState = 'collecting';
}
function observeBias(c, sample, now, delta) {
  const accel = sample.accel_mps2, gyro = sample.gyro_rps;
  // A low-motion interval is a zero-rate hypothesis, not an absolute heading
  // reference. Perfectly steady rotation about gravity remains unobservable.
  const norm = Math.hypot(...accel);
  if (norm < 0.9*GRAVITY || norm > 1.1*GRAVITY || Math.hypot(...gyro) > 0.12 ||
      delta > 100000000n) return false;
  if (c.start === null) c.start = now;
  const elapsed = now-c.start;
  c.count++;
  for (let i=0;i<6;i++) {
    const value = i<3 ? gyro[i] : accel[i-3];
    const difference = value-c.mean[i];
    c.mean[i] += difference/c.count;
    c.m2[i] += difference*(value-c.mean[i]);
  }
  for (let i=0;i<3;i++) c.biasMean[i] += (sample.attitude_gyro_rps[i]-c.biasMean[i])/c.count;
  const first = elapsed < CALIBRATION_NS/2n;
  const sum = first ? c.accelFirst : c.accelLast;
  if (first) c.firstCount++; else c.lastCount++;
  const gyroSum = first ? c.gyroFirst : c.gyroLast;
  for (let i=0;i<3;i++) { sum[i] += accel[i]; gyroSum[i] += gyro[i]; }
  if (elapsed < CALIBRATION_NS) return null;
  const gyroVariance = (c.m2[0]+c.m2[1]+c.m2[2])/c.count;
  const accelVariance = (c.m2[3]+c.m2[4]+c.m2[5])/c.count;
  const tiltChange = Math.hypot(...c.accelFirst.map((value,i) =>
    value/c.firstCount-c.accelLast[i]/c.lastCount));
  const rateChange = Math.hypot(...c.gyroFirst.map((value,i) =>
    value/c.firstCount-c.gyroLast[i]/c.lastCount));
  const accepted = c.count >= 90 && c.firstCount > 0 && c.lastCount > 0 &&
    gyroVariance < 0.05**2 && accelVariance < 0.85**2 && tiltChange < 0.2 && rateChange < 0.01;
  return accepted;
}
function stableBiasBlock(window, sample, now, complete) {
  if (window.blockStart === null) window.blockStart = now;
  window.blockCount++;
  for (let i=0;i<3;i++) window.blockSum[i] += sample.gyro_rps[i]-gyroBias[i];
  if (!complete && now-window.blockStart < 1000000000n) return true;
  // Check short sections too: a late turn must not disappear in a mostly-still
  // whole-window mean. Noise may defer tracking rather than calibrating motion.
  const stable = Math.hypot(...window.blockSum)/window.blockCount <= BIAS_TRACK_RATE;
  window.blockStart = null; window.blockCount = 0; window.blockSum.fill(0);
  return stable;
}
function updateBias(sample, now, delta) {
  const manual = calibration !== null;
  const window = manual ? calibration : (stationaryWindow ??= biasWindow());
  let accepted = observeBias(window, sample, now, delta);
  if (!manual && biasReady && accepted !== false &&
      !stableBiasBlock(window, sample, now, accepted === true)) accepted = false;
  if (accepted === null) return;
  const residual = Math.hypot(...window.biasMean.map((value,i) => value-gyroBias[i]));
  // Once initialized, do not learn sustained measurable turns as bias. Rates
  // below this conservative threshold can still be absorbed as apparent drift.
  const apply = accepted && (manual || !biasReady || residual <= BIAS_TRACK_RATE);
  if (apply) {
    const blend = manual || !biasReady ? 1 :
      1-Math.exp(-Number(now-window.start)/1e9/BIAS_TRACK_SECONDS);
    for (let i=0;i<3;i++) gyroBias[i] += blend*(window.biasMean[i]-gyroBias[i]);
    biasReady = true;
  }
  if (manual) {
    calibration = null; calibrationState = apply ? 'calibrated' : 'rejected';
  } else {
    stationaryWindow = null; biasTracking = apply;
  }
}
function decimal(value) {
  return typeof value === 'string' && /^\d{1,20}$/.test(value) ? BigInt(value) : null;
}
function axes(value) {
  return Array.isArray(value) && value.length === 3 &&
    [0,1,2].every(index => Number.isFinite(value[index]));
}
function gravityUp(accel) {
  const norm = Math.hypot(...accel);
  if (norm < 0.8 * GRAVITY || norm > 1.2 * GRAVITY) return null;
  return accel.map(value => -value / norm);
}
function initialize(up) {
  const q = [1 + up[2], up[1], -up[0], 0];
  const norm = Math.hypot(...q);
  if (norm < 1e-12) return [0, 1, 0, 0];
  return q.map(value => value / norm);
}
function multiply(a, b) {
  const [w,x,y,z] = a, [v,i,j,k] = b;
  return [w*v-x*i-y*j-z*k, w*i+x*v+y*k-z*j,
    w*j-x*k+y*v+z*i, w*k+x*j-y*i+z*v];
}
function integrate(gyro, up, dt) {
  const [w,x,y,z] = quaternion;
  // Mahony proportional correction: measured up crossed with predicted up,
  // both in the device frame. Translation outside the gravity band is ignored.
  let [gx,gy,gz] = gyro.map((value,i) => value-gyroBias[i]);
  if (up) {
    const predicted = [2*(x*z-w*y), 2*(y*z+w*x), 1-2*(x*x+y*y)];
    gx += GRAVITY_GAIN * (up[1]*predicted[2]-up[2]*predicted[1]);
    gy += GRAVITY_GAIN * (up[2]*predicted[0]-up[0]*predicted[2]);
    gz += GRAVITY_GAIN * (up[0]*predicted[1]-up[1]*predicted[0]);
  }
  const rate = Math.hypot(gx, gy, gz);
  if (rate === 0) return;
  const half = rate * dt / 2, scale = Math.sin(half) / rate;
  const next = multiply(quaternion, [Math.cos(half),gx*scale,gy*scale,gz*scale]);
  const norm = Math.hypot(...next);
  quaternion = next.map(value => value / norm);
}
function publish(sample, status, corrected) {
  const [w,x,y,z] = quaternion;
  const degrees = 180 / Math.PI;
  self.postMessage({type:'attitude', epoch:sample.epoch, session_id:session,
    sample_sequence:sample.sample_sequence, quaternion:quaternion.slice(), status,
    accel_correction:corrected,
    gyro_bias_rps:biasReady ? gyroBias.slice() : null,
    calibration_state:calibrationState,
    bias_tracking:biasTracking,
    bias_progress:stationaryWindow && stationaryWindow.start !== null ?
      Math.min(1,Number(timestamp-stationaryWindow.start)/Number(CALIBRATION_NS)) : 0,
    calibration_progress:calibration && calibration.start !== null ?
      Math.min(1,Number(timestamp-calibration.start)/Number(CALIBRATION_NS)) : 0,
    roll_deg:Math.atan2(2*(w*x+y*z),1-2*(x*x+y*y))*degrees,
    pitch_deg:Math.asin(Math.max(-1,Math.min(1,2*(w*y-z*x))))*degrees,
    yaw_deg:Math.atan2(2*(w*z+x*y),1-2*(y*y+z*z))*degrees});
}
function sample(message) {
  const nextSequence = decimal(message.sample_sequence), nextTimestamp = decimal(message.timestamp_ns);
  if (typeof message.session_id !== 'string' || !message.session_id ||
      !axes(message.accel_mps2) || !axes(message.gyro_rps) ||
      !axes(message.attitude_accel_mps2) || !axes(message.attitude_gyro_rps) ||
      nextSequence === null || nextTimestamp === null || nextTimestamp === 0n) {
    stationaryWindow = null; biasTracking = false; cancelCalibration();
    return;
  }
  const changed = session !== message.session_id;
  if (!changed && sequence !== null && nextSequence <= sequence) return;
  const delta = timestamp === null ? 0n : nextTimestamp - timestamp;
  const discontinuity = changed || timestamp === null || delta <= 0n || delta > MAX_DT_NS;
  const up = gravityUp(message.accel_mps2) && gravityUp(message.attitude_accel_mps2);
  const wasInitialized = quaternion !== null;
  if (changed) {
    gyroBias = [0,0,0]; biasReady = false;
    calibration = null; calibrationState = 'uncalibrated';
    stationaryWindow = null; biasTracking = false;
  } else if (discontinuity) {
    cancelCalibration(); stationaryWindow = null; biasTracking = false;
  }
  session = message.session_id; sequence = nextSequence; timestamp = nextTimestamp;
  if (discontinuity) quaternion = null;
  if (quaternion === null) {
    if (!up) return;
    quaternion = initialize(up);
    publish(message, wasInitialized ? 'resync' : 'initial', true);
    return;
  }
  updateBias(message, nextTimestamp, delta);
  // Subtract integer nanoseconds before converting, preserving epoch precision.
  integrate(message.attitude_gyro_rps, up, Number(delta) / 1e9);
  publish(message, 'tracking', up !== null);
}
self.onmessage = event => {
  if (event.data?.type === 'reset') reset();
  else if (event.data?.type === 'calibrate') beginCalibration();
  else if (event.data?.type === 'sample') {
    sample(event.data);
    // Acknowledge rejected samples too so the UI's latest-only mailbox drains.
    self.postMessage({type:'ready', epoch:event.data.epoch});
  }
};
