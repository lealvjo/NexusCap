#include "nexuscap/system_metrics.hpp"
#include "nexuscap/clock.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "dxgi.lib")

namespace nexuscap {
namespace {

unsigned long long filetime_u64(const FILETIME& ft) {
  return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
         static_cast<unsigned long long>(ft.dwLowDateTime);
}

using NvmlReturn_t = int;
using NvmlDevice_t = void*;
using NvmlInit_t = NvmlReturn_t (*)();
using NvmlShutdown_t = NvmlReturn_t (*)();
using NvmlDeviceGetCount_t = NvmlReturn_t (*)(unsigned int*);
using NvmlDeviceGetHandleByIndex_t = NvmlReturn_t (*)(unsigned int, NvmlDevice_t*);
using NvmlDeviceGetPowerUsage_t = NvmlReturn_t (*)(NvmlDevice_t, unsigned int*);
using NvmlDeviceGetPowerLimit_t = NvmlReturn_t (*)(NvmlDevice_t, unsigned int*);
using NvmlDeviceGetUtilizationRates_t = NvmlReturn_t (*)(NvmlDevice_t, void*);
using NvmlDeviceGetTemperature_t = NvmlReturn_t (*)(NvmlDevice_t, int /*sensor*/, unsigned int*);

constexpr int kNvmlTempGpu = 0;  // NVML_TEMPERATURE_GPU

struct NvmlUtilization {
  unsigned int gpu;
  unsigned int memory;
};

HMODULE try_load_nvml() {
  const wchar_t* candidates[] = {
      L"nvml.dll",
      L"C:\\Windows\\System32\\nvml.dll",
      L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll",
      L"C:\\Windows\\SysWOW64\\nvml.dll",
  };
  for (const wchar_t* p : candidates) {
    HMODULE m = LoadLibraryW(p);
    if (m) return m;
  }
  return nullptr;
}

bool vram_from_adapter(IDXGIAdapter* adapter, SystemMetricsSample& out) {
  if (!adapter) return false;
  IDXGIAdapter3* adapter3 = nullptr;
  if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) ||
      !adapter3) {
    return false;
  }
  DXGI_QUERY_VIDEO_MEMORY_INFO info{};
  const HRESULT hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
  adapter3->Release();
  if (FAILED(hr)) return false;
  out.vram_used_mb = static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
  out.vram_budget_mb = static_cast<double>(info.Budget) / (1024.0 * 1024.0);
  out.vram_ok = true;
  return true;
}

std::string wide_to_utf8(const wchar_t* w) {
  if (!w || !*w) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string read_cpu_name_registry() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                    KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t buf[256]{};
  DWORD type = 0;
  DWORD bytes = sizeof(buf);
  const LONG st = RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type, reinterpret_cast<LPBYTE>(buf),
                                   &bytes);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
  return wide_to_utf8(buf);
}

bool is_software_adapter(const DXGI_ADAPTER_DESC& desc) {
  return desc.VendorId == 0x1414 && desc.DeviceId == 0x8c;  // Microsoft Basic Render
}

std::string gpu_name_from_adapter(IDXGIAdapter* adapter) {
  if (!adapter) return {};
  DXGI_ADAPTER_DESC desc{};
  if (FAILED(adapter->GetDesc(&desc))) return {};
  if (is_software_adapter(desc)) return {};
  return wide_to_utf8(desc.Description);
}

std::string resolve_gpu_name(ID3D11Device* device) {
  if (device) {
    IDXGIDevice* dxgi_dev = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev))) &&
        dxgi_dev) {
      IDXGIAdapter* adapter = nullptr;
      if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
        std::string name = gpu_name_from_adapter(adapter);
        adapter->Release();
        dxgi_dev->Release();
        if (!name.empty()) return name;
      } else {
        dxgi_dev->Release();
      }
    }
  }
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    return {};
  }
  std::string name;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    name = gpu_name_from_adapter(adapter);
    adapter->Release();
    if (!name.empty()) break;
  }
  factory->Release();
  return name;
}

}  // namespace

void SystemMetricsSampler::resolve_hw_names(ID3D11Device* device, SystemMetricsSample& out) {
  if (names_resolved_ && !out.cpu_name.empty() && !out.gpu_name.empty()) return;
  if (out.cpu_name.empty()) {
    out.cpu_name = shorten_cpu_display_name(read_cpu_name_registry());
  }
  if (out.gpu_name.empty()) {
    out.gpu_name = shorten_gpu_display_name(resolve_gpu_name(device));
  }
  names_resolved_ = !out.cpu_name.empty() || !out.gpu_name.empty();
}

