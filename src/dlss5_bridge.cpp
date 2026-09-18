#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/settings_store.hpp"

#include <windows.h>

#include <fstream>
#include <iterator>
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

// Blocking wait — lobby/boot only. Session uses async poll (no Present freeze).
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
        *out_on = logged_new == 1;
        if (*out_on != want_on && recover < 1) {
          ++recover;
          from = file_size_off(log_path);
          if (pulse_renodx_nr(exe_dir)) break;
        }
        return true;
      }
      if (GetTickCount() - t0 >= timeout_ms) {
        *out_on = want_on;
        return true;  // pulse already sent — prefer want over stuck HUD
      }
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

bool pulse_renodx_nr(const std::string& exe_dir) {
  if (renodx_active_addon_is_multipass(exe_dir)) return false;
  if (!dlss5_bridge_available()) return false;
  return dlss5_bridge_pulse_nr();
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

bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on, bool feed_on,
                                  NrConfirmPending* async_out, double now_ms);

int peek_reshade_nr_toggled_since(const std::string& exe_dir, std::streamoff log_size_before) {
  return parse_reshade_nr_last_toggled(read_file_from_off(exe_dir + "\\ReShade.log", log_size_before));
}

int peek_reshade_mp_preset_since(const std::string& exe_dir, std::streamoff log_size_before) {
  return parse_reshade_last_int(read_file_from_off(exe_dir + "\\ReShade.log", log_size_before),
                               parse_reshade_mp_preset_transaction);
}

int peek_reshade_mp_pass_since(const std::string& exe_dir, std::streamoff log_size_before) {
  return parse_reshade_last_int(read_file_from_off(exe_dir + "\\ReShade.log", log_size_before),
                               parse_reshade_mp_pass_count_line);
}

bool arm_key_ack(const std::string& exe_dir, int vk, NrConfirmPending* out, double now_ms, LiveAckKind kind,
                 int extra_pulses) {
  if (!out) return false;
  const std::streamoff log_sz = file_size_off(exe_dir + "\\ReShade.log");
  if (!inject_nr_toggle_key(vk)) return false;
  out->active = true;
  out->exe_dir = exe_dir;
  out->log_size_before = log_sz;
  out->log_origin = log_sz;
  out->deadline_ms = now_ms + 2000.0;
  out->recover_pulses = 0;
  out->followup_enable = false;
  out->relaunch_if_unconfirmed = true;
  out->ack_kind = kind;
  out->pulses_left = extra_pulses;
  out->repeat_vk = vk;
  return true;
}

NrConfirmPoll finish_or_repeat_key(NrConfirmPending& pending, double now_ms) {
  if (pending.pulses_left > 0 && pending.repeat_vk > 0) {
    --pending.pulses_left;
    pending.log_size_before = file_size_off(pending.exe_dir + "\\ReShade.log");
    pending.log_origin = pending.log_size_before;
    Sleep(100);
    if (!inject_nr_toggle_key(pending.repeat_vk)) {
      pending.active = false;
      return NrConfirmPoll::Failed;
    }
    pending.deadline_ms = now_ms + 2000.0;
    return NrConfirmPoll::Pending;
  }
  pending.active = false;
  return NrConfirmPoll::Confirmed;
}

