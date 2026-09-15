#pragma once
#include <cstdint>
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

// RenoDX runtime boots OFF every new effect_runtime / swapchain.
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

// Settings vs runtime must not be conflated — this was the Delete desync bug.
inline bool settings_imply_runtime_on_at_boot(bool enhance_renodx, bool neural_rendering_flag) {
  (void)enhance_renodx;
  (void)neural_rendering_flag;
  return false;  // always OFF until Bridge pulse
}

bool dlss5_bridge_available();
bool dlss5_bridge_ready_for_nr();
bool dlss5_bridge_pulse_nr();
bool dlss5_bridge_query(Dlss5BridgeStatus& out);

// Apply want_on: sync feed/ReShade files + Bridge pulse if runtime differs.
// Updates *runtime_on in/out. Returns true if files synced and (pulse ok or not needed).
bool apply_neural_rendering_state(const std::string& exe_dir, bool want_on, bool* runtime_on);

}  // namespace nexuscap