SystemMetricsSampler::SystemMetricsSampler() {
  // Prime CPU baselines so the first real sample (~0.5s later) already has a delta.
  FILETIME idle{}, kernel{}, user{};
  if (GetSystemTimes(&idle, &kernel, &user)) {
    prev_idle_ = filetime_u64(idle);
    prev_kernel_ = filetime_u64(kernel);
    prev_user_ = filetime_u64(user);
    cpu_have_prev_ = true;
  }
}

SystemMetricsSampler::~SystemMetricsSampler() {
  if (pdh_query_) {
    PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_query_));
    pdh_query_ = nullptr;
    pdh_counter_ = nullptr;
  }
  if (pdh_temp_query_) {
    PdhCloseQuery(static_cast<PDH_HQUERY>(pdh_temp_query_));
    pdh_temp_query_ = nullptr;
    pdh_temp_counter_ = nullptr;
  }
  if (nvml_lib_) {
    auto shutdown =
        reinterpret_cast<NvmlShutdown_t>(GetProcAddress(static_cast<HMODULE>(nvml_lib_), "nvmlShutdown"));
    if (shutdown) shutdown();
    FreeLibrary(static_cast<HMODULE>(nvml_lib_));
    nvml_lib_ = nullptr;
  }
}

bool SystemMetricsSampler::sample_cpu(SystemMetricsSample& out) {
  FILETIME idle{}, kernel{}, user{};
  if (!GetSystemTimes(&idle, &kernel, &user)) return false;
  const unsigned long long i = filetime_u64(idle);
  const unsigned long long k = filetime_u64(kernel);
  const unsigned long long u = filetime_u64(user);
  if (!cpu_have_prev_) {
    prev_idle_ = i;
    prev_kernel_ = k;
    prev_user_ = u;
    cpu_have_prev_ = true;
    return false;
  }
  const unsigned long long idle_d = i - prev_idle_;
  const unsigned long long kernel_d = k - prev_kernel_;
  const unsigned long long user_d = u - prev_user_;
  prev_idle_ = i;
  prev_kernel_ = k;
  prev_user_ = u;
  out.cpu_percent = cpu_percent_from_idle_deltas(idle_d, kernel_d, user_d);
  out.cpu_ok = true;
  return true;
}

bool SystemMetricsSampler::sample_cpu_temp_pdh(SystemMetricsSample& out) {
  if (!pdh_temp_tried_) {
    pdh_temp_tried_ = true;
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
      pdh_temp_ready_ = false;
      return false;
    }
    PDH_HCOUNTER c = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\Temperature", 0, &c);
    if (st != ERROR_SUCCESS) {
      st = PdhAddCounterW(q, L"\\Thermal Zone Information(*)\\Temperature", 0, &c);
    }
    if (st != ERROR_SUCCESS) {
      // High-precision variant on some Win10/11 builds.
      st = PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\High Precision Temperature", 0, &c);
    }
    if (st != ERROR_SUCCESS) {
      PdhCloseQuery(q);
      pdh_temp_ready_ = false;
      return false;
    }
    PdhCollectQueryData(q);
    pdh_temp_query_ = q;
    pdh_temp_counter_ = c;
    pdh_temp_ready_ = true;
  }
  if (!pdh_temp_ready_ || !pdh_temp_query_ || !pdh_temp_counter_) return false;

  if (PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_temp_query_)) != ERROR_SUCCESS) return false;

  DWORD buf_size = 0;
  DWORD item_count = 0;
  PDH_STATUS st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdh_temp_counter_), PDH_FMT_DOUBLE,
                                               &buf_size, &item_count, nullptr);
  if (st != PDH_MORE_DATA && st != ERROR_SUCCESS) return false;
  if (buf_size == 0) return false;

  std::vector<char> buf(buf_size);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
  st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdh_temp_counter_), PDH_FMT_DOUBLE, &buf_size,
                                    &item_count, items);
  if (st != ERROR_SUCCESS || item_count == 0) return false;

  double best_c = -1000.0;
  for (DWORD i = 0; i < item_count; ++i) {
    if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
    const double c = celsius_from_thermal_kelvin_raw(items[i].FmtValue.doubleValue);
    // Plausible package/zone range; ignore bogus ACPI zeros.
    if (c >= 10.0 && c <= 125.0) best_c = (std::max)(best_c, c);
  }
  if (best_c < 0.0) return false;
  out.cpu_temp_c = best_c;
  out.cpu_temp_ok = true;
  return true;
}

