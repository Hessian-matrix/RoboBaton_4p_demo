#include "panorama_stitcher.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <yaml.h>

namespace robobaton_demo {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRadians = kPi / 180.0;

class CalibrationDocument {
 public:
  explicit CalibrationDocument(const std::string& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 1024 * 1024)
      throw std::runtime_error("calibration must be a readable YAML file of 1..1048576 bytes");
    const size_t size = static_cast<size_t>(input.tellg());
    std::string text(size, '\0');
    input.seekg(0);
    if (!input.read(text.data(), static_cast<std::streamsize>(size)))
      throw std::runtime_error("cannot read calibration YAML");
    yaml_parser_t parser{};
    if (!yaml_parser_initialize(&parser)) throw std::bad_alloc();
    yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    const bool ok = yaml_parser_load(&parser, &document_) != 0;
    std::string error = ok ? "" : (parser.problem ? parser.problem : "unknown YAML error");
    yaml_parser_delete(&parser);
    if (!ok) throw std::runtime_error("calibration YAML: " + error);
  }
  ~CalibrationDocument() { yaml_document_delete(&document_); }
  CalibrationDocument(const CalibrationDocument&) = delete;
  CalibrationDocument& operator=(const CalibrationDocument&) = delete;
  yaml_node_t* Root() { return yaml_document_get_root_node(&document_); }
  yaml_node_t* Node(int id) { return yaml_document_get_node(&document_, id); }
  std::string Text(yaml_node_t* node) {
    if (!node || node->type != YAML_SCALAR_NODE) throw std::runtime_error("expected calibration scalar");
    return std::string(reinterpret_cast<char*>(node->data.scalar.value), node->data.scalar.length);
  }
  yaml_node_t* Field(yaml_node_t* map, const std::string& key) {
    if (!map || map->type != YAML_MAPPING_NODE) throw std::runtime_error("expected calibration mapping: " + key);
    yaml_node_t* result = nullptr;
    for (auto* pair = map->data.mapping.pairs.start; pair != map->data.mapping.pairs.top; ++pair) {
      if (Text(Node(pair->key)) == key) {
        if (result) throw std::runtime_error("duplicate calibration key: " + key);
        result = Node(pair->value);
      }
    }
    if (!result) throw std::runtime_error("missing calibration key: " + key);
    return result;
  }
  double Number(yaml_node_t* node) {
    const auto text = Text(node);
    size_t end = 0;
    const double value = std::stod(text, &end);
    if (end != text.size() || !std::isfinite(value)) throw std::runtime_error("invalid finite calibration number");
    return value;
  }
  std::vector<yaml_node_t*> Sequence(yaml_node_t* seq, size_t count) {
    if (!seq || seq->type != YAML_SEQUENCE_NODE ||
        static_cast<size_t>(seq->data.sequence.items.top - seq->data.sequence.items.start) != count)
      throw std::runtime_error("invalid calibration sequence size, expected " + std::to_string(count));
    std::vector<yaml_node_t*> result;
    result.reserve(count);
    for (auto* item = seq->data.sequence.items.start; item != seq->data.sequence.items.top; ++item)
      result.push_back(Node(*item));
    return result;
  }
 private:
  yaml_document_t document_{};
};
uint32_t Dimension(double n) {
  if (n < 2 || n > 8192 || n != std::floor(n) || static_cast<uint32_t>(n) % 2)
    throw std::runtime_error("calibration dimensions must be even integers in 2..8192");
  return static_cast<uint32_t>(n);
}
size_t CameraId(const std::string& id) {
  if (id.size() != 4 || id.compare(0, 3, "cam") || id[3] < '0' || id[3] > '3')
    throw std::runtime_error("expected camera id cam0..cam3");
  return static_cast<size_t>(id[3] - '0');
}
double Distort(double theta, const PanoramaCamera& c) {
  const double t2 = theta * theta;
  return theta * (1 + t2 * (c.distortion[0] + t2 * (c.distortion[1] +
         t2 * (c.distortion[2] + t2 * c.distortion[3]))));
}
bool Project(const std::array<double, 3>& direction, const PanoramaCamera& c,
             const PanoramaProjection& p, double* u, double* v, double* weight) {
  std::array<double, 3> ray{};
  for (size_t row = 0; row < 3; ++row) {
    ray[row] = c.rotation[row * 3] * direction[0] + c.rotation[row * 3 + 1] * direction[1] +
               c.rotation[row * 3 + 2] * direction[2];
    if (p.distance_m > 0) ray[row] += c.translation_m[row] / p.distance_m;
  }
  const double radial = std::hypot(ray[0], ray[1]);
  if (radial < 1e-12 && ray[2] <= 0) return false;
  const double theta = std::atan2(radial, ray[2]);
  if (theta >= p.max_theta_deg * kRadians) return false;
  const double scale = radial > 1e-12 ? Distort(theta, c) / radial : 1.0;
  *u = c.fx * ray[0] * scale + c.cx;
  *v = c.fy * ray[1] * scale + c.cy;
  if (!std::isfinite(*u) || !std::isfinite(*v) || *u < 0 || *v < 0 ||
      *u >= c.width - 1.0 || *v >= c.height - 1.0) return false;
  const double edge = std::min({*u, *v, c.width - 1.0 - *u, c.height - 1.0 - *v});
  // Prefer the optical axis and interior source pixels, without a fixed camera order.
  const double margin = std::min(edge / (0.08 * std::min(c.width, c.height)),
                                 (p.max_theta_deg * kRadians - theta) / (10 * kRadians));
  *weight = std::pow(std::clamp(margin, 0.001, 1.0), 2) *
            std::pow(std::max(0.05, std::cos(theta * 0.8)), 2);
  return true;
}
float Bilinear(const uint8_t* base, uint32_t stride, uint32_t xq, uint32_t yq,
               uint32_t step, uint32_t component) {
  const uint32_t x = xq >> 8, y = yq >> 8;
  const uint32_t fx = xq & 255, fy = yq & 255;
  const auto* a = base + static_cast<size_t>(y) * stride + x * step + component;
  const auto* b = a + stride;
  return static_cast<float>((a[0] * (256 - fx) + a[step] * fx) * (256 - fy) +
                            (b[0] * (256 - fx) + b[step] * fx) * fy) / 65536.0f;
}
std::array<float, 3> Pixel(const PanoramaImage& im, uint32_t xq, uint32_t yq) {
  if (im.rgb) return {Bilinear(im.rgb, im.stride, xq, yq, 3, 0),
                      Bilinear(im.rgb, im.stride, xq, yq, 3, 1),
                      Bilinear(im.rgb, im.stride, xq, yq, 3, 2)};
  const float y = Bilinear(im.y, im.stride, xq, yq, 1, 0) - 16;
  const auto uq = std::min(xq / 2, (im.width / 2 - 1) * 256 - 1);
  const auto vq = std::min(yq / 2, (im.height / 2 - 1) * 256 - 1);
  const float u = Bilinear(im.uv, im.stride, uq, vq, 2, 0) - 128;
  const float v = Bilinear(im.uv, im.stride, uq, vq, 2, 1) - 128;
  return {std::clamp(1.164f*y + 1.596f*v, 0.0f, 255.0f),
          std::clamp(1.164f*y - 0.392f*u - 0.813f*v, 0.0f, 255.0f),
          std::clamp(1.164f*y + 2.017f*u, 0.0f, 255.0f)};
}
uint8_t Byte(float value) { return static_cast<uint8_t>(std::clamp(value + 0.5f, 0.0f, 255.0f)); }
float Luma(const std::array<float, 3>& color) {
  return (color[0] + 2 * color[1] + color[2]) * 0.25f;
}
bool Comparable(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  const float la = Luma(a), lb = Luma(b);
  if (la < 16 || lb < 16 || la > 240 || lb > 240) return false;
  for (size_t ch = 0; ch < 3; ++ch)
    if (a[ch] > 248 || b[ch] > 248 || std::abs(a[ch] / la - b[ch] / lb) > 0.25f) return false;
  return true;
}
}  // namespace

