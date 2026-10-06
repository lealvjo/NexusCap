#include "nexuscap/max_perf.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace nexuscap {
namespace {

constexpr unsigned kPreferredPstateId = 0x1057EB71u;
constexpr unsigned kPreferMax = 1u;

constexpr unsigned kNvInitialize = 0x0150e828u;
constexpr unsigned kNvDrsCreateSession = 0x0694d52eu;
constexpr unsigned kNvDrsDestroySession = 0xdad9cff8u;
constexpr unsigned kNvDrsLoadSettings = 0x375dbd6bu;
constexpr unsigned kNvDrsSaveSettings = 0xfcbc7e14u;
constexpr unsigned kNvDrsCreateProfile = 0xcc176068u;
constexpr unsigned kNvDrsFindProfileByName = 0x7e4a9a0bu;
constexpr int kNvProfileNameInUse = -164;
constexpr int kNvExecutableAlreadyInUse = -167;
constexpr unsigned kNvDrsCreateApplication = 0x4347a9deu;
constexpr unsigned kNvDrsFindApplicationByName = 0xeee566b2u;
constexpr unsigned kNvDrsSetSetting = 0x577dd202u;
constexpr unsigned kNvDrsDeleteProfileSetting = 0xe4a26362u;

constexpr int kNvOk = 0;
constexpr int kNvSettingNotFound = -160;
constexpr int kNvProfileNotFound = -163;
constexpr int kNvExecutableNotFound = -166;

constexpr unsigned kUnicodeMax = 2048;
constexpr unsigned kBinaryMax = 4096;

using NvStatus = int;
using QueryFn = void*(__cdecl*)(unsigned id);
using InitFn = NvStatus(__cdecl*)();
using SessionFn = NvStatus(__cdecl*)(void** session);
using SessionOnlyFn = NvStatus(__cdecl*)(void* session);
using CreateProfileFn = NvStatus(__cdecl*)(void* session, void* profile, void** out_profile);
using CreateAppFn = NvStatus(__cdecl*)(void* session, void* profile, void* app);
using FindAppFn = NvStatus(__cdecl*)(void* session, unsigned short* name, void** profile, void* app);
using FindProfileFn = NvStatus(__cdecl*)(void* session, unsigned short* name, void** profile);
using SetSettingFn = NvStatus(__cdecl*)(void* session, void* profile, void* setting);
using DeleteSettingFn = NvStatus(__cdecl*)(void* session, void* profile, unsigned setting_id);

#define NC_NV_VER(type_, ver_) (static_cast<unsigned>(sizeof(type_)) | (static_cast<unsigned>(ver_) << 16))

struct NvBinary {
  unsigned value_length;
  unsigned char value_data[kBinaryMax];
};

#pragma pack(push, 4)
struct NvSetting {
  unsigned version;
  unsigned short setting_name[kUnicodeMax];
  unsigned setting_id;
  unsigned setting_type;
  unsigned setting_location;
  unsigned is_current_predefined;
  unsigned is_predefined_valid;
  union {
    unsigned u32_predefined;
    NvBinary binary_predefined;
    unsigned short wsz_predefined[kUnicodeMax];
    unsigned long long u64_predefined;
  };
  union {
    unsigned u32_current;
    NvBinary binary_current;
    unsigned short wsz_current[kUnicodeMax];
    unsigned long long u64_current;
  };
};
#pragma pack(pop)

struct NvApplication {
  unsigned version = 0;
  unsigned is_predefined = 0;
  unsigned short app_name[kUnicodeMax]{};
  unsigned short user_friendly[kUnicodeMax]{};
  unsigned short launcher[kUnicodeMax]{};
  unsigned short file_in_folder[kUnicodeMax]{};
  unsigned flags = 0;
  unsigned short command_line[kUnicodeMax]{};
};

struct NvProfile {
  unsigned version = 0;
  unsigned short profile_name[kUnicodeMax]{};
  unsigned gpu_support = 0;
  unsigned is_predefined = 0;
  unsigned num_of_apps = 0;
  unsigned num_of_settings = 0;
};

struct State {
  bool wanted = false;
  bool timer_on = false;
  bool cpu_ok = true;
  bool gpu_ok = true;
  bool throttling_saved = false;
  PROCESS_POWER_THROTTLING_STATE throttling_before{};
  CRITICAL_SECTION lock{};
  bool lock_ready = false;
};

State& st() {
  static State s;
  if (!s.lock_ready) {
    InitializeCriticalSection(&s.lock);
    s.lock_ready = true;
  }
  return s;
}

void copy_w(unsigned short* dst, const wchar_t* src) {
  std::memset(dst, 0, kUnicodeMax * sizeof(unsigned short));
  if (!src) return;
  for (unsigned i = 0; i + 1 < kUnicodeMax && src[i]; ++i) dst[i] = static_cast<unsigned short>(src[i]);
}

bool exe_names(wchar_t* full, unsigned full_n, wchar_t* file, unsigned file_n) {
  if (!full || full_n < 2) return false;
  const DWORD n = GetModuleFileNameW(nullptr, full, full_n);
  if (n == 0 || n >= full_n) return false;
  const wchar_t* slash = wcsrchr(full, L'\\');
  const wchar_t* base = slash ? slash + 1 : full;
  if (file && file_n > 1) {
    wcsncpy_s(file, file_n, base, _TRUNCATE);
  }
  return base[0] != 0;
}

void* nv_query() {
  static QueryFn query = nullptr;
  static bool tried = false;
  if (tried) return reinterpret_cast<void*>(query);
  tried = true;
  wchar_t dir[MAX_PATH]{};
  if (GetSystemDirectoryW(dir, MAX_PATH) == 0) return nullptr;
  wchar_t path[MAX_PATH]{};
  lstrcpyW(path, dir);
  lstrcatW(path, L"\\nvapi64.dll");
  HMODULE lib = LoadLibraryW(path);
  if (!lib) return nullptr;
  query = reinterpret_cast<QueryFn>(GetProcAddress(lib, "nvapi_QueryInterface"));
  return reinterpret_cast<void*>(query);
}

template <typename T>
T nv_fn(unsigned id) {
  auto* q = reinterpret_cast<QueryFn>(nv_query());
  if (!q) return nullptr;
  return reinterpret_cast<T>(q(id));
}

bool cpu_apply(bool on) {
  State& s = st();
  PROCESS_POWER_THROTTLING_STATE cur{};
  cur.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  if (on) {
    if (!s.throttling_saved) {
      PROCESS_POWER_THROTTLING_STATE before{};
      before.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
      if (GetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &before, sizeof(before))) {
        s.throttling_before = before;
        s.throttling_saved = true;
      }
    }
    cur.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    cur.StateMask = 0;
    const bool info_ok =
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &cur, sizeof(cur)) != 0;
    if (!s.timer_on) {
      if (timeBeginPeriod(1) == TIMERR_NOERROR) s.timer_on = true;
    }
    s.cpu_ok = info_ok && s.timer_on;
    return s.cpu_ok;
  }
  if (s.timer_on) {
    timeEndPeriod(1);
    s.timer_on = false;
  }
  if (s.throttling_saved) {
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s.throttling_before,
                          sizeof(s.throttling_before));
    s.throttling_saved = false;
  }
  s.cpu_ok = true;
  return true;
}