NrConfirmPoll poll_nr_confirm_pending(NrConfirmPending& pending, bool* runtime_on, double now_ms) {
  if (!pending.active || !runtime_on) return NrConfirmPoll::Failed;
  if (pending.ack_kind == LiveAckKind::PresetCycle || pending.ack_kind == LiveAckKind::PassCount) {
    const int got = (pending.ack_kind == LiveAckKind::PresetCycle)
                        ? peek_reshade_mp_preset_since(pending.exe_dir, pending.log_size_before)
                        : peek_reshade_mp_pass_since(pending.exe_dir, pending.log_size_before);
    if (got >= 0) return finish_or_repeat_key(pending, now_ms);
    if (now_ms >= pending.deadline_ms) {
      const int last = (pending.ack_kind == LiveAckKind::PresetCycle)
                           ? peek_reshade_mp_preset_since(pending.exe_dir, pending.log_origin)
                           : peek_reshade_mp_pass_since(pending.exe_dir, pending.log_origin);
      if (last >= 0) return finish_or_repeat_key(pending, now_ms);
      pending.active = false;
      return NrConfirmPoll::Failed;
    }
    return NrConfirmPoll::Pending;
  }
  const int logged = peek_reshade_nr_toggled_since(pending.exe_dir, pending.log_size_before);
  if (logged >= 0) {
    *runtime_on = logged == 1;
    // Enable pulse flipped an already-ON boot → OFF: recover pulse(s) to match want.
    if (*runtime_on != pending.want_on && pending.recover_pulses < 2) {
      if (!renodx_active_addon_is_multipass(pending.exe_dir) && !dlss5_bridge_ready_for_nr()) {
        return NrConfirmPoll::Pending;
      }
      ++pending.recover_pulses;
      pending.log_size_before = file_size_off(pending.exe_dir + "\\ReShade.log");
      Sleep(120);  // RenoDX needs a beat between edges
      if (pulse_renodx_nr(pending.exe_dir)) {
        pending.deadline_ms = now_ms + 2500.0;
        return NrConfirmPoll::Pending;
      }
    }
    if (pending.followup_enable && *runtime_on == pending.want_on && !pending.want_on) {
      pending.followup_enable = false;
      const std::string dir = pending.exe_dir;
      bool rt = *runtime_on;
      if (!apply_neural_rendering_state(dir, true, &rt, true, &pending, now_ms)) {
        pending.active = false;
        return NrConfirmPoll::Failed;
      }
      return pending.active ? NrConfirmPoll::Pending : NrConfirmPoll::Confirmed;
    }
    pending.active = false;
    return NrConfirmPoll::Confirmed;
  }
  if (now_ms >= pending.deadline_ms) {
    const int last = peek_reshade_nr_toggled_since(pending.exe_dir, pending.log_origin);
    pending.active = false;
    if (last >= 0) {
      *runtime_on = last == 1;
      return NrConfirmPoll::Confirmed;
    }
    return NrConfirmPoll::Failed;
  }
  return NrConfirmPoll::Pending;
}

bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on, bool feed_on,
                                  NrConfirmPending* async_out, double now_ms) {
  if (!runtime_on) return false;
  const NeuralRuntimePlan plan = plan_neural_runtime(want_on, *runtime_on);
  const bool files_ok = sync_neural_rendering_enabled_files(exe_dir, plan.sync_files_enabled, feed_on);
  if (plan.need_bridge_pulse) {
    if (renodx_active_addon_is_multipass(exe_dir)) {
      if (async_out) async_out->active = false;
      return files_ok;
    }
    if (!dlss5_bridge_available()) return false;
    const std::string log_path = exe_dir + "\\ReShade.log";
    const std::streamoff log_sz = file_size_off(log_path);
    if (!pulse_renodx_nr(exe_dir)) return false;
    if (async_out) {
      const bool keep_followup = async_out->followup_enable;
      async_out->active = true;
      async_out->exe_dir = exe_dir;
      async_out->log_size_before = log_sz;
      async_out->log_origin = log_sz;
      async_out->want_on = plan.resulting_runtime_on;
      async_out->deadline_ms = now_ms + 2000.0;
      async_out->recover_pulses = 0;
      async_out->followup_enable = keep_followup;
      async_out->relaunch_if_unconfirmed = false;
      async_out->ack_kind = LiveAckKind::NrToggle;
      async_out->pulses_left = 0;
      async_out->repeat_vk = 0;
      return files_ok;
    }
    bool actual_on = plan.resulting_runtime_on;
    if (!confirm_nr_after_pulse_blocking(exe_dir, plan.resulting_runtime_on, log_sz, &actual_on, 800)) {
      return false;
    }
    *runtime_on = actual_on;
    return files_ok;
  }
  *runtime_on = plan.resulting_runtime_on;
  return files_ok;
}