bool SystemMetricsSampler::sample_gpu_pdh(SystemMetricsSample& out) {
  if (!pdh_tried_) {
    pdh_tried_ = true;
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) {
      pdh_ready_ = false;
      return false;
    }
    PDH_HCOUNTER c = nullptr;
    PDH_STATUS st = PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c);
    if (st != ERROR_SUCCESS) {
      // Fallback: localized counter path (PT-BR / etc).
      st = PdhAddCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage", 0, &c);
    }
    if (st != ERROR_SUCCESS) {
      PdhCloseQuery(q);
      pdh_ready_ = false;
      return false;
    }
    PdhCollectQueryData(q);  // prime (often invalid until 2nd collect)
    pdh_query_ = q;
    pdh_counter_ = c;
    pdh_ready_ = true;
  }
  if (!pdh_ready_ || !pdh_query_ || !pdh_counter_) return false;

  if (PdhCollectQueryData(static_cast<PDH_HQUERY>(pdh_query_)) != ERROR_SUCCESS) return false;

  DWORD buf_size = 0;
  DWORD item_count = 0;
  PDH_STATUS st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdh_counter_), PDH_FMT_DOUBLE,
                                               &buf_size, &item_count, nullptr);
  if (st != PDH_MORE_DATA && st != ERROR_SUCCESS) return false;
  if (buf_size == 0) return false;

  std::vector<char> buf(buf_size);
  auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
  st = PdhGetFormattedCounterArrayW(static_cast<PDH_HCOUNTER>(pdh_counter_), PDH_FMT_DOUBLE, &buf_size,
                                    &item_count, items);
  if (st != ERROR_SUCCESS || item_count == 0) return false;

  double best_3d = -1.0;
  double best_any = -1.0;
  for (DWORD i = 0; i < item_count; ++i) {
    if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
    const wchar_t* name = items[i].szName ? items[i].szName : L"";
    const double v = items[i].FmtValue.doubleValue;
    best_any = (std::max)(best_any, v);
    if (wcsstr(name, L"engtype_3D") != nullptr || wcsstr(name, L"3D") != nullptr) {
      best_3d = (std::max)(best_3d, v);
    }
  }
  const double best = best_3d >= 0.0 ? best_3d : best_any;
  if (best < 0.0) return false;

  out.gpu_percent = (std::min)(100.0, (std::max)(0.0, best));
  out.gpu_ok = true;
  return true;
}

bool SystemMetricsSampler::sample_vram(ID3D11Device* device, SystemMetricsSample& out) {
  if (device) {
    IDXGIDevice* dxgi_dev = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_dev))) &&
        dxgi_dev) {
      IDXGIAdapter* adapter = nullptr;
      if (SUCCEEDED(dxgi_dev->GetAdapter(&adapter)) && adapter) {
        const bool ok = vram_from_adapter(adapter, out);
        adapter->Release();
        dxgi_dev->Release();
        if (ok) return true;
      } else {
        dxgi_dev->Release();
      }
    }
  }

  // Fallback: first hardware adapter via DXGI factory.
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
    return false;
  }
  bool ok = false;
  for (UINT i = 0;; ++i) {
    IDXGIAdapter* adapter = nullptr;
    if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    const bool is_soft = is_software_adapter(desc);
    if (!is_soft && vram_from_adapter(adapter, out)) {
      ok = true;
      adapter->Release();
      break;
    }
    adapter->Release();
  }
  factory->Release();
  return ok;
}

bool SystemMetricsSampler::sample_ram(SystemMetricsSample& out) {
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (!GlobalMemoryStatusEx(&ms)) return false;
  out.ram_total_mb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0);
  out.ram_used_mb = static_cast<double>(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0);
  out.ram_ok = true;
  return true;
}

