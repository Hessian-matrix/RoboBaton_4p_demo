#include "panorama_http.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace robobaton_demo {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxClients = 8;
constexpr size_t kMaxRequestBytes = 8192;
constexpr size_t kSendBudget = 64 * 1024;
constexpr auto kReadDeadline = std::chrono::seconds(2);
constexpr auto kSendDeadline = std::chrono::seconds(3);

constexpr std::string_view kHtml = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>四目360°环视</title>
<link rel="stylesheet" href="/style.css">
<script src="/app.js" defer></script>
</head>
<body>
<main>
<header>
<div><p class="eyebrow">ROBOBATON · 全景预览</p><h1>四目360°环视</h1></div>
<div class="actions"><span id="health" role="status" aria-live="polite" data-tone="warn">等待首帧</span>
<button id="pause" type="button" aria-pressed="false">暂停刷新</button></div>
</header>
<p class="intro">四路标定图像 · 等距柱状投影 · 仅供可信局域网只读预览</p>
<p id="error" class="error" role="alert" hidden></p>
<noscript><p class="error">此页面需要启用 JavaScript 才能刷新全景与状态。</p></noscript>
<figure aria-label="360度全景与方位角刻度">
<div class="viewport"><p id="placeholder">尚无全景图像，正在等待服务器首帧。</p>
<img id="panorama" alt="四路相机标定拼接的360度全景；未覆盖区域为黑色" hidden></div>
<div id="azimuth" class="ruler" aria-label="方位角尚未收到"><span>—</span><span>—</span><span>—</span><span>—</span><span>—</span></div>
<figcaption id="projection">正在等待投影范围。图像保持原始宽高比，完整显示，不裁切。</figcaption>
</figure>
<section aria-labelledby="status-title">
<h2 id="status-title">处理状态</h2>
<dl class="metrics">
<div><dt>输出尺寸</dt><dd id="dimensions">—</dd></div>
<div><dt>全景覆盖率</dt><dd id="coverage">—</dd></div>
<div><dt>地平线覆盖率</dt><dd id="horizon">—</dd></div>
<div><dt>单帧处理耗时</dt><dd id="processing">—</dd></div>
<div><dt>已处理帧</dt><dd id="processed">—</dd></div>
<div><dt>丢弃同步组</dt><dd id="dropped">—</dd></div>
<div><dt>投影距离</dt><dd id="distance">—</dd></div>
<div><dt>图像来源</dt><dd id="source">—</dd></div>
</dl>
<dl class="identity"><div><dt>同步组 ID</dt><dd id="group">—</dd></div>
<div><dt>采集时间戳 / ns</dt><dd id="timestamp">—</dd></div>
<div><dt>最近图像更新</dt><dd id="age">尚未收到</dd></div></dl>
</section>
<aside class="caution" aria-labelledby="calibration-title">
<h2 id="calibration-title">标定与成像边界</h2>
<p id="calibration">尚未收到标定验收状态；预览不代表标定精度已通过人工验收。</p>
<p>黑色区域表示相机视场未覆盖，不是补全画面。360°方位角不代表所有仰角都有覆盖。固定投影距离无法消除不同深度物体的视差；近处物体及拼接交界可能出现错位、重影。</p>
</aside>
<footer>自动顺序刷新约每 100 ms 一次；网络或处理变慢时降低刷新频率，不堆积请求。页面不提供录制、控制或参数修改。</footer>
</main>
</body>
</html>
)HTML";

