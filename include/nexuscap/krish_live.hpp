#pragma once

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

// Writes the INI for kind, then pulses per plan_krish_live. runtime_on == nullptr: INI only.
// runtime_on changes only on a confirmed "NR toggled" line (see apply_neural_rendering_state).
bool apply_krish_session(const std::string& exe_dir, KrishLiveKind kind, bool nr_on, int visual_preset,
                         int pass_count, const NrAdvancedLook* look, bool* runtime_on = nullptr);

}  // namespace nexuscap