bool SystemMetricsSampler::sample_nvml(SystemMetricsSample& out) {
  if (!nvml_tried_) {
    nvml_tried_ = true;
    HMODULE m = try_load_nvml();
    if (!m) {
      nvml_ready_ = false;
      return false;
    }
    auto init = reinterpret_cast<NvmlInit_t>(GetProcAddress(m, "nvmlInit_v2"));
    if (!init) init = reinterpret_cast<NvmlInit_t>(GetProcAddress(m, "nvmlInit"));
    auto get_count = reinterpret_cast<NvmlDeviceGetCount_t>(GetProcAddress(m, "nvmlDeviceGetCount_v2"));
    if (!get_count)
      get_count = reinterpret_cast<NvmlDeviceGetCount_t>(GetProcAddress(m, "nvmlDeviceGetCount"));
    auto get_handle =
        reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2"));
    if (!get_handle)
      get_handle =
          reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex"));
    auto get_power =
        reinterpret_cast<NvmlDeviceGetPowerUsage_t>(GetProcAddress(m, "nvmlDeviceGetPowerUsage"));
    if (!init || !get_count || !get_handle || !get_power || init() != 0) {
      FreeLibrary(m);
      nvml_ready_ = false;
      return false;
    }
    unsigned int count = 0;
    if (get_count(&count) != 0 || count == 0) {
      auto shutdown = reinterpret_cast<NvmlShutdown_t>(GetProcAddress(m, "nvmlShutdown"));
      if (shutdown) shutdown();
      FreeLibrary(m);
      nvml_ready_ = false;
      return false;
    }
    nvml_lib_ = m;
    nvml_device_index_ = 0;
    nvml_ready_ = true;
  }
  if (!nvml_ready_ || !nvml_lib_) return false;

  HMODULE m = static_cast<HMODULE>(nvml_lib_);
  auto get_handle =
      reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2"));
  if (!get_handle)
    get_handle =
        reinterpret_cast<NvmlDeviceGetHandleByIndex_t>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex"));
  auto get_power = reinterpret_cast<NvmlDeviceGetPowerUsage_t>(GetProcAddress(m, "nvmlDeviceGetPowerUsage"));
  auto get_util =
      reinterpret_cast<NvmlDeviceGetUtilizationRates_t>(GetProcAddress(m, "nvmlDeviceGetUtilizationRates"));
  auto get_temp =
      reinterpret_cast<NvmlDeviceGetTemperature_t>(GetProcAddress(m, "nvmlDeviceGetTemperature"));
  if (!get_handle || !get_power) return false;

  NvmlDevice_t dev = nullptr;
  if (get_handle(static_cast<unsigned int>(nvml_device_index_), &dev) != 0 || !dev) return false;

  unsigned int mw = 0;
  if (get_power(dev, &mw) == 0) {
    out.power_w = static_cast<double>(mw) / 1000.0;
    out.power_ok = true;
  }

  if (!power_limit_tried_) {
    power_limit_tried_ = true;
    auto get_limit = reinterpret_cast<NvmlDeviceGetPowerLimit_t>(
        GetProcAddress(m, "nvmlDeviceGetEnforcedPowerLimit"));
    if (!get_limit) {
      get_limit = reinterpret_cast<NvmlDeviceGetPowerLimit_t>(
          GetProcAddress(m, "nvmlDeviceGetPowerManagementLimit"));
    }
    unsigned int limit_mw = 0;
    if (get_limit && get_limit(dev, &limit_mw) == 0 && limit_mw > 0) {
      power_limit_w_ = static_cast<double>(limit_mw) / 1000.0;
      power_limit_ok_ = true;
    }
  }
  if (power_limit_ok_) {
    out.power_limit_w = power_limit_w_;
    out.power_limit_ok = true;
  }

  // NVML GPU util is often more reliable than PDH for a single discrete GPU.
  if (get_util) {
    NvmlUtilization u{};
    if (get_util(dev, &u) == 0) {
      out.gpu_percent = static_cast<double>(u.gpu);
      out.gpu_ok = true;
    }
  }

  if (get_temp) {
    unsigned int tc = 0;
    if (get_temp(dev, kNvmlTempGpu, &tc) == 0 && tc > 0 && tc < 130) {
      out.gpu_temp_c = static_cast<double>(tc);
      out.gpu_temp_ok = true;
    }
  }
  return out.power_ok || out.gpu_ok || out.gpu_temp_ok;
}

void SystemMetricsSampler::sample(ID3D11Device* device, SystemMetricsSample& out) {
  QpcClock clk;
  const double now = clk.now_ms();
  if (last_sample_ms_ > 0 && (now - last_sample_ms_) < 400.0) {
    out = last_;
    return;
  }
  last_sample_ms_ = now;

  // Keep last good values; only overwrite fields that succeed this tick.
  SystemMetricsSample s = last_;
  resolve_hw_names(device, s);
  sample_cpu(s);
  sample_cpu_temp_pdh(s);
  sample_ram(s);
  sample_vram(device, s);
  sample_nvml(s);  // GPU% + GPU temp + PWR (NVIDIA)
  if (!s.gpu_ok) sample_gpu_pdh(s);
  last_ = s;
  out = s;
}

}  // namespace nexuscap