constexpr std::string_view kCss = R"CSS(:root{color-scheme:dark;--bg:#11171c;--surface:#1b242c;--text:#edf2f6;--muted:#aebcc7;--line:#3b4b58;--accent:#79d4c8;--warn:#ffd08a;--error:#ffb7ac}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.6 system-ui,-apple-system,"Noto Sans SC","Microsoft YaHei",sans-serif}
main{width:100%;padding:1rem clamp(.75rem,2vw,2rem) 2rem}header{display:flex;justify-content:space-between;align-items:center;gap:1rem;flex-wrap:wrap}
h1{font-size:clamp(1.5rem,3vw,2rem);line-height:1.3;margin:0}h2{font-size:1rem;margin:0 0 .75rem}.eyebrow{font-size:.75rem;letter-spacing:.12em;color:var(--accent);margin:0 0 .25rem}
.actions{display:flex;align-items:center;gap:.75rem;flex-wrap:wrap}button{font:inherit;min-height:44px;padding:.5rem 1rem;background:var(--surface);color:var(--text);border:1px solid var(--line);border-radius:.25rem;cursor:pointer}
button:hover{border-color:var(--accent)}button:focus-visible{outline:3px solid var(--accent);outline-offset:3px}#health{font-size:.875rem}#health[data-tone=ok]{color:var(--accent)}#health[data-tone=warn]{color:var(--warn)}#health[data-tone=error]{color:var(--error)}
p{margin:.5rem 0}.intro,figcaption,footer{color:var(--muted);font-size:.875rem}.intro{margin:.75rem 0 1rem}.error{padding:.75rem 1rem;border:1px solid var(--error);color:var(--error);background:var(--surface);overflow-wrap:anywhere}
figure{margin:0 0 1.5rem}.viewport{background:#000;border:1px solid var(--line)}#placeholder{padding:2rem 1rem;margin:0;color:var(--muted);text-align:center}#panorama{display:block;width:100%;height:auto}.ruler{display:flex;justify-content:space-between;border-top:1px solid var(--line);color:var(--muted);font-size:.75rem;font-variant-numeric:tabular-nums}.ruler span{position:relative;padding-top:.5rem}.ruler span:before{content:"";position:absolute;top:0;left:50%;height:.375rem;border-left:1px solid var(--line)}.ruler span:first-child:before{left:0}.ruler span:last-child:before{left:auto;right:0}figcaption{margin-top:.75rem}
section{margin-bottom:1.5rem}.metrics{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:1rem;margin:0;padding:1rem;background:var(--surface);border:1px solid var(--line)}dt{color:var(--muted);font-size:.75rem}dd{margin:0;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}.metrics dd{font-size:1.125rem}.identity{display:flex;gap:.75rem 2rem;flex-wrap:wrap;margin:1rem 0 0}.identity div{min-width:0;max-width:100%}.identity dd{font-size:.875rem}
.caution{border-left:3px solid var(--warn);padding:.75rem 1rem;background:var(--surface);font-size:.875rem}.caution h2{color:var(--warn);margin:0 0 .5rem}footer{margin-top:1rem}[hidden]{display:none!important}
@media(max-width:400px){.actions{width:100%;justify-content:space-between}.metrics{padding:.75rem;gap:.75rem;grid-template-columns:repeat(2,minmax(0,1fr))}.metrics dd{font-size:1rem}}
)CSS";