PanoramaCalibration LoadPanoramaCalibration(const std::string& path) {
  CalibrationDocument d(path);
  auto* root = d.Root();
  if (d.Number(d.Field(root, "schema_version")) != 7)
    throw std::runtime_error("panorama requires calibration schema_version 7");
  PanoramaCalibration result;
  result.acceptance = d.Text(d.Field(root, "acceptance"));
  if (result.acceptance.empty() || result.acceptance.size() > 96 ||
      result.acceptance.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ") != std::string::npos)
    throw std::runtime_error("invalid calibration acceptance label");
  std::array<bool, 4> seen{};
  for (auto* camera : d.Sequence(d.Field(root, "cameras"), 4)) {
    const auto id = CameraId(d.Text(d.Field(camera, "id")));
    if (seen[id]) throw std::runtime_error("duplicate camera intrinsics");
    seen[id] = true;
    auto& c = result.cameras[id];
    c.width = Dimension(d.Number(d.Field(camera, "image_width")));
    c.height = Dimension(d.Number(d.Field(camera, "image_height")));
    auto* model = d.Field(camera, "parameters");
    if (d.Text(d.Field(model, "model")) != "kannala-brandt4")
      throw std::runtime_error("only kannala-brandt4 calibration is supported");
    auto* intrinsics = d.Field(model, "parameters");
    c.fx = d.Number(d.Field(intrinsics, "fx")); c.fy = d.Number(d.Field(intrinsics, "fy"));
    c.cx = d.Number(d.Field(intrinsics, "cx")); c.cy = d.Number(d.Field(intrinsics, "cy"));
    if (c.fx <= 0 || c.fy <= 0 || c.cx < 0 || c.cy < 0 || c.cx >= c.width || c.cy >= c.height)
      throw std::runtime_error("invalid camera intrinsics");
    for (size_t i = 0; i < 4; ++i) c.distortion[i] = d.Number(d.Field(intrinsics, "k" + std::to_string(i + 1)));
  }
  seen.fill(false);
  auto* inertial = d.Field(root, "inertial");
  for (auto* extrinsic : d.Sequence(d.Field(inertial, "camera_from_imu"), 4)) {
    const auto direction = d.Text(d.Field(extrinsic, "direction"));
    if (direction.size() != 15 || direction.compare(0, 2, "T_") || direction.substr(6) != "_from_imu")
      throw std::runtime_error("expected extrinsic direction T_camN_from_imu");
    const auto id = CameraId(direction.substr(2, 4));
    if (seen[id]) throw std::runtime_error("duplicate camera extrinsics");
    seen[id] = true;
    auto& c = result.cameras[id];
    auto* pose = d.Field(extrinsic, "pose");
    auto translation = d.Sequence(d.Field(pose, "translation_m"), 3);
    for (size_t i = 0; i < 3; ++i) c.translation_m[i] = d.Number(translation[i]);
    auto quaternion = d.Sequence(d.Field(pose, "quaternion_wxyz"), 4);
    double w = d.Number(quaternion[0]), x = d.Number(quaternion[1]);
    double y = d.Number(quaternion[2]), z = d.Number(quaternion[3]);
    const double norm = std::sqrt(w*w + x*x + y*y + z*z);
    if (std::abs(norm - 1.0) > 0.02) throw std::runtime_error("extrinsic quaternion is not unit wxyz");
    w /= norm; x /= norm; y /= norm; z /= norm;
    c.rotation = {1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w),
                  2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w),
                  2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)};
  }
  return result;
}

