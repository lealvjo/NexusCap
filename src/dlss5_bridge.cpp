#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/settings_store.hpp"

#include <windows.h>

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

bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on) {
  if (!runtime_on) return false;
  const NeuralRuntimePlan plan = plan_neural_runtime(want_on, *runtime_on);
  const bool files_ok = sync_neural_rendering_enabled_files(exe_dir, plan.sync_files_enabled);
  if (plan.need_bridge_pulse) {
    if (!dlss5_bridge_available()) {
      // Files updated; runtime pulse deferred until Bridge loads (caller may retry).
      return files_ok;
    }
    if (!dlss5_bridge_pulse_nr()) return false;
    *runtime_on = plan.resulting_runtime_on;
    return files_ok;
  }
  *runtime_on = plan.resulting_runtime_on;
  return files_ok;
}

}  // namespace nexuscap