constexpr std::string_view kJs = R"JS('use strict';
(() => {
  const byId = id => document.getElementById(id);
  const health = byId('health'), error = byId('error'), button = byId('pause');
  let panorama = byId('panorama'), activeUrl = null, timer = null, controller = null;
  let paused = false, inFlight = false, closed = false, available = false;
  let lastSignature = null, lastChange = 0, source = '', sequence = 0;
  const text = (id, value) => { byId(id).textContent = value; };
  const report = message => { error.textContent = message; error.hidden = !message; };
  function showHealth() {
    const elapsed = lastChange ? Math.max(0, performance.now() - lastChange) : 0;
    text('age', lastChange ? `${(elapsed / 1000).toFixed(1)} 秒前${source === 'images' ? '（静态图像）' : ''}` : '尚未收到');
    let label = '等待首帧', tone = 'warn';
    if (paused) label = '已暂停 · 保留最近快照';
    else if (!available) { label = lastChange ? '不可用 · 显示最近快照' : '等待图像 / 暂不可用'; tone = 'error'; }
    else if (source === 'images') { label = '离线图像 · 非实时'; tone = 'ok'; }
    else if (elapsed > 2000) label = `图像已过期 · ${(elapsed / 1000).toFixed(1)} 秒未更新`;
    else { label = '实时图像更新中'; tone = 'ok'; }
    health.textContent = label;
    health.dataset.tone = tone;
  }
  function validateStatus(s) {
    if (!s || typeof s !== 'object' || Array.isArray(s)) throw new Error('状态响应不是有效对象。');
    if (s.state === 'waiting') return;
    for (const key of ['group_id', 'timestamp_ns', 'calibration_acceptance', 'source']) {
      if (typeof s[key] !== 'string') throw new Error(`状态字段 ${key} 格式错误。`);
    }
    for (const key of ['width', 'height', 'coverage_percent', 'horizon_coverage_percent', 'processed_frames', 'dropped_groups', 'processing_ms', 'yaw_deg', 'elevation_min_deg', 'elevation_max_deg']) {
      if (typeof s[key] !== 'number' || !Number.isFinite(s[key])) throw new Error(`状态字段 ${key} 格式错误。`);
    }
    if (s.width <= 0 || s.height <= 0 || !['live', 'images'].includes(s.source)) throw new Error('图像尺寸或来源无效。');
    if (s.projection_distance_m !== null && (typeof s.projection_distance_m !== 'number' || !Number.isFinite(s.projection_distance_m) || s.projection_distance_m < 0)) throw new Error('投影距离格式错误。');
  }
  function renderStatus(s) {
    text('dimensions', `${s.width} × ${s.height}`);
    text('coverage', `${s.coverage_percent.toFixed(1)} %`);
    text('horizon', `${s.horizon_coverage_percent.toFixed(1)} %`);
    text('processing', `${s.processing_ms.toFixed(1)} ms`);
    text('processed', String(s.processed_frames)); text('dropped', String(s.dropped_groups));
    text('distance', s.projection_distance_m > 0 ? `${s.projection_distance_m.toFixed(2)} m（有限距离）` : '无穷远');
    text('source', s.source === 'images' ? '离线图像' : '相机实时采集');
    text('group', s.group_id); text('timestamp', s.timestamp_ns);
    const centre = ((s.yaw_deg % 360) + 540) % 360 - 180;
    const ruler = byId('azimuth');
    const angles = [-180, -90, 0, 90, 180].map(offset => `${Number((centre + offset).toFixed(1))}°`);
    [...ruler.children].forEach((tick, i) => { tick.textContent = angles[i]; });
    ruler.setAttribute('aria-label', `IMU 方位角从 ${angles[0]} 到 ${angles[4]}，中心 ${angles[2]}`);
    text('projection', `中心方位角 ${angles[2]}；0° = IMU +X，正方向朝 +Y。上沿仰角 ${s.elevation_max_deg}°，下沿 ${s.elevation_min_deg}°，正方向为 +Z。图像保持原始宽高比，完整显示，不裁切。`);
    const pending = /pending[-_ ]human[-_ ]review/i.test(s.calibration_acceptance);
    text('calibration', pending ? '标定待人工复核（pending-human-review）：请检查接缝与几何对齐，勿将本预览视为精度验收结论。' : `标定验收状态：${s.calibration_acceptance}。本页面仅显示调用方状态，不代替人工精度验收。`);
  }
  async function request(path, signal) {
    const response = await fetch(`${path}?v=${++sequence}`, {cache:'no-store', credentials:'omit', redirect:'error', signal});
    if (!response.ok) throw new Error(`${path === '/frame.jpg' ? '全景图像' : '处理状态'}暂不可用（HTTP ${response.status}）。`);
    return response;
  }
  async function decode(url, signal) {
    const image = new Image();
    image.id = 'panorama'; image.alt = '四路相机标定拼接的360度全景；未覆盖区域为黑色'; image.decoding = 'async';
    await new Promise((resolve, reject) => {
      const finish = failure => {
        clearTimeout(deadline); image.onload = image.onerror = null;
        signal.removeEventListener('abort', abort);
        if (failure) { image.removeAttribute('src'); reject(failure); } else resolve();
      };
      const abort = () => finish(new Error('刷新已取消或超时。'));
      const deadline = setTimeout(() => finish(new Error('全景 JPEG 解码超时。')), 3000);
      image.onload = () => image.decode().then(() => finish(null), () => finish(new Error('无法解码服务器返回的全景 JPEG。')));
      image.onerror = () => finish(new Error('无法解码服务器返回的全景 JPEG。'));
      signal.addEventListener('abort', abort, {once:true});
      if (signal.aborted) abort(); else image.src = url;
    });
    return image;
  }
  async function tick() {
    if (paused || closed || inFlight) return;
    inFlight = true;
    const started = performance.now();
    controller = new AbortController();
    const signal = controller.signal;
    const deadline = setTimeout(() => controller.abort(), 3000);
    let candidateUrl = null;
    try {
      const status = await (await request('/status.json', signal)).json();
      validateStatus(status);
      if (status.state === 'waiting') {
        available = false; report('服务器正在等待首帧；请检查采集与全景处理是否已启动。');
      } else {
        renderStatus(status);
        const frame = await request('/frame.jpg', signal);
        candidateUrl = URL.createObjectURL(await frame.blob());
        const image = await decode(candidateUrl, signal);
        if (paused || closed || signal.aborted) return;
        panorama.replaceWith(image); panorama = image;
        const previousUrl = activeUrl; activeUrl = candidateUrl; candidateUrl = null;
        if (previousUrl) URL.revokeObjectURL(previousUrl);
        byId('placeholder').hidden = true;
        const signature = `${status.group_id}/${status.timestamp_ns}/${status.processed_frames}`;
        if (signature !== lastSignature) { lastSignature = signature; lastChange = performance.now(); }
        source = status.source; available = true; report('');
      }
    } catch (failure) {
      if (!paused && !closed) {
        available = false;
        report(signal.aborted ? '刷新超时：连接或图像处理未及时响应；页面会自动重试。' : `刷新失败：${failure.message || '未知错误'} 页面会自动重试。`);
      }
    } finally {
      clearTimeout(deadline);
      if (candidateUrl) URL.revokeObjectURL(candidateUrl);
      controller = null; inFlight = false; showHealth();
      if (!paused && !closed) timer = setTimeout(tick, Math.max(0, 100 - (performance.now() - started)));
    }
  }
  button.addEventListener('click', () => {
    paused = !paused; button.setAttribute('aria-pressed', String(paused));
    button.textContent = paused ? '继续刷新' : '暂停刷新';
    clearTimeout(timer);
    if (paused && controller) controller.abort();
    if (!paused && !inFlight) timer = setTimeout(tick, 0);
    showHealth();
  });
  let ageTimer = setInterval(showHealth, 250);
  window.addEventListener('pagehide', event => {
    closed = true; clearTimeout(timer); clearInterval(ageTimer);
    if (controller) controller.abort();
    if (!event.persisted && activeUrl) URL.revokeObjectURL(activeUrl);
  });
  window.addEventListener('pageshow', event => {
    if (!event.persisted) return;
    closed = false; ageTimer = setInterval(showHealth, 250);
    if (!paused && !inFlight) timer = setTimeout(tick, 0);
    showHealth();
  });
  tick();
})();
)JS";

