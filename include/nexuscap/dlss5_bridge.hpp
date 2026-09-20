#pragma once
#include <cstdint>
#include <cstdlib>
#include <ios>
#include <string>

namespace nexuscap {

// dlss5-bridge.addon64 protocol (Version=1, Protocol=2).
// RenoDX Neural Rendering always boots runtime OFF even when NeuralUplift=1 on disk —
// writing ReShade.ini alone does NOT enable NR. The Bridge Submit opcode 6 queues a
// fake NRToggleKey edge that RenoDX honors without focus.
inline constexpr uint32_t kDlss5BridgeVersion = 1;
inline constexpr uint32_t kDlss5BridgeProtocol = 2;
inline constexpr uint32_t kDlss5BridgeCommandSize = 0xACu;
inline constexpr uint32_t kDlss5BridgeOpcodeNrPulse = 6;
inline constexpr uint32_t kDlss5BridgeStatusSize = 0x3Cu;
// Inject key used only by Bridge→RenoDX (not the user Delete hotkey) to avoid double-toggle.
inline constexpr int kRenodxNrInjectVk = 0x7C;  // VK_F13

#pragma pack(push, 1)
struct Dlss5BridgeCommand {
  uint32_t size = kDlss5BridgeCommandSize;
  uint32_t opcode = 0;
  uint8_t bytes[kDlss5BridgeCommandSize - 8]{};
};

struct Dlss5BridgeStatus {
  uint32_t size = kDlss5BridgeStatusSize;
  uint32_t protocol = 0;
  uint32_t has_runtime = 0;
  uint32_t pad0[8]{};
  uint32_t nr_ready = 0;    // offset 44
  uint32_t pad1 = 0;
  uint32_t nr_pulses = 0;   // offset 52 — pending/delivered counter
  uint32_t nr_delivered = 0;
};
#pragma pack(pop)

static_assert(sizeof(Dlss5BridgeCommand) == kDlss5BridgeCommandSize, "bridge cmd size");
static_assert(sizeof(Dlss5BridgeStatus) >= kDlss5BridgeStatusSize, "bridge status size");

inline Dlss5BridgeCommand make_nr_pulse_command() {
  Dlss5BridgeCommand cmd{};
  cmd.size = kDlss5BridgeCommandSize;
  cmd.opcode = kDlss5BridgeOpcodeNrPulse;
  // Submit rejects cmd when byte at +0x48 is zero.
  reinterpret_cast<uint8_t*>(&cmd)[0x48] = 1;
  return cmd;
}

inline bool bridge_command_is_valid_nr_pulse(const Dlss5BridgeCommand& cmd) {
  if (cmd.size != kDlss5BridgeCommandSize) return false;
  if (cmd.opcode != kDlss5BridgeOpcodeNrPulse) return false;
  return reinterpret_cast<const uint8_t*>(&cmd)[0x48] != 0;
}

// Fresh process / first ReShade attach: often OFF, but some RenoDX builds honor NeuralUplift=1
// and boot already ON — never assume without cfg.nr_runtime_on / log confirm.
inline constexpr bool kRenodxRuntimeStartsOff = true;

struct NeuralRuntimePlan {
  bool sync_files_enabled = false;
  bool need_bridge_pulse = false;
  bool resulting_runtime_on = false;
};

// Map desired NR ↔ current runtime. Disk NeuralUplift alone never flips runtime.
inline NeuralRuntimePlan plan_neural_runtime(bool want_on, bool runtime_on) {
  NeuralRuntimePlan p;
  p.sync_files_enabled = want_on;
  p.need_bridge_pulse = (want_on != runtime_on);
  p.resulting_runtime_on = want_on;
  return p;
}

inline bool want_neural_rendering(bool enhance_renodx, bool neural_rendering_flag) {
  return enhance_renodx && neural_rendering_flag;
}

// Boot / new swapchain: runtime is always OFF → pulse once when we want NR on.
inline bool nr_boot_needs_enable_pulse(bool want_on) {
  return want_on && kRenodxRuntimeStartsOff;
}

// Delete mid-session: flip runtime via Bridge (not via RenoDX hearing the same VK).
inline NeuralRuntimePlan plan_nr_user_toggle(bool runtime_on) {
  return plan_neural_runtime(!runtime_on, runtime_on);
}

// Ignore a second Delete within debounce_ms (auto-repeat / double WM_* that bit30 misses).
inline bool nr_toggle_debounce_ok(double now_ms, double& last_accepted_ms, double debounce_ms = 350.0) {
  if (last_accepted_ms > 0.0 && (now_ms - last_accepted_ms) < debounce_ms) return false;
  last_accepted_ms = now_ms;
  return true;
}

// Settings vs runtime must not be conflated — this was the Delete desync bug.
inline bool settings_imply_runtime_on_at_boot(bool enhance_renodx, bool neural_rendering_flag) {
  (void)enhance_renodx;
  (void)neural_rendering_flag;
  return false;  // always OFF until Bridge pulse
}

// Lobby CFG: enable/disable or change Default/Natural/Cinematic while preview is live.
struct LobbyRenodxPreviewPlan {
  bool show_on_present = false;       // want NR → visible via lobby Present
  bool sync_preset_files = false;     // rewrite feed + ReShade preset keys
  bool need_nr_pulse = false;         // single Bridge pulse to match want↔runtime
  bool need_preset_reload_cycle = false;  // OFF→ON so RenoDX re-reads NRPreset/feed
  bool resulting_runtime_on = false;
};

inline LobbyRenodxPreviewPlan plan_lobby_renodx_toggle(bool enhance_renodx, bool neural_rendering_flag,
                                                      bool runtime_on) {
  LobbyRenodxPreviewPlan p;
  const bool want = want_neural_rendering(enhance_renodx, neural_rendering_flag);
  p.show_on_present = want;
  const NeuralRuntimePlan nr = plan_neural_runtime(want, runtime_on);
  p.need_nr_pulse = nr.need_bridge_pulse;
  p.resulting_runtime_on = nr.resulting_runtime_on;
  return p;
}

inline LobbyRenodxPreviewPlan plan_lobby_renodx_preset_change(bool enhance_renodx, bool neural_rendering_flag,
                                                             int old_preset, int new_preset, bool runtime_on) {
  LobbyRenodxPreviewPlan p;
  const bool want = want_neural_rendering(enhance_renodx, neural_rendering_flag);
  p.show_on_present = want;
  const int a = old_preset < 0 ? 0 : (old_preset > 2 ? 2 : old_preset);
  const int b = new_preset < 0 ? 0 : (new_preset > 2 ? 2 : new_preset);
  p.sync_preset_files = (a != b);
  if (want && runtime_on && p.sync_preset_files) {
    // Disk write alone may not refresh RenoDX look — cycle NR to re-ingest preset.
    p.need_preset_reload_cycle = true;
    p.resulting_runtime_on = true;
  } else {
    const NeuralRuntimePlan nr = plan_neural_runtime(want, runtime_on);
    p.need_nr_pulse = nr.need_bridge_pulse;
    p.resulting_runtime_on = nr.resulting_runtime_on;
  }
  return p;
}

bool dlss5_bridge_available();
bool dlss5_bridge_ready_for_nr();
bool dlss5_bridge_pulse_nr();
bool dlss5_bridge_query(Dlss5BridgeStatus& out);

// Snapshot nr_delivered or nr_pulses; either advancing means Bridge consumed an edge.
inline bool bridge_nr_ack_advanced(uint32_t delivered_before, uint32_t pulses_before,
                                   const Dlss5BridgeStatus& after) {
  return after.nr_delivered > delivered_before || after.nr_pulses > pulses_before;
}

// Snapshot nr_delivered; after pulse, wait until it increases (Bridge consumed the edge).
inline bool bridge_nr_delivered_advanced(uint32_t before, const Dlss5BridgeStatus& after) {
  return after.nr_delivered > before;
}

// Parse RenoDX log lines: "NR toggled ON via 0x124" / "NR toggled OFF via ..."
// Returns +1 ON, 0 OFF, -1 not a toggle line.
inline int parse_reshade_nr_toggled_line(const std::string& line) {
  if (line.find("NR toggled ON") != std::string::npos) return 1;
  if (line.find("NR toggled OFF") != std::string::npos) return 0;
  return -1;
}

// Scan log text newest-first (split by newline); last matching toggle wins.
inline int parse_reshade_nr_last_toggled(const std::string& log_text) {
  int last = -1;
  size_t i = 0;
  while (i < log_text.size()) {
    size_t e = log_text.find('\n', i);
    if (e == std::string::npos) e = log_text.size();
    std::string line = log_text.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const int v = parse_reshade_nr_toggled_line(line);
    if (v >= 0) last = v;
    i = e + 1;
  }
  return last;
}

// "EvaluateFeature succeeded: evaluation=178" — prova de NR real, nao o flag do plano.
inline int parse_reshade_nr_eval_line(const std::string& line) {
  if (line.find("EvaluateFeature succeeded") == std::string::npos) return -1;
  const auto ev = line.find("evaluation=");
  if (ev == std::string::npos) return 1;
  const auto eq = line.find('=', ev);
  if (eq == std::string::npos) return 1;
  const int n = std::atoi(line.c_str() + eq + 1);
  return n > 0 ? n : 1;
}

inline int parse_reshade_nr_eval_count(const std::string& log_text) {
  int n = 0;
  size_t i = 0;
  while (i < log_text.size()) {
    size_t e = log_text.find('\n', i);
    if (e == std::string::npos) e = log_text.size();
    std::string line = log_text.substr(i, e - i);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (parse_reshade_nr_eval_line(line) >= 0) ++n;
    i = e + 1;
  }
  return n;
}

// Measured on the multipass build: it never reads NRToggleKey (its ReShade sections
// [RENODX-DLSS]* carry no key binding) and an injected edge produces no log line, no state
// change and no frame-cost change — NR runs for the whole session there. Only the stable
// build toggles by key ("NR toggled ON via 0x7C" in ReShade.log).
inline bool renodx_stack_supports_nr_hotkey(bool multipass_stack) { return !multipass_stack; }

// FG HUD: only claim ON if LS process is alive; OFF only if none remain.
inline bool fg_overlay_on_confirmed(bool want_on, unsigned running_ls) {
  if (want_on) return running_ls > 0;
  return running_ls == 0;
}

bool dlss5_bridge_pulse_nr_confirmed(uint32_t timeout_ms = 400);

// Non-blocking NR confirm: pulse now, finish on later poll (keeps capture Present alive).
struct NrConfirmPending {
  bool active = false;
  std::string exe_dir;
  std::streamoff log_size_before = 0;
  std::streamoff log_origin = 0;  // first marker — deadline reads full pulse window
  bool want_on = false;
  double deadline_ms = 0;
  // RenoDX may boot already ON when NeuralUplift=1; first enable-pulse then lands OFF.
  int recover_pulses = 0;
};

enum class NrConfirmPoll { Pending, Confirmed, Failed };

// Read only bytes appended since log_size_before; -1 = no toggle line yet.
int peek_reshade_nr_toggled_since(const std::string& exe_dir, std::streamoff log_size_before);
int consume_reshade_nr_evals(const std::string& exe_dir, std::streamoff& cursor);

NrConfirmPoll poll_nr_confirm_pending(NrConfirmPending& pending, bool* runtime_on, double now_ms);

// Fake NRToggleKey edge. Krish: Bridge GetAsyncKeyState inject. Multipass: real VK_F13 SendInput
// (addon has NRToggleKey but does not import GetAsyncKeyState for Bridge).
bool inject_nr_toggle_key(int vk = kRenodxNrInjectVk);

// Apply want_on: sync NeuralUplift + Bridge pulse if runtime differs.
// Multipass: SendInput NRToggleKey instead of Bridge.
// If async_out != nullptr: pulse without blocking and fill pending (HUD updates on poll).
// If async_out == nullptr: wait for ReShade.log line (lobby/boot; may hitch briefly).
bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on,
                                  bool feed_on = true, NrConfirmPending* async_out = nullptr,
                                  double now_ms = 0);

// Lobby live preview: sync visual preset + apply NR (reload cycle when already ON).
bool apply_lobby_renodx_preset_live(const std::string& exe_dir, int visual_preset, bool want_nr,
                                    bool* runtime_on, int pass_count = 1);

// Sync Pass Count + look defaults + reload NR so RenoDX re-reads.
bool apply_lobby_nr_pass_count_live(const std::string& exe_dir, int pass_count, bool want_nr,
                                    bool* runtime_on, int visual_preset = 0);

}  // namespace nexuscap
