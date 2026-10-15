#pragma once

#include <cstdint>
#include <string>

namespace nexuscap {

struct NrAdvancedLook;

// Krish 8.5 (addons-multipass) evaluates Feature 18 on Present. Every CFG change in the same
// PID is: write the INI the AddonPath reads, then Bridge-pulse so Krish re-reads it.
enum class KrishLiveKind { Nr, Pass, Preset, Look };

struct KrishLivePlan {
  bool write_ini = true;
  bool pulse_toggle = false;   // one edge: want ≠ runtime
  bool reload_cycle = false;   // OFF→ON so Krish re-reads NRPasses / NRStyle / NRLook*
  bool relaunch = false;
  bool resulting_runtime_on = false;
};

inline KrishLivePlan plan_krish_live(KrishLiveKind kind, bool want_nr, bool runtime_on) {
  KrishLivePlan p;
  if (kind != KrishLiveKind::Nr && want_nr && runtime_on) {
    p.reload_cycle = true;
  } else {
    p.pulse_toggle = want_nr != runtime_on;
  }
  p.resulting_runtime_on = want_nr;
  return p;
}

inline bool krish_live_needs_relaunch(KrishLiveKind kind) {
  return plan_krish_live(kind, true, true).relaunch;
}

// Krish only attaches through dxgi.dll next to the exe — it must be there before the first
// Present of a RenoDX process. Parking it is only for RenoDX off.
inline bool session_boot_restores_reshade(bool enhance_renodx) { return enhance_renodx; }
inline bool boot_parks_reshade_dxgi(bool enhance_renodx) { return !session_boot_restores_reshade(enhance_renodx); }

// Pin helpers used by tests. Poke RVA is extra, never a substitute for the Bridge pulse.
// The 8-byte context pointer occupies [kKrishRvaCtxPtr, kKrishRvaCtxPtr+8); the exclusive
// end is not a valid pin of that slot. CtxPtr+9 is the first byte past it.
inline constexpr std::uint32_t kKrishRvaCtxPtr = 0x160B00;
inline bool krish_pin_image_ok(std::uint64_t rva) {
  return rva > static_cast<std::uint64_t>(kKrishRvaCtxPtr) + 8;
}

// Natural (1) and Cinematic (2) use the txn/preset path; Default (0) does not.
inline bool krish_preset_uses_txn(int visual_preset) {
  const int v = visual_preset < 0 ? 0 : (visual_preset > 2 ? 2 : visual_preset);
  return v != 0;
}

struct KrishLookOverlay {
  int encoding = 2;
  int hook = 2;
  int nits = 100;
};

// All four radios use the optimized hook (encoding 2, hook 2, nits 100).
inline KrishLookOverlay krish_look_overlay_values(int /*pass_count*/) { return {}; }

// Writes the INI for kind, then pulses per plan_krish_live. runtime_on == nullptr: INI only.
// runtime_on changes only on a confirmed "NR toggled" line (see apply_neural_rendering_state).
bool apply_krish_session(const std::string& exe_dir, KrishLiveKind kind, bool nr_on, int visual_preset,
                         int pass_count, const NrAdvancedLook* look, bool* runtime_on = nullptr);

}  // namespace nexuscap