class Fd final {
 public:
  explicit Fd(int value = -1) noexcept : value_(value) {}
  ~Fd() { Reset(); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd(Fd&& other) noexcept : value_(other.Release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) Reset(other.Release());
    return *this;
  }
  int Get() const noexcept { return value_; }
  int Release() noexcept { return std::exchange(value_, -1); }
  void Reset(int value = -1) noexcept {
    if (value_ >= 0) ::close(value_);
    value_ = value;
  }
 private:
  int value_;
};

[[noreturn]] void ThrowSocketError(const char* operation) {
  const int error = errno;
  throw std::system_error(error, std::generic_category(),
                          std::string("panorama HTTP: ") + operation);
}

bool IsToken(unsigned char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') ||
         (c != 0 && std::strchr("!#$%&'*+-.^_`|~", c) != nullptr);
}

bool EqualHeader(std::string_view name, std::string_view lowercase) noexcept {
  if (name.size() != lowercase.size()) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    if (c != lowercase[i]) return false;
  }
  return true;
}

struct Client {
  Fd fd;
  std::array<char, kMaxRequestBytes> request{};
  size_t request_bytes = 0;
  std::string header;
  size_t header_sent = 0;
  const char* body = nullptr;
  size_t body_bytes = 0;
  size_t body_sent = 0;
  std::shared_ptr<const std::vector<uint8_t>> jpeg;
  std::shared_ptr<const std::string> status;
  Clock::time_point deadline;
  bool writing = false;