bool gpu_profile(bool on) {
  auto init = nv_fn<InitFn>(kNvInitialize);
  auto create_session = nv_fn<SessionFn>(kNvDrsCreateSession);
  auto destroy_session = nv_fn<SessionOnlyFn>(kNvDrsDestroySession);
  auto load = nv_fn<SessionOnlyFn>(kNvDrsLoadSettings);
  auto save = nv_fn<SessionOnlyFn>(kNvDrsSaveSettings);
  auto create_profile = nv_fn<CreateProfileFn>(kNvDrsCreateProfile);
  auto find_profile = nv_fn<FindProfileFn>(kNvDrsFindProfileByName);
  auto create_app = nv_fn<CreateAppFn>(kNvDrsCreateApplication);
  auto find_app = nv_fn<FindAppFn>(kNvDrsFindApplicationByName);
  auto set_setting = nv_fn<SetSettingFn>(kNvDrsSetSetting);
  auto del_setting = nv_fn<DeleteSettingFn>(kNvDrsDeleteProfileSetting);
  if (!init || !create_session || !destroy_session || !load || !save || !find_app || !set_setting ||
      !del_setting) {
    return !on;
  }
  if (init() != kNvOk) return !on;

  wchar_t full[MAX_PATH]{};
  wchar_t file[MAX_PATH]{};
  if (!exe_names(full, MAX_PATH, file, MAX_PATH)) return !on;

  void* session = nullptr;
  if (create_session(&session) != kNvOk || !session) return !on;
  struct SessionGuard {
    void* session;
    SessionOnlyFn destroy;
    ~SessionGuard() {
      if (session && destroy) destroy(session);
    }
  } guard{session, destroy_session};

  if (load(session) != kNvOk) return !on;

  auto find_named = [&](const wchar_t* name, void** profile) -> bool {
    NvApplication app{};
    app.version = NC_NV_VER(NvApplication, 4);
    unsigned short wide[kUnicodeMax]{};
    copy_w(wide, name);
    const NvStatus st = find_app(session, wide, profile, &app);
    return st == kNvOk && *profile;
  };

  void* profile = nullptr;
  if (!find_named(full, &profile)) find_named(file, &profile);
  if (!profile && on && create_profile && create_app) {
    NvProfile info{};
    info.version = NC_NV_VER(NvProfile, 1);
    copy_w(info.profile_name, L"NexusCap");
    void* made = nullptr;
    const NvStatus created = create_profile(session, &info, &made);
    if (created == kNvOk) profile = made;
    else if (created == kNvProfileNameInUse && find_profile) {
      unsigned short wide[kUnicodeMax]{};
      copy_w(wide, L"NexusCap");
      find_profile(session, wide, &profile);
    }
    if (profile) {
      NvApplication app{};
      app.version = NC_NV_VER(NvApplication, 4);
      copy_w(app.app_name, full);
      copy_w(app.user_friendly, L"NexusCap");
      const NvStatus added = create_app(session, profile, &app);
      if (added != kNvOk && added != kNvExecutableAlreadyInUse) profile = nullptr;
    }
  }
  if (!profile) return !on;

  NvStatus st = kNvOk;
  if (on) {
    NvSetting setting{};
    setting.version = NC_NV_VER(NvSetting, 1);
    setting.setting_id = kPreferredPstateId;
    setting.u32_current = kPreferMax;
    st = set_setting(session, profile, &setting);
  } else {
    st = del_setting(session, profile, kPreferredPstateId);
    if (st == kNvSettingNotFound || st == kNvProfileNotFound || st == kNvExecutableNotFound) st = kNvOk;
  }
  if (st != kNvOk) return false;
  return save(session) == kNvOk;
}

