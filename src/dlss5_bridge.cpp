#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/settings_store.hpp"

#include <windows.h>

#include <fstream>
#include <string>

namespace nexuscap {
namespace {

using FnVersion = uint32_t (*)();
using FnProtocol = uint32_t (*)();
using FnSubmit = int (*)(const Dlss5BridgeCommand*);
using FnQuery = int (*)(Dlss5BridgeStatus*);

struct BridgeApi {
  HMODULE mod = nullptr;
  FnVersion version = nullptr;
  FnProtocol protocol = nullptr;
  FnSubmit submit = nullptr;
  FnQuery query = nullptr;
  bool resolved = false;
};

BridgeApi& api() {
  static BridgeApi a;
  return a;
}

bool resolve_bridge() {
  BridgeApi& a = api();
  HMODULE mod = GetModuleHandleW(L"dlss5-bridge.addon64");
  if (mod != a.mod) {
    a = {};
    a.mod = mod;
  }
  if (a.resolved) return a.submit != nullptr && a.query != nullptr;
  if (!a.mod) {
    a.resolved = true;
    return false;
  }
  a.version = reinterpret_cast<FnVersion>(GetProcAddress(a.mod, "DLSS5Bridge_Version"));
  a.protocol = reinterpret_cast<FnProtocol>(GetProcAddress(a.mod, "DLSS5Bridge_Protocol"));
  a.submit = reinterpret_cast<FnSubmit>(GetProcAddress(a.mod, "DLSS5Bridge_Submit"));
  a.query = reinterpret_cast<FnQuery>(GetProcAddress(a.mod, "DLSS5Bridge_Query"));
  a.resolved = true;
  if (!a.submit || !a.query) return false;
  if (a.version && a.version() != kDlss5BridgeVersion) return false;
  if (a.protocol && a.protocol() != kDlss5BridgeProtocol) return false;
  return true;
}

std::streamoff file_size_off(const std::string& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return 0;
  return static_cast<std::streamoff>(in.tellg());
}

std::string read_file_from_off(const std::string& path, std::streamoff start) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  in.seekg(0, std::ios::end);
  const auto end = static_cast<std::streamoff>(in.tellg());
  if (end <= start) return {};
  in.seekg(start);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Krish samples GetAsyncKeyState once per Present; back-to-back edges merge into one.
constexpr DWORD kNrPulseGapMs = 120;

// Blocking wait — lobby/boot only. Session uses async poll (no Present freeze).
// Returns false when no "NR toggled" line shows up: *out_on keeps its value.
bool confirm_nr_after_pulse_blocking(const std::string& exe_dir, bool want_on, std::streamoff log_size_before,
                                     bool* out_on, uint32_t timeout_ms) {
  if (!out_on) return false;
  const std::string log_path = exe_dir + "\\ReShade.log";
  int recover = 0;
  std::streamoff from = log_size_before;
  for (;;) {
    const DWORD t0 = GetTickCount();
    for (;;) {
      const int logged_new = parse_reshade_nr_last_toggled(read_file_from_off(log_path, from));
      if (logged_new >= 0) {
        *out_on = nr_runtime_after_log(logged_new, *out_on);
        if (*out_on != want_on && recover < 1 && resolve_bridge()) {
          ++recover;
          from = file_size_off(log_path);
          Sleep(kNrPulseGapMs);
          const Dlss5BridgeCommand cmd = make_nr_pulse_command();
          if (bridge_command_is_valid_nr_pulse(cmd) && api().submit(&cmd) != 0) break;
        }
        return true;
      }
      if (GetTickCount() - t0 >= timeout_ms) return false;
      Sleep(8);
    }
  }
}

}  // namespace

bool dlss5_bridge_available() { return resolve_bridge(); }

bool dlss5_bridge_query(Dlss5BridgeStatus& out) {
  if (!resolve_bridge()) return false;
  out = {};
  out.size = kDlss5BridgeStatusSize;
  return api().query(&out) != 0;
}

bool dlss5_bridge_ready_for_nr() {
  Dlss5BridgeStatus st{};
  if (!dlss5_bridge_query(st)) return false;
  return st.has_runtime != 0 && st.nr_ready != 0;
}

bool dlss5_bridge_pulse_nr() {
  if (!resolve_bridge()) return false;
  const Dlss5BridgeCommand cmd = make_nr_pulse_command();
  if (!bridge_command_is_valid_nr_pulse(cmd)) return false;
  return api().submit(&cmd) != 0;
}

bool inject_nr_toggle_key(int vk) {
  if (vk <= 0) vk = kRenodxNrInjectVk;
  INPUT down{};
  down.type = INPUT_KEYBOARD;
  down.ki.wVk = static_cast<WORD>(vk);
  INPUT up = down;
  up.ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(1, &down, sizeof(INPUT)) == 1 && SendInput(1, &up, sizeof(INPUT)) == 1;
}

bool dlss5_bridge_pulse_nr_confirmed(uint32_t timeout_ms) {
  if (!resolve_bridge()) return false;
  Dlss5BridgeStatus before{};
  const bool ok = dlss5_bridge_query(before);
  const uint32_t delivered0 = ok ? before.nr_delivered : 0;
  const uint32_t pulses0 = ok ? before.nr_pulses : 0;
  if (!dlss5_bridge_pulse_nr()) return false;
  const DWORD t0 = GetTickCount();
  for (;;) {
    Dlss5BridgeStatus st{};
    if (dlss5_bridge_query(st) && bridge_nr_ack_advanced(delivered0, pulses0, st)) return true;
    if (GetTickCount() - t0 >= timeout_ms) return false;
    Sleep(5);
  }
}

int peek_reshade_nr_toggled_since(const std::string& exe_dir, std::streamoff log_size_before) {
  return parse_reshade_nr_last_toggled(read_file_from_off(exe_dir + "\\ReShade.log", log_size_before));
}

int consume_reshade_nr_evals(const std::string& exe_dir, std::streamoff& cursor) {
  const std::string path = exe_dir + "\\ReShade.log";
  const std::streamoff end = file_size_off(path);
  if (end <= cursor) return 0;
  const int n = parse_reshade_nr_eval_count(read_file_from_off(path, cursor));
  cursor = end;
  return n;
}

NrConfirmPoll poll_nr_confirm_pending(NrConfirmPending& pending, bool* runtime_on, double now_ms) {
  if (!pending.active || !runtime_on) return NrConfirmPoll::Failed;
  const int logged = peek_reshade_nr_toggled_since(pending.exe_dir, pending.log_size_before);
  if (logged >= 0) {
    *runtime_on = logged == 1;
    // Enable pulse flipped an already-ON boot → OFF: recover pulse(s) to match want.
    if (*runtime_on != pending.want_on && pending.recover_pulses < 2) {
      if (!dlss5_bridge_ready_for_nr()) return NrConfirmPoll::Pending;
      ++pending.recover_pulses;
      pending.log_size_before = file_size_off(pending.exe_dir + "\\ReShade.log");
      Sleep(kNrPulseGapMs);
      if (dlss5_bridge_pulse_nr()) {
        pending.deadline_ms = now_ms + 2500.0;
        return NrConfirmPoll::Pending;
      }
    }
    pending.active = false;
    return NrConfirmPoll::Confirmed;
  }
  if (now_ms >= pending.deadline_ms) {
    const int last = peek_reshade_nr_toggled_since(pending.exe_dir, pending.log_origin);
    pending.active = false;
    if (last < 0) return NrConfirmPoll::Failed;
    *runtime_on = nr_runtime_after_log(last, *runtime_on);
    return NrConfirmPoll::Confirmed;
  }
  return NrConfirmPoll::Pending;
}

bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on, bool feed_on,
                                  NrConfirmPending* async_out, double now_ms) {
  if (!runtime_on) return false;
  if (async_out) async_out->active = false;
  const NeuralRuntimePlan plan = plan_neural_runtime(want_on, *runtime_on);
  const bool files_ok = sync_neural_rendering_enabled_files(exe_dir, plan.sync_files_enabled, feed_on);
  if (!plan.need_bridge_pulse) return files_ok;
  const std::string log_path = exe_dir + "\\ReShade.log";
  const std::streamoff log_sz = file_size_off(log_path);
  if (!nr_runtime_may_flip_without_pulse(dlss5_bridge_pulse_nr())) return false;
  if (async_out) {
    async_out->active = true;
    async_out->exe_dir = exe_dir;
    async_out->log_size_before = log_sz;
    async_out->log_origin = log_sz;
    async_out->want_on = plan.resulting_runtime_on;
    async_out->deadline_ms = now_ms + 2000.0;
    async_out->recover_pulses = 0;
    return files_ok;
  }
  bool actual_on = *runtime_on;
  if (!confirm_nr_after_pulse_blocking(exe_dir, plan.resulting_runtime_on, log_sz, &actual_on, 800)) {
    return false;
  }
  *runtime_on = actual_on;
  return files_ok && actual_on == want_on;
}

}  // namespace nexuscap
