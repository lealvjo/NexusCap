#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

struct ID3D11Device;

namespace nexuscap {

struct SystemMetricsSample {
  bool cpu_ok = false;
  double cpu_percent = 0;
  bool cpu_temp_ok = false;
  double cpu_temp_c = 0;
  bool gpu_ok = false;
  double gpu_percent = 0;
  bool gpu_temp_ok = false;
  double gpu_temp_c = 0;
  bool vram_ok = false;
  double vram_used_mb = 0;
  double vram_budget_mb = 0;
  bool ram_ok = false;
  double ram_used_mb = 0;
  double ram_total_mb = 0;
  bool power_ok = false;
  double power_w = 0;
};

// Pure: FILETIME 64-bit idle/kernel/user deltas → CPU % (0–100).
inline double cpu_percent_from_idle_deltas(unsigned long long idle_d, unsigned long long kernel_d,
                                          unsigned long long user_d) {
  const unsigned long long total = kernel_d + user_d;
  if (total == 0) return 0.0;
  // kernel includes idle time on Windows.
  const unsigned long long busy = (total > idle_d) ? (total - idle_d) : 0ull;
  double pct = 100.0 * static_cast<double>(busy) / static_cast<double>(total);
  if (pct < 0.0) pct = 0.0;
  if (pct > 100.0) pct = 100.0;
  return pct;
}

// ACPI/PDH thermal zone values arrive as Kelvin or tenths-of-Kelvin.
inline double celsius_from_thermal_kelvin_raw(double raw) {
  if (raw <= 0.0) return 0.0;
  // Tenths of Kelvin (e.g. 3100 → 36.85 °C).
  if (raw > 400.0) return raw / 10.0 - 273.15;
  return raw - 273.15;
}

inline const char* sync_image_label(bool sync_image) { return sync_image ? "vsync" : "tear"; }

inline const char* sync_image_badge(bool sync_image) {
  return sync_image ? "SYNC VSYNC" : "SYNC TEAR";
}

// How to verify sync without trusting the checkbox alone.
inline const char* sync_image_how_to_see(bool sync_image) {
  return sync_image ? "Present(1): FPS trava no Hz do monitor; present_ms sobe (~16ms@60)"
                    : "Present(0): FPS livre / tearing possivel; present_ms baixo";
}

inline void format_sys_cpu_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  if (s.cpu_ok && s.cpu_temp_ok) {
    std::snprintf(out, n, "CPU  %.0f%%  %.0fC", s.cpu_percent, s.cpu_temp_c);
  } else if (s.cpu_ok) {
    std::snprintf(out, n, "CPU  %.0f%%", s.cpu_percent);
  } else if (s.cpu_temp_ok) {
    std::snprintf(out, n, "CPU  --  %.0fC", s.cpu_temp_c);
  } else {
    std::snprintf(out, n, "CPU  --");
  }
}

inline void format_sys_gpu_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  if (s.gpu_ok && s.gpu_temp_ok) {
    std::snprintf(out, n, "GPU  %.0f%%  %.0fC", s.gpu_percent, s.gpu_temp_c);
  } else if (s.gpu_ok) {
    std::snprintf(out, n, "GPU  %.0f%%", s.gpu_percent);
  } else if (s.gpu_temp_ok) {
    std::snprintf(out, n, "GPU  --  %.0fC", s.gpu_temp_c);
  } else {
    std::snprintf(out, n, "GPU  --");
  }
}

inline void format_sys_vram_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  if (s.vram_ok && s.vram_budget_mb > 0.5) {
    std::snprintf(out, n, "VRAM %.1f / %.1f GB", s.vram_used_mb / 1024.0, s.vram_budget_mb / 1024.0);
  } else if (s.vram_ok) {
    std::snprintf(out, n, "VRAM %.0f MB", s.vram_used_mb);
  } else {
    std::snprintf(out, n, "VRAM --");
  }
}

