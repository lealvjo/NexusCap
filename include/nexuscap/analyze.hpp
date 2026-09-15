#pragma once
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "nexuscap/hitch.hpp"
#include "nexuscap/stats_window.hpp"

namespace nexuscap {

struct SessionInfo {
  std::string build = "NexusCap-0.1.0";
  std::string device;
  int width = 0;
  int height = 0;
  double fps = 0;
  std::string fourcc = "NV12";
  bool buffering = false;
  uint32_t swapchain_flags = 0;
  bool waitable = false;
  std::string thread_priority = "ABOVE_NORMAL+MMCSS";
  bool fake_capture = false;
  bool dlss_enabled = false;
  std::string dlss_mode = "off";
  std::string dlss_backend = "none";
  std::string renodx_preset = "Default";  // Default / Natural / Cinematic
  bool fg_enabled = false;
  int fg_multiplier = 1;
  std::string fg_backend = "none";
  std::string fg_status = "off";
};

class AnalyzeWriter {
public:
  void set_verbose(bool v) { verbose_ = v; }
  bool verbose() const { return verbose_; }

  void session_start(const SessionInfo& info, double t_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    info_ = info;
    start_ms_ = t_ms;
    lines_.clear();
    hitch_count_ = 0;
    cluster_count_ = 0;
    max_gap_ms_ = 0;
    std::ostringstream o;
    o << "{\"t_ms\":" << t_ms << ",\"type\":\"session_start\""
      << ",\"build\":\"" << json_escape(info.build) << "\""
      << ",\"device\":\"" << json_escape(info.device) << "\""
      << ",\"width\":" << info.width << ",\"height\":" << info.height
      << ",\"fps\":" << info.fps
      << ",\"fourcc\":\"" << json_escape(info.fourcc) << "\""
      << ",\"buffering\":" << (info.buffering ? "true" : "false")
      << ",\"swapchain_flags\":" << info.swapchain_flags
      << ",\"waitable\":" << (info.waitable ? "true" : "false")
      << ",\"thread_priority\":\"" << json_escape(info.thread_priority) << "\""
      << ",\"fake_capture\":" << (info.fake_capture ? "true" : "false")
      << ",\"dlss_enabled\":" << (info.dlss_enabled ? "true" : "false")
      << ",\"dlss_mode\":\"" << json_escape(info.dlss_mode) << "\""
      << ",\"dlss_backend\":\"" << json_escape(info.dlss_backend) << "\""
      << ",\"renodx_preset\":\"" << json_escape(info.renodx_preset) << "\""
      << ",\"fg_enabled\":" << (info.fg_enabled ? "true" : "false")
      << ",\"fg_multiplier\":" << info.fg_multiplier
      << ",\"fg_backend\":\"" << json_escape(info.fg_backend) << "\""
      << ",\"fg_status\":\"" << json_escape(info.fg_status) << "\""
      << "}";
    push_line(o.str());
  }

  void hitch(const HitchEvent& e) {
    std::lock_guard<std::mutex> lock(mu_);
    ++hitch_count_;
    std::ostringstream o;
    o << "{\"t_ms\":" << e.t_ms << ",\"type\":\"hitch\""
      << ",\"frame\":" << e.frame
      << ",\"interval_ms\":" << e.interval_ms
      << ",\"cap_ms\":" << e.cap_ms
      << ",\"upload_ms\":" << e.upload_ms
      << ",\"present_ms\":" << e.present_ms
      << ",\"outside_ms\":" << e.outside_ms
      << "}";
    push_line(o.str());
  }

  void cluster(const ClusterEvent& c) {
    std::lock_guard<std::mutex> lock(mu_);
    ++cluster_count_;
    if (c.gap_since_prev_cluster_ms > max_gap_ms_) max_gap_ms_ = c.gap_since_prev_cluster_ms;
    std::ostringstream o;
    o << "{\"t_ms\":" << c.t_ms << ",\"type\":\"cluster\""
      << ",\"n\":" << c.n
      << ",\"span_ms\":" << c.span_ms
      << ",\"gap_since_prev_cluster_ms\":" << c.gap_since_prev_cluster_ms
      << ",\"frames_since_prev\":" << c.frames_since_prev
      << ",\"first_frame\":" << c.first_frame
      << "}";
    push_line(o.str());
  }