// Jobs write disjoint pixels: static owners exclude adaptive bands, and every
// adaptive pixel belongs to exactly one seam. All workers finish within Render.
struct PanoramaStitcher::Workers {
  explicit Workers(PanoramaStitcher& owner) : owner(owner) {
    const unsigned count = std::min(3U, std::max(1U, std::thread::hardware_concurrency()) - 1);
    threads.reserve(count);
    try {
      for (unsigned i = 0; i < count; ++i) threads.emplace_back([this] { Work(); });
    } catch (...) { Stop(); throw; }
  }
  ~Workers() { Stop(); }
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    ready.notify_all();
    for (auto& thread : threads) thread.join();
  }
  void RunJobs() {
    for (;;) {
      const size_t job = next.fetch_add(1, std::memory_order_relaxed);
      if (job >= 4 + owner.seams_.size()) break;
      owner.RenderJob(job, *images);
    }
  }
  void Work() {
    uint64_t observed = 0;
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      ready.wait(lock, [&] { return stopping || generation != observed; });
      if (stopping) return;
      observed = generation;
      lock.unlock();
      RunJobs();
      lock.lock();
      if (--pending == 0) done.notify_one();
    }
  }
  void Render(const std::array<PanoramaImage, 4>& input) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      images = &input;
      next.store(0, std::memory_order_relaxed);
      pending = threads.size();
      ++generation;
    }
    ready.notify_all();
    RunJobs();
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [&] { return pending == 0; });
    images = nullptr;
  }
  PanoramaStitcher& owner;
  std::vector<std::thread> threads;
  std::mutex mutex;
  std::condition_variable ready, done;
  std::atomic<size_t> next{0};
  const std::array<PanoramaImage, 4>* images = nullptr;
  size_t pending = 0;
  uint64_t generation = 0;
  bool stopping = false;
};

PanoramaStitcher::~PanoramaStitcher() = default;