inline void format_sys_ram_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  if (s.ram_ok && s.ram_total_mb > 0.5) {
    std::snprintf(out, n, "RAM  %.1f / %.1f GB", s.ram_used_mb / 1024.0, s.ram_total_mb / 1024.0);
  } else if (s.ram_ok) {
    std::snprintf(out, n, "RAM  %.0f MB", s.ram_used_mb);
  } else {
    std::snprintf(out, n, "RAM  --");
  }
}

inline void format_sys_power_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  if (s.power_ok) {
    std::snprintf(out, n, "PWR  %.0f W", s.power_w);
  } else {
    std::snprintf(out, n, "PWR  --");
  }
}

// Single ADV HUD line (legacy / tests): CPU(+C) + GPU(+C) + RAM + VRAM + PWR.
inline void format_sys_metrics_line(char* out, size_t n, const SystemMetricsSample& s) {
  if (!out || n == 0) return;
  char cpu[40], gpu[40], ram[48], vram[48], pwr[32];
  format_sys_cpu_line(cpu, sizeof(cpu), s);
  format_sys_gpu_line(gpu, sizeof(gpu), s);
  format_sys_ram_line(ram, sizeof(ram), s);
  format_sys_vram_line(vram, sizeof(vram), s);
  format_sys_power_line(pwr, sizeof(pwr), s);
  std::snprintf(out, n, "%s   %s   %s   %s   %s", cpu, gpu, ram, vram, pwr);
}

// One metric per line (HUD / preview). Returns how many lines written (always 5).
inline constexpr int kSysMetricsLineCount = 5;

inline int format_sys_metrics_lines(char (*lines)[64], int max_lines, const SystemMetricsSample& s) {
  if (!lines || max_lines <= 0) return 0;
  const int n = (std::min)(kSysMetricsLineCount, max_lines);
  if (n > 0) format_sys_cpu_line(lines[0], 64, s);
  if (n > 1) format_sys_gpu_line(lines[1], 64, s);
  if (n > 2) format_sys_ram_line(lines[2], 64, s);
  if (n > 3) format_sys_vram_line(lines[3], 64, s);
  if (n > 4) format_sys_power_line(lines[4], 64, s);
  return n;
}

class SystemMetricsSampler {
public:
  SystemMetricsSampler();
  ~SystemMetricsSampler();

  SystemMetricsSampler(const SystemMetricsSampler&) = delete;
  SystemMetricsSampler& operator=(const SystemMetricsSampler&) = delete;

  // Throttled (~2 Hz). Pass device for DXGI VRAM; may be null.
  void sample(ID3D11Device* device, SystemMetricsSample& out);

  const SystemMetricsSample& last() const { return last_; }

private:
  bool sample_cpu(SystemMetricsSample& out);
  bool sample_cpu_temp_pdh(SystemMetricsSample& out);
  bool sample_gpu_pdh(SystemMetricsSample& out);
  bool sample_vram(ID3D11Device* device, SystemMetricsSample& out);
  bool sample_ram(SystemMetricsSample& out);
  bool sample_nvml(SystemMetricsSample& out);

  SystemMetricsSample last_{};
  unsigned long long prev_idle_ = 0;
  unsigned long long prev_kernel_ = 0;
  unsigned long long prev_user_ = 0;
  bool cpu_have_prev_ = false;

  void* pdh_query_ = nullptr;   // PDH_HQUERY
  void* pdh_counter_ = nullptr; // PDH_HCOUNTER
  bool pdh_ready_ = false;
  bool pdh_tried_ = false;

  void* pdh_temp_query_ = nullptr;
  void* pdh_temp_counter_ = nullptr;
  bool pdh_temp_ready_ = false;
  bool pdh_temp_tried_ = false;

  void* nvml_lib_ = nullptr;
  bool nvml_ready_ = false;
  bool nvml_tried_ = false;
  int nvml_device_index_ = 0;

  double last_sample_ms_ = 0;
};

}  // namespace nexuscap
