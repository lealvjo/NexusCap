#pragma once
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "nexuscap/settings_store.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/version.hpp"

namespace nexuscap {

enum class SessionReportKind { Bug, Perf };

inline const char* session_report_subdir(SessionReportKind k) {
  return k == SessionReportKind::Bug ? "report" : "performance";
}

inline const char* session_report_file_prefix(SessionReportKind k) {
  return k == SessionReportKind::Bug ? "bug" : "performance";
}

// bug_20260916_210530.txt / performance_20260916_210530.txt
inline std::string make_session_report_filename(SessionReportKind k, int y, int mon, int d, int h, int mi,
                                               int sec) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%s_%04d%02d%02d_%02d%02d%02d.txt", session_report_file_prefix(k), y, mon, d,
                h, mi, sec);
  return buf;
}

inline std::string make_session_report_path(const std::string& exe_dir, SessionReportKind k, int y, int mon,
                                           int d, int h, int mi, int sec) {
  std::string base = exe_dir;
  while (!base.empty() && (base.back() == '/' || base.back() == '\\')) base.pop_back();
  return base + "\\" + session_report_subdir(k) + "\\" +
         make_session_report_filename(k, y, mon, d, h, mi, sec);
}

inline bool hud_hotkey_fired(int vk, bool ctrl_down, int vk_hud, bool hud_ctrl) {
  if (vk != vk_hud) return false;
  if (hud_ctrl && !ctrl_down) return false;
  return true;
}

// Ctrl+M na captura abre o CFG. Mesma regra do HUD, tecla própria.
inline bool cfg_hotkey_fired(int vk, bool ctrl_down, int vk_cfg, bool cfg_ctrl) {
  if (vk != vk_cfg) return false;
  if (cfg_ctrl && !ctrl_down) return false;
  return true;
}

struct SessionReportSnapshot {
  std::string build = kAppAnalyzeBuild;
  std::string device;
  int width = 0;
  int height = 0;
  double fps = 60;
  int scale_percent = 100;
  bool sync_image = true;
  bool fullscreen = true;
  bool show_fps = true;
  bool mini_overlay = false;
  bool advanced_metrics = false;
  bool hw_info = false;
  bool fg_enabled = false;
  int fg_multiplier = 1;
  bool neural_rendering = true;
  bool nr_runtime_on = false;
  int renodx_preset = 0;
  int nr_pass_count = 1;
  int video_fourcc = 0;
  int color_space = 0;
  int color_range = 0;
  int capture_buffering = 0;
  int output_w = 0;
  int output_h = 0;
  std::string os_version;
  std::string cpu_name;
  std::string gpu_name;
  double ram_total_mb = 0;
  double vram_budget_mb = 0;
  std::string reshade_snippet;  // KEY=value lines from ReShade.ini
  std::string feed_snippet;
  std::string mgf_snippet;
};

class SessionReportWriter {
public:
  void start(SessionReportKind kind, const std::string& path, const SessionReportSnapshot& snap);
  void note_event(double t_ms, const char* event, const char* detail = "");
  // Bug report: falhas / erros de runtime (FG, NR, present, etc.).
  void note_exception(double t_ms, const char* where, const char* message);
  void heartbeat(double t_ms, double fps_smooth, double e2e_ms, double present_ms, double ft_avg_ms,
                 double ft_p95_ms, uint64_t dropped, uint64_t stalls_window, uint64_t hitch_count, bool fg_on,
                 bool nr_on, double cpu_pct, double gpu_pct, bool have_hw, bool has_s600,
                 double s600_p95_e2e, double s600_p95_present);
  void end(double t_ms, uint64_t frames, uint64_t dropped, uint64_t hitch_count, uint64_t cluster_count);
  void flush();
  bool active() const { return active_; }
  const std::string& path() const { return path_; }
  SessionReportKind kind() const { return kind_; }

private:
  void push(std::string line);
  void write_header(const SessionReportSnapshot& snap);

  bool active_ = false;
  SessionReportKind kind_{SessionReportKind::Bug};
  std::string path_;
  std::vector<std::string> lines_;
  double last_flush_ms_ = 0;
  double last_heartbeat_ms_ = -1e9;
  static constexpr size_t kMaxLines = 4000;
  static constexpr double kFlushEveryMs = 5000.0;
  static constexpr double kHeartbeatEveryMs = 2000.0;
};

// Pull KEY= lines whose key starts with any of the prefixes (case-sensitive).
std::string extract_keyed_lines(const std::string& text, const char* const* keys, size_t nkeys);

// Read file to string; empty if missing.
std::string read_text_file_utf8(const std::string& path);

// Collect RenoDX-related snippets from exe_dir (missing files → "missing=...").
void collect_renodx_report_snippets(const std::string& exe_dir, std::string& reshade_out,
                                   std::string& feed_out, std::string& mgf_out);

std::string windows_version_string();

}  // namespace nexuscap