PanoramaStitcher::PanoramaStitcher(const PanoramaCalibration& calibration, const PanoramaProjection& p)
    : calibration_(calibration), projection_(p) {
  if (p.width < 64 || p.width > 4096 || p.height < 32 || p.height > 2048 ||
      static_cast<uint64_t>(p.width) * p.height > 4194304 ||
      !std::isfinite(p.elevation_min_deg) || !std::isfinite(p.elevation_max_deg) ||
      p.elevation_min_deg < -90 || p.elevation_max_deg > 90 ||
      p.elevation_min_deg >= p.elevation_max_deg || !std::isfinite(p.yaw_deg) ||
      !std::isfinite(p.distance_m) || p.distance_m < 0 ||
      (p.distance_m > 0 && p.distance_m < 0.1) ||
      !std::isfinite(p.max_theta_deg) || p.max_theta_deg < 1 || p.max_theta_deg > 120)
    throw std::runtime_error("invalid panorama projection limits");
  for (const auto& c : calibration.cameras) {
    double previous = 0;
    for (int i = 1; i <= 1024; ++i) {
      const double value = Distort(p.max_theta_deg * kRadians * i / 1024.0, c);
      if (!std::isfinite(value) || value <= previous)
        throw std::runtime_error("KB4 polynomial folds inside requested FOV");
      previous = value;
    }
  }
  const size_t pixels = static_cast<size_t>(p.width) * p.height;
  std::vector<double> weights(pixels, 0.0);
  std::vector<uint8_t> owner(pixels, 4);
  for (uint32_t y = 0; y < p.height; ++y) {
    const double elevation = (p.elevation_max_deg - (y + 0.5) / p.height *
                             (p.elevation_max_deg - p.elevation_min_deg)) * kRadians;
    for (uint32_t x = 0; x < p.width; ++x) {
      const double azimuth = ((x + 0.5) / p.width * 360.0 - 180.0 + p.yaw_deg) * kRadians;
      const std::array<double, 3> ray{std::cos(elevation) * std::cos(azimuth),
                                    std::cos(elevation) * std::sin(azimuth), std::sin(elevation)};
      const auto output = y * p.width + x;
      for (size_t cam = 0; cam < 4; ++cam) {
        double u, v, weight;
        if (!Project(ray, calibration.cameras[cam], p, &u, &v, &weight)) continue;
        maps_[cam].push_back({output, static_cast<uint32_t>(u * 256), static_cast<uint32_t>(v * 256), static_cast<float>(weight)});
        if (weight > weights[output]) {
          weights[output] = weight;
          owner[output] = static_cast<uint8_t>(cam);
        }
      }
    }
  }
  coverage_percent_ = 100.0 * std::count_if(weights.begin(), weights.end(), [](double w) { return w > 0; }) / pixels;
  // Locate ordered camera boundaries on the periodic image, not at fixed azimuths.
  std::array<double, 16> boundary_sin{}, boundary_cos{};
  std::array<size_t, 16> boundary_count{};
  for (uint32_t y = 0; y < p.height; ++y) for (uint32_t x = 0; x < p.width; ++x) {
    const auto left = owner[y * p.width + (x + p.width - 1) % p.width];
    const auto right = owner[y * p.width + x];
    if (left >= 4 || right >= 4 || left == right) continue;
    const auto pair = left * 4 + right;
    boundary_sin[pair] += std::sin(x * 2 * kPi / p.width);
    boundary_cos[pair] += std::cos(x * 2 * kPi / p.width);
    ++boundary_count[pair];
  }
  for (size_t pair = 0; pair < 16; ++pair) {
    if (!boundary_count[pair] || std::hypot(boundary_sin[pair], boundary_cos[pair]) <
        0.5 * boundary_count[pair]) continue;
    const int centre = static_cast<int>(std::lround(std::atan2(boundary_sin[pair], boundary_cos[pair]) *
                                                  p.width / (2 * kPi)));
    Seam seam;
    seam.left = static_cast<uint8_t>(pair / 4);
    seam.right = static_cast<uint8_t>(pair % 4);
    const uint32_t half = std::max(4U, p.width / 10);
    seam.start = (centre - static_cast<int>(half) + 2 * static_cast<int>(p.width)) % p.width;
    seam.width = half * 2;
    const size_t count = static_cast<size_t>(seam.width) * p.height;
    for (size_t side = 0; side < 2; ++side) {
      seam.maps[side].resize(count);
      seam.colors[side].resize(count);
      for (const auto& sample : maps_[side ? seam.right : seam.left]) {
        const uint32_t x = (sample.output % p.width + p.width - seam.start) % p.width;
        if (x < seam.width) seam.maps[side][sample.output / p.width * seam.width + x] = sample;
      }
    }
    seam.writable.resize(count);
    for (uint32_t y = 0; y < p.height; ++y) for (uint32_t x = 0; x < seam.width; ++x) {
      const auto id = owner[y * p.width + (seam.start + x) % p.width];
      seam.writable[y * seam.width + x] = id == seam.left || id == seam.right;
    }
    seam.costs.resize(count);
    seam.filtered.resize(count);
    seam.parents.resize(count);
    seam.accumulated.resize(seam.width);
    seam.next.resize(seam.width);
    seam.path.resize(p.height);
    const uint32_t cell_x = std::max(4U, p.width / 100), cell_y = std::max(4U, p.height / 40);
    seam.illumination_width = (seam.width - 1 + cell_x - 1) / cell_x + 1;
    seam.illumination_height = (p.height - 1 + cell_y - 1) / cell_y + 1;
    seam.illumination.resize(static_cast<size_t>(seam.illumination_width) * seam.illumination_height * 3);
    seam.correction_weight.resize(count);
    seams_.push_back(std::move(seam));
  }
  // Overlapping bands must not let a later camera pair overwrite an earlier cut.
  // Partition by circular distance to each band centre, independently of camera IDs.
  std::vector<uint8_t> adaptive(pixels, 255);
  std::vector<uint32_t> distance(pixels, p.width);
  for (size_t id = 0; id < seams_.size(); ++id) {
    const auto& seam = seams_[id];
    const uint32_t centre = (seam.start + seam.width / 2) % p.width;
    for (size_t i = 0; i < seam.writable.size(); ++i) {
      if (!seam.writable[i]) continue;
      const size_t output = i / seam.width * p.width + (seam.start + i % seam.width) % p.width;
      const uint32_t delta = static_cast<uint32_t>(std::abs(static_cast<int>(i % seam.width) -
                                                          static_cast<int>(seam.width / 2)));
      const auto previous = adaptive[output];
      if (delta < distance[output] || (delta == distance[output] && previous != 255 &&
          centre < (seams_[previous].start + seams_[previous].width / 2) % p.width)) {
        adaptive[output] = static_cast<uint8_t>(id);
        distance[output] = delta;
      }
    }
  }
  for (size_t id = 0; id < seams_.size(); ++id) {
    auto& seam = seams_[id];
    for (size_t i = 0; i < seam.writable.size(); ++i) {
      const size_t output = i / seam.width * p.width + (seam.start + i % seam.width) % p.width;
      seam.writable[i] = seam.writable[i] && adaptive[output] == id;
    }
    // Fade at overlap/ownership edges instead of adding a new band boundary.
    for (uint32_t y = 0; y < p.height; ++y) for (uint32_t x = 0; x < seam.width; ++x) {
      const size_t i = static_cast<size_t>(y) * seam.width + x;
      seam.correction_weight[i] = seam.writable[i] && seam.maps[0][i].weight > 0 && seam.maps[1][i].weight > 0
          ? static_cast<float>(std::min({x + 1, seam.width - x, y + 1, p.height - y})) : 0;
    }
    for (uint32_t y = 0; y < p.height; ++y) for (uint32_t x = 0; x < seam.width; ++x) {
      const size_t i = static_cast<size_t>(y) * seam.width + x;
      if (x) seam.correction_weight[i] = std::min(seam.correction_weight[i], seam.correction_weight[i - 1] + 1);
      if (y) seam.correction_weight[i] = std::min(seam.correction_weight[i], seam.correction_weight[i - seam.width] + 1);
    }
    for (uint32_t y = p.height; y-- > 0;) for (uint32_t x = seam.width; x-- > 0;) {
      const size_t i = static_cast<size_t>(y) * seam.width + x;
      if (x + 1 < seam.width) seam.correction_weight[i] = std::min(seam.correction_weight[i], seam.correction_weight[i + 1] + 1);
      if (y + 1 < p.height) seam.correction_weight[i] = std::min(seam.correction_weight[i], seam.correction_weight[i + seam.width] + 1);
    }
    for (auto& weight : seam.correction_weight) weight = std::clamp((weight - 1) / std::max(2.0f, p.width / 100.0f), 0.0f, 1.0f);
    seam.quality_loss.resize(seam.writable.size());
    seam.centre_costs.resize(seam.width);
    for (uint32_t x = 0; x < seam.width; ++x)
      seam.centre_costs[x] = 0.04f * std::abs(static_cast<int>(x) - static_cast<int>(seam.width / 2));
    seam.column_sums.resize(seam.width);
    for (uint32_t y = 0; y < p.height; ++y) for (uint32_t x = 0; x < seam.width; ++x) {
      const size_t i = static_cast<size_t>(y) * seam.width + x;
      const float left = seam.maps[0][i].weight, right = seam.maps[1][i].weight;
      const float balance = std::min(left, right) / std::max(0.001f, std::max(left, right));
      seam.quality_loss[i] = 1 - balance;
    }
  }
  for (size_t cam = 0; cam < 4; ++cam) {
    auto& map = maps_[cam];
    map.erase(std::remove_if(map.begin(), map.end(), [&](const Sample& sample) {
      return owner[sample.output] != cam || adaptive[sample.output] != 255;
    }), map.end());
  }
  size_t covered = 0;
  for (uint32_t x = 0; x < p.width; ++x) {
    const double a = ((x + 0.5) / p.width * 360.0 - 180.0 + p.yaw_deg) * kRadians;
    const std::array<double, 3> ray{std::cos(a), std::sin(a), 0};
    for (const auto& c : calibration.cameras) {
      double u, v, weight;
      if (Project(ray, c, p, &u, &v, &weight)) { ++covered; break; }
    }
  }
  horizon_coverage_percent_ = 100.0 * covered / p.width;
  if (coverage_percent_ == 0) throw std::runtime_error("calibration does not cover the requested panorama");
  rgb_.resize(pixels * 3);
  workers_ = std::make_unique<Workers>(*this);
}
void PanoramaStitcher::BalanceExposure(const std::array<PanoramaImage, 4>& images) {
  std::array<std::array<double, 4>, 4> weights{}, differences{};
  constexpr double range = 1.3862943611198906;
  for (const auto& s : seams_) {
    std::array<unsigned, 257> histogram{};
    std::array<double, 257> sums{};
    unsigned count = 0;
    const uint32_t dx = std::max(1U, s.width / 40), dy = std::max(1U, projection_.height / 30);
    for (uint32_t y = dy / 2; y < projection_.height; y += dy)
      for (uint32_t x = dx / 2; x < s.width; x += dx) {
        const size_t i = static_cast<size_t>(y) * s.width + x;
        const auto& a = s.maps[0][i];
        const auto& b = s.maps[1][i];
        if (!s.writable[i] || a.weight < 0.02f || b.weight < 0.02f) continue;
        const auto left = Pixel(images[s.left], a.x_q8, a.y_q8);
        const auto right = Pixel(images[s.right], b.x_q8, b.y_q8);
        if (!Comparable(left, right)) continue;
        const double ratio = std::log(Luma(right) / Luma(left));
        if (std::abs(ratio) > range) continue;
        const auto bin = static_cast<size_t>(std::clamp(std::lround(ratio * 128 / range) + 128L, 0L, 256L));
        ++histogram[bin]; sums[bin] += ratio; ++count;
      }
    if (count < 32) continue;
    unsigned cumulative = 0;
    size_t low = 0, high = 256;
    bool found_low = false;
    for (size_t bin = 0; bin < histogram.size(); ++bin) {
      cumulative += histogram[bin];
      if (!found_low && cumulative * 4 >= count) { low = bin; found_low = true; }
      if (cumulative * 4 >= count * 3) { high = bin; break; }
    }
    if (high - low > 64) continue;
    double sum = 0;
    unsigned support = 0;
    for (size_t bin = low; bin <= high; ++bin) {
      sum += sums[bin]; support += histogram[bin];
    }
    const double weight = static_cast<double>(support) / count;
    const double ratio = sum / support;
    weights[s.left][s.right] += weight; weights[s.right][s.left] += weight;
    differences[s.left][s.right] += weight * ratio;
    differences[s.right][s.left] -= weight * ratio;
  }
  // Each connected overlap component has its own zero-mean log gain. Missing
  // evidence never couples unrelated cameras or changes an isolated camera.
  std::array<bool, 4> visited{};
  gains_.fill(1);
  for (size_t root = 0; root < 4; ++root) {
    if (visited[root]) continue;
    std::array<size_t, 4> component{};
    size_t size = 1;
    component[0] = root; visited[root] = true;
    for (size_t i = 0; i < size; ++i) for (size_t j = 0; j < 4; ++j)
      if (weights[component[i]][j] > 0 && !visited[j]) { visited[j] = true; component[size++] = j; }
    if (size < 2) continue;
    std::array<std::array<double, 5>, 4> matrix{};
    for (size_t i = 0; i < size; ++i) {
      for (size_t j = 0; j < size; ++j) {
        const double w = weights[component[i]][component[j]];
        matrix[i][i] += w;
        matrix[i][j] += 1.0 / size - w;
        matrix[i][size] += differences[component[i]][component[j]];
      }
    }
    for (size_t k = 0; k < size; ++k) {
      size_t pivot = k;
      for (size_t i = k + 1; i < size; ++i)
        if (std::abs(matrix[i][k]) > std::abs(matrix[pivot][k])) pivot = i;
      std::swap(matrix[k], matrix[pivot]);
      const double divisor = matrix[k][k];
      for (size_t j = k; j <= size; ++j) matrix[k][j] /= divisor;
      for (size_t i = 0; i < size; ++i) if (i != k) {
        const double factor = matrix[i][k];
        for (size_t j = k; j <= size; ++j) matrix[i][j] -= factor * matrix[k][j];
      }
    }
    double largest = 0;
    for (size_t i = 0; i < size; ++i) largest = std::max(largest, std::abs(matrix[i][size]));
    const double scale = largest > range / 2 ? (range / 2) / largest : 1;
    for (size_t i = 0; i < size; ++i) gains_[component[i]] = static_cast<float>(std::exp(matrix[i][size] * scale));
  }
}