  void stats_600(const Stats600& s, double t_ms, double dlss_ms = 0, bool dlss_on = false) {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream o;
    o << "{\"t_ms\":" << t_ms << ",\"type\":\"stats_600\""
      << ",\"frames\":" << s.frames
      << ",\"fps\":" << s.fps
      << ",\"mean_interval_ms\":" << s.mean_interval_ms
      << ",\"p50_e2e_ms\":" << s.p50_e2e_ms
      << ",\"p95_e2e_ms\":" << s.p95_e2e_ms
      << ",\"p50_present_ms\":" << s.p50_present_ms
      << ",\"p95_present_ms\":" << s.p95_present_ms
      << ",\"max_interval_ms\":" << s.max_interval_ms
      << ",\"stalls\":" << s.stalls
      << ",\"dlss_on\":" << (dlss_on ? "true" : "false")
      << ",\"dlss_ms\":" << dlss_ms
      << "}";
    push_line(o.str());
  }

  void frame_sample(double t_ms, uint64_t frame, double cap_ms, double upload_ms,
                    double present_ms, double e2e_ms, double interval_ms,
                    uint64_t dropped, double slot_age_ms) {
    if (!verbose_) return;
    std::lock_guard<std::mutex> lock(mu_);
    if ((frame % 30) != 0) return;
    std::ostringstream o;
    o << "{\"t_ms\":" << t_ms << ",\"type\":\"frame\""
      << ",\"frame\":" << frame
      << ",\"cap_ms\":" << cap_ms
      << ",\"upload_ms\":" << upload_ms
      << ",\"present_ms\":" << present_ms
      << ",\"e2e_ms\":" << e2e_ms
      << ",\"qpc_interval_ms\":" << interval_ms
      << ",\"dropped\":" << dropped
      << ",\"slot_age_ms\":" << slot_age_ms
      << "}";
    push_line(o.str());
  }

  void session_end(double t_ms) {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream end;
    end << "{\"t_ms\":" << t_ms << ",\"type\":\"session_end\""
        << ",\"hitch_count\":" << hitch_count_
        << ",\"cluster_count\":" << cluster_count_
        << ",\"max_gap_ms\":" << max_gap_ms_
        << ",\"duration_ms\":" << (t_ms - start_ms_)
        << "}";
    push_line(end.str());
  }

  size_t line_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lines_.size();
  }

  std::vector<std::string> lines_copy() const {
    std::lock_guard<std::mutex> lock(mu_);
    return lines_;
  }

  std::string summary_text(double t_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream f;
    f << "NexusCap analyze summary\n";
    f << "build: " << info_.build << "\n";
    f << "device: " << info_.device << "\n";
    f << "media: " << info_.width << "x" << info_.height << " @" << info_.fps << " " << info_.fourcc << "\n";
    f << "buffering: " << (info_.buffering ? 1 : 0) << " waitable: " << (info_.waitable ? 1 : 0) << "\n";
    f << "dlss: " << (info_.dlss_enabled ? 1 : 0) << " mode: " << info_.dlss_mode
      << " backend: " << info_.dlss_backend << "\n";
    f << "renodx_preset: " << info_.renodx_preset << "\n";
    f << "fg: " << (info_.fg_enabled ? 1 : 0) << " x" << info_.fg_multiplier
      << " backend: " << info_.fg_backend << " status: " << info_.fg_status << "\n";
    f << "duration_ms: " << (t_ms - start_ms_) << "\n";
    f << "hitch_count: " << hitch_count_ << "\n";
    f << "cluster_count: " << cluster_count_ << "\n";
    f << "max_gap_ms: " << max_gap_ms_ << "\n";
    f << "lines: " << lines_.size() << "\n";
    return f.str();
  }

  uint64_t hitch_count() const { return hitch_count_; }
  uint64_t cluster_count() const { return cluster_count_; }

private:
  static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
      if (c == '"' || c == '\\') o.push_back('\\');
      if (c == '\n') { o += "\\n"; continue; }
      o.push_back(c);
    }
    return o;
  }

  void push_line(std::string line) {
    lines_.push_back(std::move(line));
    if (lines_.size() > 20000) lines_.erase(lines_.begin(), lines_.begin() + 1000);
  }

  mutable std::mutex mu_;
  SessionInfo info_;
  double start_ms_ = 0;
  bool verbose_ = false;
  uint64_t hitch_count_ = 0;
  uint64_t cluster_count_ = 0;
  double max_gap_ms_ = 0;
  std::vector<std::string> lines_;
};

bool write_analyze_files(const std::string& dir, const AnalyzeWriter& writer, double t_ms,
                         std::string& jsonl_path, std::string& summary_path);

}  // namespace nexuscap
