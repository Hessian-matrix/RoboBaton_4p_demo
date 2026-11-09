#include "panorama_http.h"
#include "panorama_jpeg.h"
#include "panorama_stitcher.h"
#ifndef PANORAMA_OFFLINE_ONLY
#include "sc132camera.h"
#endif
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <fcntl.h>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <unistd.h>

#ifndef ROBOBATON_RELEASE_VERSION
#define ROBOBATON_RELEASE_VERSION "0.0.0+unknown"
#endif
namespace {
using namespace robobaton_demo;
using Clock = std::chrono::steady_clock;
volatile sig_atomic_t stopped = 0;
void Signal(int) { stopped = 1; }
struct Options {
  PanoramaProjection projection;
  std::string calibration, images, output;
  std::string listen = "0.0.0.0", timestamp_path = "observer";
  uint16_t port = 8080;
  double seconds = 0, max_fps = 10;
  bool once = false;
};
std::string DefaultCalibration() {
  std::array<char,4096> path{};
  const auto n = readlink("/proc/self/exe", path.data(), path.size()-1);
  if (n <= 0) throw std::runtime_error("cannot locate executable; supply --calibration");
  const auto directory = std::filesystem::path(std::string(path.data(), static_cast<size_t>(n))).parent_path();
  for (const auto& p : {directory/"config/calibration.yaml", directory/"../config/calibration.yaml"})
    if (std::filesystem::is_regular_file(p)) return p.lexically_normal().string();
  return "config/calibration.yaml";
}
double Number(const std::string& text) {
  size_t end = 0;
  const double n = std::stod(text,&end);
  if (end != text.size() || !std::isfinite(n)) throw std::runtime_error("invalid numeric argument: " + text);
  return n;
}
uint32_t Integer(const std::string& text, uint32_t min, uint32_t max) {
  const auto n=Number(text);
  if(n<min || n>max || std::floor(n)!=n) throw std::runtime_error("integer argument out of range: "+text);
  return static_cast<uint32_t>(n);
}
void Help() {
  std::cout << "panorama_demo " << ROBOBATON_RELEASE_VERSION << " - calibrated four-camera 360 equirectangular Web preview\n"
    "  --calibration <yaml>   schema 7 KB4 + T_camN_from_imu; defaults to package config/calibration.yaml\n"
    "  --listen <IPv4>        default 0.0.0.0 (trusted isolated LAN only; no authentication)\n"
    "  --port <1..65535>      default 8080; GET / /frame.jpg /status.json\n"
    "  --width <64..4096> --height <32..2048>   default 1600x600, at most 4194304 pixels\n"
    "  --elevation-min <deg> --elevation-max <deg> default -60/+60, within -90/+90\n"
    "  --yaw <deg>           panorama centre azimuth, default 0 (IMU +X, positive toward +Y)\n"
    "  --distance-m <m>      sphere radius >=0.1m; 0=infinity rotation-only (default)\n"
    "  --max-theta-deg <deg> optical ray limit, default 96.9 (Camera A diagonal FOV/2)\n"
    "  --max-fps <1..30>     processing limit, default 10; capture stays 30fps, latest complete group only\n"
    "  --camera-timestamp-path <observer|official>  default observer\n"
    "  --images <directory>  offline cam0.jpg..cam3.jpg in calibration pixel orientation, no camera access\n"
    "  --output <jpg>        save first stitched JPEG; refuses to overwrite an existing path\n"
    "  --once                offline only, requires --output; render once and exit without HTTP\n"
    "  --duration <seconds>  normal stop after duration; default until SIGINT/SIGTERM\n"
    "Uncovered directions stay black. Finite baselines cause depth-dependent parallax; not depth reconstruction.\n";
}
Options Parse(int argc,char** argv) {
  Options o;
  o.calibration=DefaultCalibration();
  for(int i=1;i<argc;++i) {
    const std::string key=argv[i];
    if(key=="--once") { o.once=true;continue; }
    if(i+1>=argc) throw std::runtime_error("missing value for "+key);
    const std::string v=argv[++i];
    if(key=="--calibration") o.calibration=v;
    else if(key=="--images") o.images=v;
    else if(key=="--output") o.output=v;
    else if(key=="--listen") o.listen=v;
    else if(key=="--port") o.port=static_cast<uint16_t>(Integer(v,1,65535));
    else if(key=="--width") o.projection.width=Integer(v,64,4096);
    else if(key=="--height") o.projection.height=Integer(v,32,2048);
    else if(key=="--elevation-min") o.projection.elevation_min_deg=Number(v);
    else if(key=="--elevation-max") o.projection.elevation_max_deg=Number(v);
    else if(key=="--yaw") o.projection.yaw_deg=Number(v);
    else if(key=="--distance-m") o.projection.distance_m=Number(v);
    else if(key=="--max-theta-deg") o.projection.max_theta_deg=Number(v);
    else if(key=="--duration") o.seconds=Number(v);
    else if(key=="--max-fps") o.max_fps=Number(v);
    else if(key=="--camera-timestamp-path") o.timestamp_path=v;
    else throw std::runtime_error("unknown argument: "+key);
  }
  if(o.seconds<0 || o.max_fps<1 || o.max_fps>30 ||
     (o.timestamp_path!="observer" && o.timestamp_path!="official")) throw std::runtime_error("invalid duration, FPS or timestamp path");
  if(o.once && (o.images.empty() || o.output.empty())) throw std::runtime_error("--once requires --images and --output");
  if(!o.output.empty() && std::filesystem::exists(o.output)) throw std::runtime_error("output already exists: "+o.output);
  return o;
}
std::string Status(const Options& o,const PanoramaCalibration& c,const PanoramaStitcher& s,
                   uint64_t group,uint64_t timestamp,uint64_t processed,uint64_t dropped,double ms) {
  std::ostringstream out;
  out << std::setprecision(12) << "{\"group_id\":\""<<group<<"\",\"timestamp_ns\":\""<<timestamp
      <<"\",\"width\":"<<o.projection.width<<",\"height\":"<<o.projection.height
      <<",\"coverage_percent\":"<<s.coverage_percent()<<",\"horizon_coverage_percent\":"<<s.horizon_coverage_percent()
      <<",\"processed_frames\":"<<processed<<",\"dropped_groups\":"<<dropped<<",\"processing_ms\":"<<ms
      <<",\"projection_distance_m\":"<<o.projection.distance_m<<",\"calibration_acceptance\":\""<<c.acceptance
      <<"\",\"source\":\""<<(o.images.empty()?"live":"images")<<"\",\"yaw_deg\":"<<o.projection.yaw_deg
      <<",\"elevation_min_deg\":"<<o.projection.elevation_min_deg<<",\"elevation_max_deg\":"<<o.projection.elevation_max_deg<<"}";
  return out.str();
}
void Save(const std::string& path,const std::vector<uint8_t>& bytes) {
  if(path.empty())return;
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  if (fd < 0) throw std::runtime_error("cannot create new panorama JPEG: " + path);
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      close(fd);
      throw std::runtime_error("cannot write panorama JPEG: " + path);
    }
    offset += static_cast<size_t>(written);
  }
  if (close(fd)) throw std::runtime_error("cannot close panorama JPEG: " + path);
}
#ifndef PANORAMA_OFFLINE_ONLY
struct FrameGroup {
  std::array<sc132_frame_t*,4> frames{};
  uint64_t id=0,timestamp=0;
  FrameGroup()=default;
  ~FrameGroup() { Clear(); }
  FrameGroup(const FrameGroup&)=delete;
  FrameGroup& operator=(const FrameGroup&)=delete;
  FrameGroup(FrameGroup&& other) noexcept { *this=std::move(other); }
  FrameGroup& operator=(FrameGroup&& other) noexcept {
    if(this!=&other) { Clear(); frames=other.frames;other.frames.fill(nullptr);id=other.id;timestamp=other.timestamp; }
    return *this;
  }
  void Clear() noexcept { for(auto*& frame:frames) if(frame) { sc132_frame_release(frame);frame=nullptr; } }
};
class Capture {
 public:
  ~Capture() { Stop(); }
  void Start(const Options& o,const PanoramaCalibration& calibration) {
    for(const auto& c:calibration.cameras) if(c.width!=calibration.cameras[0].width || c.height!=calibration.cameras[0].height)
      throw std::runtime_error("live capture requires identical calibrated image dimensions");
    if(setenv("SC132_TRIGGER_MODE","vin_lpwm",1) || setenv("SC132_TIMESTAMP_PATH",o.timestamp_path.c_str(),1) ||
       setenv("SC132_TEST_EXPOSURE_MIDPOINT","0",1)) throw std::runtime_error("cannot set capture startup environment");
    // Same package --rotate 0 orientation used by calibration images; sensor mount compensation is 90 degrees.
    if(sc132_set_output_rotation(90)!=SC132_STATUS_OK || sc132_set_fps(30)!=SC132_STATUS_OK)
      throw std::runtime_error("cannot configure SC132 30fps/calibration pixel orientation");
    sc132_frame_set_config_t config=SC132_FRAME_SET_CONFIG_INIT;
    config.callback=Callback;config.user_data=this;
    config.width=calibration.cameras[0].width;config.height=calibration.cameras[0].height;
    attempted_=true;
    if(sc132_start_frame_set(&config,15)!=SC132_STATUS_OK) throw std::runtime_error("SC132 start failed; inspect owner/calibration resolution/trigger diagnostics");
  }
  void Stop() noexcept {
    if(!attempted_)return;
    sc132_request_stop();
    { std::lock_guard<std::mutex> lock(mutex_); closing_=true;pending_.Clear(); }
    sc132_stop();attempted_=false;
  }
  bool Next(FrameGroup* out,std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait_for(lock,wait,[this]{return failed_ || pending_.frames[0];});
    if(failed_)throw std::runtime_error("SC132 callback terminated or returned an invalid four-camera group");
    if(!pending_.frames[0])return false;
    *out=std::move(pending_);
    return true;
  }
  uint64_t dropped() const noexcept { return dropped_.load(); }
 private:
  static void Callback(const sc132_frame_set_t* set,void* data) noexcept {
    auto* self=static_cast<Capture*>(data);
    try {
      if(!set || set->struct_size!=sizeof(*set) || set->camera_count!=4) throw std::runtime_error("invalid group");
      FrameGroup group;group.id=set->group_id;group.timestamp=set->group_timestamp_ns;
      for(const auto& item:set->items) {
        if(item.camera_id>=4 || !item.frame || group.frames[item.camera_id] || sc132_frame_retain(item.frame)!=SC132_STATUS_OK)
          throw std::runtime_error("invalid frame reference");
        group.frames[item.camera_id]=item.frame;
      }
      std::lock_guard<std::mutex> lock(self->mutex_);
      if(self->closing_)return;
      if(self->pending_.frames[0])++self->dropped_;
      self->pending_=std::move(group);self->ready_.notify_one();
    } catch(...) {
      { std::lock_guard<std::mutex> lock(self->mutex_);self->failed_=true;self->ready_.notify_one(); }
      sc132_request_stop();
    }
  }
  std::mutex mutex_;
  std::condition_variable ready_;
  FrameGroup pending_;
  std::atomic<uint64_t> dropped_{0};
  bool failed_=false,closing_=false,attempted_=false;
};
#endif
}  // namespace
int main(int argc,char** argv) {
  using namespace robobaton_demo;
  for(int i=1;i<argc;++i) {
    if(std::string(argv[i])=="--help") { Help();return 0; }
    if(std::string(argv[i])=="--version") { std::cout<<"panorama_demo "<<ROBOBATON_RELEASE_VERSION<<"\n";return 0; }
  }
  try {
    const auto o=Parse(argc,argv);
    const auto calibration=LoadPanoramaCalibration(o.calibration);
    PanoramaStitcher stitcher(calibration,o.projection);
    std::signal(SIGINT,Signal);std::signal(SIGTERM,Signal);
    std::cout<<"PANORAMA calibration="<<o.calibration<<" acceptance="<<calibration.acceptance
             <<" coverage="<<stitcher.coverage_percent()<<"% horizon="<<stitcher.horizon_coverage_percent()<<"%\n";
    PanoramaHttpServer http;
    if(!o.once) { http.Start(o.listen,o.port);std::cout<<"Web panorama port="<<o.port<<" (trusted LAN only)\n"; }
    const auto start=Clock::now();
    const auto expired=[&]{return stopped || (o.seconds>0 && std::chrono::duration<double>(Clock::now()-start).count()>=o.seconds);};
    if(!o.images.empty()) {
      std::array<PanoramaRgbImage,4> owned;
      std::array<PanoramaImage,4> views;
      for(size_t i=0;i<4;++i) {
        owned[i]=ReadPanoramaJpeg((std::filesystem::path(o.images)/("cam"+std::to_string(i)+".jpg")).string());
        const auto& image=owned[i];
        views[i]={image.width,image.height,image.width*3,image.pixels.data(),nullptr,nullptr,0,0};
      }
      const auto begin=Clock::now();
      auto jpeg=EncodePanoramaJpeg(stitcher.Render(views),o.projection.width,o.projection.height);
      const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
      Save(o.output,*jpeg);
      const auto status=Status(o,calibration,stitcher,0,0,1,0,ms);
      std::cout<<"PANORAMA_RESULT "<<status<<"\n";
      if(!o.once) { http.Publish(jpeg,status);while(!expired())std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    } else {
#ifdef PANORAMA_OFFLINE_ONLY
      throw std::runtime_error("this host build supports --images only; use the X5 build for live capture");
#else
      Capture capture;
      capture.Start(o,calibration);
      uint64_t processed=0;
      auto last=Clock::now();
      while(!expired()) {
        const auto begin=Clock::now();
        FrameGroup group;
        if(!capture.Next(&group,std::chrono::milliseconds(100))) {
          if(Clock::now()-last>std::chrono::seconds(5))throw std::runtime_error("no complete SC132 frame-set for five seconds");
          continue;
        }
        last=Clock::now();
        std::array<PanoramaImage,4> views;
        for(size_t i=0;i<4;++i) {
          sc132_frame_info_t info{};info.struct_size=sizeof(info);
          if(sc132_frame_get_info(group.frames[i],&info)!=SC132_STATUS_OK || info.camera_id!=i)
            throw std::runtime_error("cannot read retained SC132 frame");
          views[i]={info.width,info.height,info.stride,nullptr,static_cast<const uint8_t*>(info.y_data),
                    static_cast<const uint8_t*>(info.uv_data),static_cast<size_t>(info.y_size),static_cast<size_t>(info.uv_size)};
        }
        const auto& rgb=stitcher.Render(views);
        group.Clear();
        auto jpeg=EncodePanoramaJpeg(rgb,o.projection.width,o.projection.height);
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        if(processed==0)Save(o.output,*jpeg);
        ++processed;
        const auto status=Status(o,calibration,stitcher,group.id,group.timestamp,processed,capture.dropped(),ms);
        http.Publish(jpeg,status);
        if(processed==1 || processed%100==0)std::cout<<"PANORAMA_RESULT "<<status<<"\n";
        const auto deadline=begin+std::chrono::duration<double>(1/o.max_fps);
        while(!expired() && Clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      capture.Stop();
#endif
    }
    http.Stop();
    return 0;
  } catch(const std::exception& error) { std::cerr<<"panorama_demo: "<<error.what()<<"\n";return 1; }
}