void PanoramaStitcher::RenderSeam(Seam& s, const std::array<PanoramaImage, 4>& images) {
  const int width = static_cast<int>(s.width), height = static_cast<int>(projection_.height);
  for (size_t side = 0; side < 2; ++side) {
    const auto& image = images[side ? s.right : s.left];
    for (size_t i = 0; i < s.maps[side].size(); ++i) {
      const auto& sample = s.maps[side][i];
      if (sample.weight <= 0) continue;
      const auto color = Pixel(image, sample.x_q8, sample.y_q8);
      const float gain = gains_[side ? s.right : s.left];
      for (size_t ch = 0; ch < 3; ++ch) s.colors[side][i][ch] = Byte(color[ch] * gain);
    }
  }
  const int cell_x = std::max(4U, projection_.width / 100), cell_y = std::max(4U, projection_.height / 40);
  // Blur only the inter-view illumination difference, never scene pixels.
  // Sparse clipped/chroma-gated samples exclude strong foreground disagreement.
  for (uint32_t gy = 0; gy < s.illumination_height; ++gy)
    for (uint32_t gx = 0; gx < s.illumination_width; ++gx) {
      std::array<float, 3> sum{};
      float count = 0;
      const int cx = std::min(width - 1, static_cast<int>(gx) * cell_x);
      const int cy = std::min(height - 1, static_cast<int>(gy) * cell_y);
      for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
        const int x = cx + dx * cell_x, y = cy + dy * cell_y;
        if (x < 0 || x >= width || y < 0 || y >= height) continue;
        const size_t i = static_cast<size_t>(y) * width + x;
        if (!s.writable[i] || s.maps[0][i].weight < 0.02f || s.maps[1][i].weight < 0.02f) continue;
        std::array<float, 3> a{}, b{};
        for (size_t ch = 0; ch < 3; ++ch) { a[ch] = s.colors[0][i][ch]; b[ch] = s.colors[1][i][ch]; }
        if (!Comparable(a, b) || std::abs(Luma(a) - Luma(b)) > 48) continue;
        const float weight = static_cast<float>((3 - std::abs(dx)) * (3 - std::abs(dy)));
        for (size_t ch = 0; ch < 3; ++ch) sum[ch] += weight * (b[ch] - a[ch]);
        count += weight;
      }
      for (size_t ch = 0; ch < 3; ++ch)
        s.illumination[(gy * s.illumination_width + gx) * 3 + ch] = count > 0 ? std::clamp(sum[ch] / count, -32.0f, 32.0f) : 0;
    }
  // Patch costs avoid matching dark lines through misregistered foregrounds.
  // Only the source selection changes; all sampling coordinates stay calibrated.
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const size_t i = static_cast<size_t>(y) * width + x;
    float difference = 0, edge = 0;
    for (size_t ch = 0; ch < 3; ++ch) {
      difference += std::abs(int(s.colors[0][i][ch]) - int(s.colors[1][i][ch])) / 3.0f;
      for (size_t side = 0; side < 2; ++side) {
        if (x > 0 && x + 1 < width)
          edge = std::max(edge, 0.5f * std::abs(int(s.colors[side][i-1][ch]) - int(s.colors[side][i+1][ch])));
        if (y > 0 && y + 1 < height)
          edge = std::max(edge, 0.5f * std::abs(int(s.colors[side][i-width][ch]) - int(s.colors[side][i+width][ch])));
      }
    }
    s.costs[i] = difference + 0.7f * edge;
  }
  const int rx = std::max(1, static_cast<int>(projection_.width / 100));
  const int ry = std::max(1, height / 120);
  for (int y = 0; y < height; ++y) {
    const size_t row = static_cast<size_t>(y) * width;
    float sum = 0;
    for (int x = 0; x <= std::min(rx, width - 1); ++x) sum += s.costs[row+x];
    for (int x = 0; x < width; ++x) {
      s.filtered[row+x] = sum / (std::min(width-1, x+rx) - std::max(0, x-rx) + 1);
      if (x-rx >= 0) sum -= s.costs[row+x-rx];
      if (x+rx+1 < width) sum += s.costs[row+x+rx+1];
    }
  }
  // Keep each column's rolling sum, but traverse source rows contiguously.
  // Addition/subtraction order within a column matches the separable filter.
  std::fill(s.column_sums.begin(), s.column_sums.end(), 0);
  for (int y = 0; y <= std::min(ry, height-1); ++y)
    for (int x = 0; x < width; ++x) s.column_sums[x] += s.filtered[y*width+x];
  for (int y = 0; y < height; ++y) {
    const size_t row = static_cast<size_t>(y) * width;
    const float count = static_cast<float>(std::min(height-1, y+ry) - std::max(0, y-ry) + 1);
    for (int x = 0; x < width; ++x) {
      const size_t i = row + x;
      const float left = s.maps[0][i].weight, right = s.maps[1][i].weight;
      // Keep the quality multiply beside the blurred cost for native FMA rounding.
      s.costs[i] = s.column_sums[x] / count + 12 * s.quality_loss[i] + s.centre_costs[x];
      if (s.initialized) s.costs[i] += 0.6f * std::min(20, std::abs(x-s.path[y]));
      if (left <= 0 || right <= 0 || !s.writable[i]) s.costs[i] += 10000;
      if (x < width/20 || x >= width-width/20) s.costs[i] += 100000;
      if (y-ry >= 0) s.column_sums[x] -= s.filtered[(y-ry)*width+x];
      if (y+ry+1 < height) s.column_sums[x] += s.filtered[(y+ry+1)*width+x];
    }
  }
  const int step = std::max(1, static_cast<int>(projection_.width / 400));
  std::copy_n(s.costs.begin(), width, s.accumulated.begin());
  for (int y = 1; y < height; ++y) {
    std::fill(s.next.begin(), s.next.end(), std::numeric_limits<double>::infinity());
    auto* parents = s.parents.data() + static_cast<size_t>(y) * width;
    // Offset order preserves the lowest-column tie break of the scalar search.
    // Each offset traverses contiguous rows, allowing SIMD compare/select.
    for (int offset = -step; offset <= step; ++offset) {
      const double penalty = 3.0 * std::abs(offset);
      for (int x = std::max(0, -offset); x < std::min(width, width-offset); ++x) {
        const double cost = s.accumulated[x+offset] + penalty;
        if (cost < s.next[x]) {
          s.next[x] = cost;
          parents[x] = static_cast<int8_t>(offset);
        }
      }
    }
    for (int x = 0; x < width; ++x) s.next[x] += s.costs[y*width+x];
    s.accumulated.swap(s.next);
  }
  s.path.back() = static_cast<int>(std::min_element(s.accumulated.begin(), s.accumulated.end()) - s.accumulated.begin());
  for (int y = height-1; y > 0; --y) s.path[y-1] = s.path[y] + s.parents[y*width+s.path[y]];
  s.initialized = true;
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
    const size_t i = static_cast<size_t>(y) * width + x;
    if (!s.writable[i]) continue;
    const bool left = s.maps[0][i].weight > 0, right = s.maps[1][i].weight > 0;
    if (!left && !right) continue;
    const size_t side = !left || (right && x >= s.path[y]) ? 1 : 0;
    const auto& color = s.colors[side][i];
    const auto& other = s.colors[1-side][i];
    const uint32_t output = y * projection_.width + (s.start+x) % projection_.width;
    auto* out = rgb_.data() + static_cast<size_t>(output) * 3;
    if (!left || !right) { std::copy(color.begin(), color.end(), out); continue; }
    const float px = static_cast<float>(x) / cell_x, py = static_cast<float>(y) / cell_y;
    const auto gx = std::min(static_cast<uint32_t>(px), s.illumination_width - 2);
    const auto gy = std::min(static_cast<uint32_t>(py), s.illumination_height - 2);
    const float fx = px - gx, fy = py - gy;
    std::array<float, 3> delta{};
    const size_t grid = (gy * s.illumination_width + gx) * 3;
    for (size_t ch = 0; ch < 3; ++ch) {
      const float top = s.illumination[grid + ch] * (1 - fx) + s.illumination[grid + 3 + ch] * fx;
      const float bottom = s.illumination[grid + s.illumination_width * 3 + ch] * (1 - fx) +
                           s.illumination[grid + s.illumination_width * 3 + 3 + ch] * fx;
      delta[ch] = (top * (1 - fy) + bottom * fy) * s.correction_weight[i];
    }
    const float radius = std::max(4.0f, width * 0.35f);
    const float position = x - s.path[y] + 0.5f;
    const float t = std::clamp(0.5f + position / (2 * radius), 0.0f, 1.0f);
    const float blend = t * t * (3 - 2 * t);
    float difference = 0;
    for (size_t ch = 0; ch < 3; ++ch)
      difference = std::max(difference, std::abs(float(s.colors[1][i][ch]) - s.colors[0][i][ch] - delta[ch]));
    const float agreement = std::max(0.0f, 1.0f - difference / 24.0f);
    const float alpha = std::max(0.0f, 0.5f - std::abs(position) / 8) * agreement * agreement;
    for (size_t ch = 0; ch < 3; ++ch) {
      const float adjusted = color[ch] + (side == 0 ? blend : blend - 1) * delta[ch];
      const float residual = other[ch] - float(color[ch]) + (side == 0 ? -delta[ch] : delta[ch]);
      out[ch] = Byte(adjusted + alpha * residual);
    }
  }
}

