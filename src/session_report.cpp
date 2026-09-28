#include "nexuscap/session_report.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace nexuscap {
namespace {

constexpr const char* kAiHint =
    "Paste this file into an AI. Use SETUP + RENODX + TUNABLES + METRICS only.\n"
    "Suggest concrete NexusCap/RenoDX knobs already listed (scale_percent, FG, NR*, preset, sync_image).\n"
    "Do not invent settings outside this app. Prefer values the user can paste back into lobby/RenoDX.";

void append_kv(std::ostringstream& o, const char* k, const std::string& v) { o << k << "=" << v << "\n"; }
void append_kv(std::ostringstream& o, const char* k, int v) { o << k << "=" << v << "\n"; }
void append_kv(std::ostringstream& o, const char* k, double v) { o << k << "=" << v << "\n"; }
void append_kv(std::ostringstream& o, const char* k, bool v) { o << k << "=" << (v ? 1 : 0) << "\n"; }

}  // namespace

std::string read_text_file_utf8(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string extract_keyed_lines(const std::string& text, const char* const* keys, size_t nkeys) {
  std::ostringstream out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    for (size_t i = 0; i < nkeys; ++i) {
      const char* want = keys[i];
      if (!want) continue;
      const size_t wn = std::strlen(want);
      if (key.size() >= wn && key.compare(0, wn, want) == 0) {
        out << line << "\n";
        break;
      }
    }
  }
  return out.str();
}

void collect_renodx_report_snippets(const std::string& exe_dir, std::string& reshade_out,
                                   std::string& feed_out, std::string& mgf_out) {
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  // Report the cfg the active stack loaded (AddonPath), not the root copy.
  const std::string stack_dir = exe_dir + "\\addons-multipass";
  std::string feed_path = stack_dir + "\\dlss5-feed.cfg";
  if (read_text_file_utf8(feed_path).empty()) feed_path = exe_dir + "\\dlss5-feed.cfg";
  const std::string mgf_path = exe_dir + "\\DLSS5MGF.ini";

  static const char* kNrKeys[] = {
      "NeuralUplift", "NRPreset", "NRIntensity", "NRGlobalTone", "NRLocalTone", "NRLocalStructure",
      "NRSkinStructure", "NRAutoMask", "NRUICorrection", "NRColorStrength", "NRTransferStrength",
      "NREnableUpscaling", "NRStyle", "NRToggleKey", "NRDepthMode", "NRDiffuseWhiteNits",
      "NRPaperWhiteScale", "NR"};
  const std::string reshade = read_text_file_utf8(reshade_path);
  if (reshade.empty()) {
    reshade_out = "missing=ReShade.ini\n";
  } else {
    reshade_out = extract_keyed_lines(reshade, kNrKeys, sizeof(kNrKeys) / sizeof(kNrKeys[0]));
    if (reshade_out.empty()) reshade_out = "missing=RenoDX.DLSS5 keys\n";
  }

  static const char* kFeedKeys[] = {"enabled", "mode", "work_resolution", "work_upscale", "work_sharpness",
                                    "hdr"};
  const std::string feed = read_text_file_utf8(feed_path);
  if (feed.empty()) {
    feed_out = "missing=dlss5-feed.cfg\n";
  } else {
    feed_out = extract_keyed_lines(feed, kFeedKeys, sizeof(kFeedKeys) / sizeof(kFeedKeys[0]));
    if (feed_out.empty()) feed_out = "missing=feed keys\n";
  }

  const std::string mgf = read_text_file_utf8(mgf_path);
  if (mgf.empty()) {
    mgf_out = "missing=DLSS5MGF.ini\n";
  } else {
    static const char* kMgfKeys[] = {"Techniques", "TechniqueSorting"};
    mgf_out = extract_keyed_lines(mgf, kMgfKeys, sizeof(kMgfKeys) / sizeof(kMgfKeys[0]));
    if (mgf_out.empty()) mgf_out = "missing=Techniques\n";
  }
}

std::string windows_version_string() {
  OSVERSIONINFOEXW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto rtl = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
  if (rtl) {
    rtl(reinterpret_cast<PRTL_OSVERSIONINFOW>(&vi));
  } else {
#pragma warning(push)
#pragma warning(disable : 4996)
    GetVersionExW(reinterpret_cast<LPOSVERSIONINFOW>(&vi));
#pragma warning(pop)
  }
  char buf[96];
  std::snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu", static_cast<unsigned long>(vi.dwMajorVersion),
                static_cast<unsigned long>(vi.dwMinorVersion),
                static_cast<unsigned long>(vi.dwBuildNumber));
  return buf;
}

void SessionReportWriter::push(std::string line) {
  if (!active_) return;
  lines_.push_back(std::move(line));
  if (lines_.size() > kMaxLines) {
    lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - kMaxLines));
  }
}