  void Close() noexcept {
    fd.Reset(); jpeg.reset(); status.reset(); header.clear();
    request_bytes = 0; header_sent = 0; body = nullptr;
    body_bytes = 0; body_sent = 0; writing = false;
  }

  void Respond(std::string_view code, std::string_view type,
               std::string_view content, bool allow_get = false) {
    body = content.data(); body_bytes = content.size();
    header = "HTTP/1.1 "; header.append(code);
    header += "\r\nContent-Type: "; header.append(type);
    header += "\r\nContent-Length: "; header += std::to_string(body_bytes);
    header += "\r\nConnection: close\r\nCache-Control: no-store\r\n"
              "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
              "Referrer-Policy: no-referrer\r\n"
              "Content-Security-Policy: default-src 'none'; script-src 'self'; "
              "style-src 'self'; img-src 'self' blob:; connect-src 'self'; "
              "base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n";
    if (allow_get) header += "Allow: GET\r\n";
    header += "\r\n";
    writing = true;
    deadline = Clock::now() + kSendDeadline;
  }

  void BadRequest() {
    Respond("400 Bad Request", "text/plain; charset=utf-8", "Invalid HTTP request.\n");
  }
};

// Parse one bounded request and close after its response; request bodies and
// transfer coding are deliberately outside this read-only protocol.
bool ParseRequest(Client& client, size_t header_end, std::string_view& target) {
  const std::string_view request(client.request.data(), header_end + 2);
  const size_t line_end = request.find("\r\n");
  const auto line = request.substr(0, line_end);
  const size_t first = line.find(' ');
  const size_t second = first == std::string_view::npos ? first : line.find(' ', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      first == 0 || second == first + 1) {
    client.BadRequest(); return false;
  }
  const auto method = line.substr(0, first);
  target = line.substr(first + 1, second - first - 1);
  const auto version = line.substr(second + 1);
  if ((version != "HTTP/1.1" && version != "HTTP/1.0") || target.front() != '/' ||
      !std::all_of(method.begin(), method.end(), IsToken) ||
      std::any_of(target.begin(), target.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
    client.BadRequest(); return false;
  }
  bool host_seen = false;
  bool length_seen = false;
  size_t cursor = line_end + 2;
  while (cursor < request.size()) {
    const size_t end = request.find("\r\n", cursor);
    if (end == std::string_view::npos) { client.BadRequest(); return false; }
    const auto field = request.substr(cursor, end - cursor);
    const size_t colon = field.find(':');
    if (colon == 0 || colon == std::string_view::npos ||
        !std::all_of(field.begin(), field.begin() + colon, IsToken)) {
      client.BadRequest(); return false;
    }
    const auto name = field.substr(0, colon);
    auto value = field.substr(colon + 1);
    if (std::any_of(value.begin(), value.end(), [](unsigned char c) {
          return (c < 32 && c != '\t') || c == 127;
        })) { client.BadRequest(); return false; }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (EqualHeader(name, "host")) {
      if (host_seen || value.empty()) { client.BadRequest(); return false; }
      host_seen = true;
    } else if (EqualHeader(name, "content-length")) {
      if (length_seen || value != "0") { client.BadRequest(); return false; }
      length_seen = true;
    } else if (EqualHeader(name, "transfer-encoding") || EqualHeader(name, "expect")) {
      client.BadRequest(); return false;
    }
    cursor = end + 2;
  }
  if ((version == "HTTP/1.1" && !host_seen) || client.request_bytes != header_end + 4) {
    client.BadRequest(); return false;
  }
  if (method != "GET") {
    client.Respond("405 Method Not Allowed", "text/plain; charset=utf-8", "Only GET is supported.\n", true);
    return false;
  }
  target = target.substr(0, target.find('?'));
  return true;
}
}  // namespace

class PanoramaHttpServer::Impl final {
 public:
  Fd listener;
  Fd wake;
  std::thread worker;
  std::atomic<bool> stopping{true};
  std::mutex latest_mutex;
  std::shared_ptr<const std::vector<uint8_t>> latest_jpeg;
  std::shared_ptr<const std::string> latest_status =
      std::make_shared<const std::string>("{\"state\":\"waiting\"}");