void PanoramaStitcher::RenderJob(size_t job, const std::array<PanoramaImage, 4>& images) {
  if (job >= 4) {
    RenderSeam(seams_[job - 4], images);
    return;
  }
  for (const auto& s : maps_[job]) {
    auto* out = rgb_.data() + static_cast<size_t>(s.output) * 3;
    const auto color = Pixel(images[job], s.x_q8, s.y_q8);
    for (size_t ch = 0; ch < 3; ++ch) out[ch] = Byte(color[ch] * gains_[job]);
  }
}

const std::vector<uint8_t>& PanoramaStitcher::Render(const std::array<PanoramaImage, 4>& images) {
  for (size_t cam = 0; cam < 4; ++cam) {
    const auto& im = images[cam];
    const auto& c = calibration_.cameras[cam];
    if (im.width != c.width || im.height != c.height ||
        (im.rgb ? im.stride < im.width * 3 :
         (!im.y || !im.uv || im.width < 4 || im.height < 4 || im.stride < im.width ||
          im.y_size < static_cast<size_t>(im.stride) * im.height ||
          im.uv_size < static_cast<size_t>(im.stride) * (im.height / 2))))
      throw std::runtime_error("input image does not match calibration dimensions/planes");
  }
  BalanceExposure(images);
  std::fill(rgb_.begin(), rgb_.end(), 0);
  workers_->Render(images);
  return rgb_;
}
}  // namespace robobaton_demo