void SessionReportWriter::write_header(const SessionReportSnapshot& snap) {
  std::ostringstream o;
  o << "=== NEXUSCAP_" << (kind_ == SessionReportKind::Bug ? "BUG" : "PERF") << "_REPORT ===\n";
  append_kv(o, "build", snap.build);
  append_kv(o, "device", snap.device);
  append_kv(o, "width", snap.width);
  append_kv(o, "height", snap.height);
  append_kv(o, "fps", snap.fps);
  append_kv(o, "scale_percent", snap.scale_percent);
  append_kv(o, "sync_image", snap.sync_image);
  append_kv(o, "fullscreen", snap.fullscreen);
  append_kv(o, "show_fps", snap.show_fps);
  append_kv(o, "mini_overlay", snap.mini_overlay);
  append_kv(o, "advanced_metrics", snap.advanced_metrics);
  append_kv(o, "hw_info", snap.hw_info);
  append_kv(o, "fg_enabled", snap.fg_enabled);
  append_kv(o, "fg_multiplier", snap.fg_multiplier);
  append_kv(o, "neural_rendering", snap.neural_rendering);
  append_kv(o, "nr_runtime_on", snap.nr_runtime_on);
  append_kv(o, "renodx_preset", renodx_visual_preset_name(snap.renodx_preset, snap.nr_pass_count));
  append_kv(o, "video_fourcc", snap.video_fourcc);
  append_kv(o, "color_space", snap.color_space);
  append_kv(o, "color_range", snap.color_range);
  append_kv(o, "capture_buffering", snap.capture_buffering);
  append_kv(o, "output_w", snap.output_w);
  append_kv(o, "output_h", snap.output_h);

  if (kind_ == SessionReportKind::Perf) {
    o << "\n=== SETUP ===\n";
    append_kv(o, "os", snap.os_version.empty() ? std::string("unknown") : snap.os_version);
    append_kv(o, "cpu_name", snap.cpu_name.empty() ? std::string("unknown") : snap.cpu_name);
    append_kv(o, "gpu_name", snap.gpu_name.empty() ? std::string("unknown") : snap.gpu_name);
    append_kv(o, "ram_total_mb", snap.ram_total_mb);
    append_kv(o, "vram_budget_mb", snap.vram_budget_mb);
    append_kv(o, "capture_device", snap.device);
    o << "capture_mode=" << snap.width << "x" << snap.height << "@" << snap.fps << "\n";
    o << "output=" << snap.output_w << "x" << snap.output_h << "\n";

    o << "\n=== RENODX ===\n";
    o << "# ReShade.ini [RenoDX.DLSS5]\n" << snap.reshade_snippet;
    o << "# dlss5-feed.cfg\n" << snap.feed_snippet;
    o << "# DLSS5MGF.ini\n" << snap.mgf_snippet;
    append_kv(o, "nr_runtime_on", snap.nr_runtime_on);
    append_kv(o, "app_renodx_preset", renodx_visual_preset_name(snap.renodx_preset, snap.nr_pass_count));

    o << "\n=== TUNABLES (app) ===\n";
    append_kv(o, "scale_percent", snap.scale_percent);
    append_kv(o, "fg_enabled", snap.fg_enabled);
    append_kv(o, "fg_multiplier", snap.fg_multiplier);
    append_kv(o, "neural_rendering", snap.neural_rendering);
    append_kv(o, "renodx_preset", renodx_visual_preset_name(snap.renodx_preset, snap.nr_pass_count));
    append_kv(o, "sync_image", snap.sync_image);
    append_kv(o, "fullscreen", snap.fullscreen);
    append_kv(o, "mini_overlay", snap.mini_overlay);
    append_kv(o, "advanced_metrics", snap.advanced_metrics);
    append_kv(o, "hw_info", snap.hw_info);
    append_kv(o, "show_fps", snap.show_fps);
    append_kv(o, "video_fourcc", snap.video_fourcc);
    append_kv(o, "color_space", snap.color_space);
    append_kv(o, "color_range", snap.color_range);
    append_kv(o, "capture_buffering", snap.capture_buffering);
    o << "note=RenoDX NR* / feed keys above are also valid AI suggestions\n";

    o << "\n=== METRICS ===\n";
    o << "# latency: e2e=capture→present, present=Present() ms, ft=frame interval\n";
    o << "# heartbeats ~2s include fps e2e present ft_avg ft_p95 (+ p95e/p95p when L600 ready)\n";

    o << "\n=== AI_HINT ===\n";
    o << kAiHint << "\n";
  } else {
    o << "\n=== EXCEPTIONS ===\n";
    o << "# runtime failures as event=exception (present/fg/nr/capture)\n";
  }

  o << "\n=== EVENTS ===\n";
  push(o.str());
}

void SessionReportWriter::start(SessionReportKind kind, const std::string& path,
                                const SessionReportSnapshot& snap) {
  active_ = true;
  kind_ = kind;
  path_ = path;
  lines_.clear();
  last_flush_ms_ = 0;
  last_heartbeat_ms_ = -1e9;
  try {
    fs::create_directories(fs::path(path).parent_path());
  } catch (...) {
  }
  write_header(snap);
  flush();
}