  void Route(Client& client, size_t header_end) {
    std::string_view target;
    if (!ParseRequest(client, header_end, target)) return;
    if (target == "/") {
      client.Respond("200 OK", "text/html; charset=utf-8", kHtml);
    } else if (target == "/app.js") {
      client.Respond("200 OK", "text/javascript; charset=utf-8", kJs);
    } else if (target == "/style.css") {
      client.Respond("200 OK", "text/css; charset=utf-8", kCss);
    } else if (target == "/favicon.ico") {
      client.Respond("204 No Content", "image/x-icon", {});
    } else if (target == "/frame.jpg" || target == "/status.json") {
      {
        std::lock_guard<std::mutex> lock(latest_mutex);
        if (target == "/frame.jpg") client.jpeg = latest_jpeg;
        else client.status = latest_status;
      }
      if (target == "/status.json") {
        client.Respond("200 OK", "application/json; charset=utf-8", *client.status);
      } else if (client.jpeg && !client.jpeg->empty()) {
        client.Respond("200 OK", "image/jpeg",
                       {reinterpret_cast<const char*>(client.jpeg->data()), client.jpeg->size()});
      } else {
        client.Respond("503 Service Unavailable", "text/plain; charset=utf-8", "No panorama frame is available.\n");
      }
    } else {
      client.Respond("404 Not Found", "text/plain; charset=utf-8", "Not found.\n");
    }
  }