void sync_locked(bool on) {
  State& s = st();
  s.wanted = on;
  cpu_apply(on);
  s.gpu_ok = gpu_profile(on);
}

}  // namespace

void max_perf_sync(bool on) {
  State& s = st();
  EnterCriticalSection(&s.lock);
  sync_locked(on);
  LeaveCriticalSection(&s.lock);
}

void max_perf_shutdown() {
  State& s = st();
  if (!s.lock_ready) return;
  EnterCriticalSection(&s.lock);
  sync_locked(false);
  LeaveCriticalSection(&s.lock);
}

extern "C" void nexuscap_max_perf_atexit() { max_perf_shutdown(); }

void max_perf_boot(bool on) {
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    std::atexit(nexuscap_max_perf_atexit);
  }
  max_perf_sync(on);
}

bool max_perf_is_on() {
  State& s = st();
  if (!s.lock_ready) return false;
  EnterCriticalSection(&s.lock);
  const bool on = s.wanted;
  LeaveCriticalSection(&s.lock);
  return on;
}

const char* max_perf_hint() {
  return "Pede ao driver da NVIDIA para preferir desempenho maximo so no perfil deste "
         "programa, nunca no perfil global nem no de jogos, e tira este processo do "
         "power throttling do Windows. Sem placa NVIDIA, so a parte da CPU vale. "
         "Ligado por padrao.";
}

const char* max_perf_note() {
  State& s = st();
  if (!s.lock_ready) return "Ligado por padrao.";
  EnterCriticalSection(&s.lock);
  const bool wanted = s.wanted;
  const bool cpu_ok = s.cpu_ok;
  const bool gpu_ok = s.gpu_ok;
  LeaveCriticalSection(&s.lock);
  if (!wanted) return "Desligado.";
  if (wanted && cpu_ok && gpu_ok) return "Ligado. Aplicado agora (GPU e CPU).";
  if (wanted && cpu_ok && !gpu_ok)
    return "Ligado. Aplicado agora (so CPU; perfil da GPU nao entrou).";
  return "Nao deu para ligar o desempenho deste processo.";
}

}  // namespace nexuscap
