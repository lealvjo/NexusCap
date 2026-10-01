#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nexuscap {

// Same contract as the validated spike: host owns the maps, child Presents+NR, host window stays.
inline constexpr wchar_t kPipelineFbInName[] = L"Local\\NexusCapPipeIn";
inline constexpr wchar_t kPipelineFbOutName[] = L"Local\\NexusCapPipeOut";
inline constexpr std::uint32_t kPipelineFbMagic = 0x4E435046u;
inline constexpr int kPipelineFbMaxW = 2560;
inline constexpr int kPipelineFbMaxH = 1440;
inline constexpr const char* kPipelineWorkerExe = "nexuscap_pipeline.exe";

inline std::string pipeline_worker_dir(const std::string& exe_dir) {
  if (exe_dir.empty()) return "worker";
  const char c = exe_dir.back();
  if (c == '\\' || c == '/') return exe_dir + "worker";
  return exe_dir + "\\worker";
}

inline bool pipeline_dir_is_worker(const std::string& dir) {
  const auto n = dir.size();
  if (n < 6) return false;
  if (dir.compare(n - 6, 6, "worker") != 0) return false;
  if (n == 6) return true;
  const char p = dir[n - 7];
  return p == '\\' || p == '/';
}

struct PipelineFbHeader {
  std::uint32_t magic = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t pass = 0;
  std::uint32_t frame = 0;
  std::uint32_t pid = 0;
  std::uint32_t gpu = 0;
  std::uint32_t pad = 0;
  std::uint64_t in_shared = 0;
  std::uint64_t out_shared = 0;
};

inline std::size_t pipeline_fb_bytes() {
  return sizeof(PipelineFbHeader) +
         static_cast<std::size_t>(kPipelineFbMaxW) * static_cast<std::size_t>(kPipelineFbMaxH) * 4u;
}

inline std::uint32_t* pipeline_fb_pixels(PipelineFbHeader* h) {
  return reinterpret_cast<std::uint32_t*>(h + 1);
}

class PipelineHost {
 public:
  ~PipelineHost() { stop(); }
  bool start(const std::string& exe_dir);
  bool recycle();
  void stop();
  bool running() const;
  bool awaiting_live() const { return awaiting_live_; }
  unsigned long pid() const { return pid_; }
  // Unique NR frames from the child — host Present can stay at 60 while 2x is ~40.
  bool consume_new_out();
  bool have_unique_ft() const { return have_unique_ft_; }
  double unique_interval_ms() const { return unique_interval_ms_; }
  double unique_fps() const { return unique_fps_; }
  // NV12-decoded BGRA goes to the child Present; last good frame holds while the child dies.
  bool process_color(void* device, void* ctx, void* tex, int w, int h, std::uint32_t frame);

 private:
  bool spawn();
  bool ensure_maps();
  bool ensure_staging(void* device, int w, int h);
  bool ensure_gpu(void* device, int w, int h);
  void release_gpu();
  void finish_recycle_if_live();
  void note_unique_out(std::uint32_t frame);
  std::string exe_dir_;
  void* process_ = nullptr;
  void* old_process_ = nullptr;
  unsigned long pid_ = 0;
  unsigned long old_pid_ = 0;
  bool awaiting_live_ = false;
  void* in_map_ = nullptr;
  void* out_map_ = nullptr;
  PipelineFbHeader* in_ = nullptr;
  PipelineFbHeader* out_ = nullptr;
  void* staging_ = nullptr;
  int stage_w_ = 0;
  int stage_h_ = 0;
  void* in_gpu_ = nullptr;
  void* out_gpu_ = nullptr;
  void* hold_gpu_ = nullptr;
  void* km_in_ = nullptr;
  void* km_out_ = nullptr;
  std::uint64_t in_shared_h_ = 0;
  std::uint64_t out_shared_h_ = 0;
  int gpu_w_ = 0;
  int gpu_h_ = 0;
  bool gpu_ok_ = false;
  std::vector<std::uint8_t> last_bgra_;
  int last_w_ = 0;
  int last_h_ = 0;
  bool have_last_ = false;
  std::uint32_t hold_min_frame_ = 2;
  std::uint32_t last_out_frame_ = 0;
  bool took_new_out_ = false;
  bool have_unique_ft_ = false;
  double unique_interval_ms_ = 0;
  double unique_fps_ = 0;
  double last_unique_t_ = 0;
};

PipelineHost& pipeline_global();
bool recycle_pipeline_worker(const std::string& exe_dir);

}  // namespace nexuscap