  void ReadRequest(Client& client) {
    for (int attempt = 0; attempt < 4; ++attempt) {
      const ssize_t count = ::recv(client.fd.Get(), client.request.data() + client.request_bytes,
                                   client.request.size() - client.request_bytes, MSG_DONTWAIT);
      if (count < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) client.Close();
        return;
      }
      if (count == 0) { client.Close(); return; }
      client.request_bytes += static_cast<size_t>(count);
      const size_t end = std::string_view(client.request.data(), client.request_bytes).find("\r\n\r\n");
      if (end != std::string_view::npos) { Route(client, end); return; }
      if (client.request_bytes == client.request.size()) {
        client.Respond("431 Request Header Fields Too Large", "text/plain; charset=utf-8", "HTTP headers exceed 8192 bytes.\n");
        return;
      }
    }
  }

  static void WriteResponse(Client& client) noexcept {
    size_t budget = kSendBudget;
    for (int attempt = 0; attempt < 8 && budget != 0; ++attempt) {
      const bool header = client.header_sent < client.header.size();
      const char* data = header ? client.header.data() : client.body;
      size_t& sent = header ? client.header_sent : client.body_sent;
      const size_t total = header ? client.header.size() : client.body_bytes;
      if (sent == total) { client.Close(); return; }
      const ssize_t count = ::send(client.fd.Get(), data + sent,
                                   std::min(total - sent, budget), MSG_DONTWAIT | MSG_NOSIGNAL);
      if (count < 0) {
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) client.Close();
        return;
      }
      if (count == 0) { client.Close(); return; }
      sent += static_cast<size_t>(count);
      budget -= static_cast<size_t>(count);
      if (client.header_sent == client.header.size() && client.body_sent == client.body_bytes) {
        client.Close(); return;
      }
    }
  }

  void Run() noexcept {
    try {
      // Only this thread owns accepted descriptors. Stop wakes it and joins before
      // closing listener/wake, so descriptor numbers cannot be reused underneath poll.
      std::array<Client, kMaxClients> clients;
      while (!stopping.load(std::memory_order_acquire)) {
        std::array<pollfd, kMaxClients + 2> pending{};
        pending[0] = {wake.Get(), POLLIN, 0};
        const bool room = std::any_of(clients.begin(), clients.end(), [](const Client& c) { return c.fd.Get() < 0; });
        pending[1] = {room ? listener.Get() : -1, POLLIN, 0};
        for (size_t i = 0; i < clients.size(); ++i) {
          pending[i + 2] = {clients[i].fd.Get(), static_cast<short>(clients[i].writing ? POLLOUT : POLLIN), 0};
        }
        const int ready = ::poll(pending.data(), pending.size(), 50);
        if (stopping.load(std::memory_order_acquire)) break;
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (pending[0].revents || (pending[1].revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        const auto now = Clock::now();
        for (size_t i = 0; i < clients.size(); ++i) {
          auto& client = clients[i];
          if (client.fd.Get() < 0) continue;
          const short events = pending[i + 2].revents;
          if (now >= client.deadline || (events & (POLLERR | POLLNVAL)) ||
              ((events & POLLHUP) && !(events & (POLLIN | POLLOUT)))) {
            client.Close(); continue;
          }
          try {
            if (!client.writing && (events & POLLIN)) ReadRequest(client);
            if (client.fd.Get() >= 0 && client.writing && (events & POLLOUT)) WriteResponse(client);
          } catch (...) { client.Close(); }
        }
        // Both accept work and per-client I/O are bounded each poll turn.
        if (pending[1].revents & POLLIN) {
          for (auto& client : clients) {
            if (stopping.load(std::memory_order_acquire)) break;
            if (client.fd.Get() >= 0) continue;
            const int fd = ::accept4(listener.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) break;
            client.fd.Reset(fd);
            client.deadline = Clock::now() + kReadDeadline;
          }
        }
      }
    } catch (...) {}
    stopping.store(true, std::memory_order_release);
    // Fatal loop failure is observable as an unavailable endpoint, not a hung listener.
    (void)::shutdown(listener.Get(), SHUT_RDWR);
  }
};

PanoramaHttpServer::PanoramaHttpServer() : impl_(std::make_unique<Impl>()) {}
PanoramaHttpServer::~PanoramaHttpServer() { Stop(); }

void PanoramaHttpServer::Start(const std::string& numeric_ipv4, uint16_t port) {
  if (impl_->worker.joinable()) throw std::logic_error("panorama HTTP: already started; Stop before restarting");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (numeric_ipv4.find('\0') != std::string::npos ||
      ::inet_pton(AF_INET, numeric_ipv4.c_str(), &address.sin_addr) != 1) {
    throw std::invalid_argument("panorama HTTP: bind address must be numeric IPv4, not a hostname");
  }
  Fd listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (listener.Get() < 0) ThrowSocketError("cannot create IPv4 listening socket");
  const int reuse_address = 1;
  if (::setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
    ThrowSocketError("cannot enable IPv4 address reuse after shutdown");
  }
  if (::bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    ThrowSocketError("cannot bind requested IPv4/port; check address, permissions, and whether the port is occupied");
  }
  if (::listen(listener.Get(), static_cast<int>(kMaxClients)) < 0) ThrowSocketError("cannot listen on bound socket");
  Fd wake(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (wake.Get() < 0) ThrowSocketError("cannot create shutdown wake descriptor");
  impl_->listener = std::move(listener);
  impl_->wake = std::move(wake);
  impl_->stopping.store(false, std::memory_order_release);
  try {
    impl_->worker = std::thread([this] { impl_->Run(); });
  } catch (const std::exception& error) {
    impl_->stopping.store(true, std::memory_order_release);
    impl_->listener.Reset(); impl_->wake.Reset();
    throw std::runtime_error(std::string("panorama HTTP: cannot start worker thread: ") + error.what());
  }
}

void PanoramaHttpServer::Publish(std::shared_ptr<const std::vector<uint8_t>> jpeg,
                                 std::string status_json) {
  auto status = std::make_shared<const std::string>(std::move(status_json));
  {
    std::lock_guard<std::mutex> lock(impl_->latest_mutex);
    impl_->latest_jpeg.swap(jpeg);
    impl_->latest_status.swap(status);
  }
  // Old snapshots are released outside the critical section. HTTP retains at most
  // one immutable snapshot per client and never holds this lock during socket I/O.
}

void PanoramaHttpServer::Stop() noexcept {
  impl_->stopping.store(true, std::memory_order_release);
  if (impl_->worker.joinable()) {
    const uint64_t signal = 1;
    ssize_t written;
    do { written = ::write(impl_->wake.Get(), &signal, sizeof(signal)); }
    while (written < 0 && errno == EINTR);
    impl_->worker.join();
  }
  impl_->listener.Reset(); impl_->wake.Reset();
}

}  // namespace robobaton_demo