bool apply_session_renodx_preset_live(const std::string& exe_dir, int visual_preset, bool want_nr,
                                      bool* runtime_on, NrConfirmPending* async_out, double now_ms,
                                      int from_visual) {
  if (!runtime_on || !async_out) return false;
  const bool files_ok = sync_renodx_visual_preset_files(exe_dir, visual_preset);
  if (renodx_active_addon_is_multipass(exe_dir)) {
    const int steps = renodx_mp_preset_cycle_steps(from_visual, visual_preset);
    if (steps <= 0) return files_ok;
    return arm_key_ack(exe_dir, kRenodxMpPresetCycleVk, async_out, now_ms, LiveAckKind::PresetCycle,
                       steps - 1) &&
           files_ok;
  }
  if (want_nr && *runtime_on) {
    const bool ok = apply_neural_rendering_state(exe_dir, false, runtime_on, true, async_out, now_ms);
    async_out->followup_enable = true;
    return ok && files_ok;
  }
  return apply_neural_rendering_state(exe_dir, want_nr, runtime_on, true, async_out, now_ms) && files_ok;
}

bool apply_session_nr_pass_count_live(const std::string& exe_dir, int from_pass, int to_pass, int visual_preset,
                                      NrConfirmPending* async_out, double now_ms) {
  if (!async_out) return false;
  const bool files_ok = sync_nr_pass_count_files(exe_dir, to_pass, visual_preset);
  const int delta = nr_pass_count_live_delta(from_pass, to_pass);
  if (delta == 0) return files_ok;
  const int vk = delta > 0 ? kRenodxMpPassIncVk : kRenodxMpPassDecVk;
  const int steps = delta > 0 ? delta : -delta;
  return arm_key_ack(exe_dir, vk, async_out, now_ms, LiveAckKind::PassCount, steps - 1) && files_ok;
}

bool apply_lobby_renodx_preset_live(const std::string& exe_dir, int visual_preset, bool want_nr,
                                    bool* runtime_on) {
  if (!runtime_on) return false;
  const bool files_ok = sync_renodx_visual_preset_files(exe_dir, visual_preset);
  if (want_nr && *runtime_on) {
    if (!apply_neural_rendering_state(exe_dir, false, runtime_on)) return false;
    if (!apply_neural_rendering_state(exe_dir, true, runtime_on)) return false;
    *runtime_on = true;
    return files_ok;
  }
  if (!apply_neural_rendering_state(exe_dir, want_nr, runtime_on)) return files_ok && !want_nr;
  return files_ok;
}

bool apply_lobby_nr_pass_count_live(const std::string& exe_dir, int pass_count, bool want_nr,
                                    bool* runtime_on, int visual_preset, int from_pass) {
  if (!runtime_on) return false;
  const bool files_ok = sync_nr_pass_count_files(exe_dir, pass_count, visual_preset);
  if (from_pass >= 0 && renodx_active_addon_is_multipass(exe_dir)) {
    const int delta = nr_pass_count_live_delta(from_pass, pass_count);
    if (delta == 0) return files_ok;
    const int vk = delta > 0 ? kRenodxMpPassIncVk : kRenodxMpPassDecVk;
    const int steps = delta > 0 ? delta : -delta;
    for (int i = 0; i < steps; ++i) {
      if (!inject_nr_toggle_key(vk)) return false;
      Sleep(150);
    }
    return files_ok;
  }
  if (want_nr && *runtime_on) {
    if (!apply_neural_rendering_state(exe_dir, false, runtime_on)) return false;
    if (!apply_neural_rendering_state(exe_dir, true, runtime_on)) return false;
    *runtime_on = true;
    return files_ok;
  }
  if (!apply_neural_rendering_state(exe_dir, want_nr, runtime_on)) return files_ok && !want_nr;
  return files_ok;
}

}  // namespace nexuscap