void SessionReportWriter::note_event(double t_ms, const char* event, const char* detail) {
  if (!active_) return;
  char buf[384];
  std::snprintf(buf, sizeof(buf), "t=%.0f event=%s %s", t_ms, event ? event : "?", detail ? detail : "");
  push(buf);
}

void SessionReportWriter::note_exception(double t_ms, const char* where, const char* message) {
  if (!active_) return;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "t=%.0f event=exception where=%s msg=%s", t_ms, where ? where : "?",
                message ? message : "");
  push(buf);
  flush();  // exceptions are rare — persist immediately
}

void SessionReportWriter::heartbeat(double t_ms, double fps_smooth, double e2e_ms, double present_ms,
                                    double ft_avg_ms, double ft_p95_ms, uint64_t dropped,
                                    uint64_t stalls_window, uint64_t hitch_count, bool fg_on, bool nr_on,
                                    double cpu_pct, double gpu_pct, bool have_hw, bool has_s600,
                                    double s600_p95_e2e, double s600_p95_present) {
  if (!active_) return;
  if (t_ms - last_heartbeat_ms_ < kHeartbeatEveryMs) return;
  last_heartbeat_ms_ = t_ms;
  char buf[512];
  if (have_hw && has_s600) {
    std::snprintf(buf, sizeof(buf),
                  "t=%.0f hb fps=%.2f e2e=%.2f present=%.2f ft_avg=%.2f ft_p95=%.2f p95e=%.2f p95p=%.2f "
                  "dropped=%llu stalls=%llu hitch=%llu fg=%d nr=%d cpu=%.0f gpu=%.0f",
                  t_ms, fps_smooth, e2e_ms, present_ms, ft_avg_ms, ft_p95_ms, s600_p95_e2e, s600_p95_present,
                  static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(stalls_window),
                  static_cast<unsigned long long>(hitch_count), fg_on ? 1 : 0, nr_on ? 1 : 0, cpu_pct,
                  gpu_pct);
  } else if (have_hw) {
    std::snprintf(buf, sizeof(buf),
                  "t=%.0f hb fps=%.2f e2e=%.2f present=%.2f ft_avg=%.2f ft_p95=%.2f dropped=%llu stalls=%llu "
                  "hitch=%llu fg=%d nr=%d cpu=%.0f gpu=%.0f",
                  t_ms, fps_smooth, e2e_ms, present_ms, ft_avg_ms, ft_p95_ms,
                  static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(stalls_window),
                  static_cast<unsigned long long>(hitch_count), fg_on ? 1 : 0, nr_on ? 1 : 0, cpu_pct,
                  gpu_pct);
  } else if (has_s600) {
    std::snprintf(buf, sizeof(buf),
                  "t=%.0f hb fps=%.2f e2e=%.2f present=%.2f ft_avg=%.2f ft_p95=%.2f p95e=%.2f p95p=%.2f "
                  "dropped=%llu stalls=%llu hitch=%llu fg=%d nr=%d",
                  t_ms, fps_smooth, e2e_ms, present_ms, ft_avg_ms, ft_p95_ms, s600_p95_e2e, s600_p95_present,
                  static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(stalls_window),
                  static_cast<unsigned long long>(hitch_count), fg_on ? 1 : 0, nr_on ? 1 : 0);
  } else {
    std::snprintf(buf, sizeof(buf),
                  "t=%.0f hb fps=%.2f e2e=%.2f present=%.2f ft_avg=%.2f ft_p95=%.2f dropped=%llu stalls=%llu "
                  "hitch=%llu fg=%d nr=%d",
                  t_ms, fps_smooth, e2e_ms, present_ms, ft_avg_ms, ft_p95_ms,
                  static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(stalls_window),
                  static_cast<unsigned long long>(hitch_count), fg_on ? 1 : 0, nr_on ? 1 : 0);
  }
  push(buf);
  if (t_ms - last_flush_ms_ >= kFlushEveryMs) flush();
}

void SessionReportWriter::end(double t_ms, uint64_t frames, uint64_t dropped, uint64_t hitch_count,
                              uint64_t cluster_count) {
  if (!active_) return;
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "=== FOOTER ===\nt=%.0f frames=%llu dropped=%llu hitch=%llu clusters=%llu\n", t_ms,
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(hitch_count), static_cast<unsigned long long>(cluster_count));
  push(buf);
  flush();
  active_ = false;
}

void SessionReportWriter::flush() {
  if (!active_ || path_.empty()) return;
  std::ofstream out(path_, std::ios::binary | std::ios::trunc);
  if (!out) return;
  for (const auto& line : lines_) out << line << (line.empty() || line.back() == '\n' ? "" : "\n");
  last_flush_ms_ = last_heartbeat_ms_ > 0 ? last_heartbeat_ms_ : last_flush_ms_;
}

}  // namespace nexuscap
