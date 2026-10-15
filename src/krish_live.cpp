#include "nexuscap/krish_live.hpp"

#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/settings_store.hpp"

#include <windows.h>

namespace nexuscap {

bool apply_krish_session(const std::string& exe_dir, KrishLiveKind kind, bool nr_on, int visual_preset,
                         int pass_count, const NrAdvancedLook* look, bool* runtime_on) {
  const int pass = clamp_nr_pass_count(pass_count);
  const int preset = clamp_renodx_visual_preset(visual_preset);
  bool files_ok = true;
  if (kind != KrishLiveKind::Nr) {
    files_ok = sync_nr_pass_count_files(exe_dir, pass, preset, look);
    if (kind != KrishLiveKind::Pass) {
      files_ok = sync_renodx_visual_preset_files(exe_dir, preset, pass, look) && files_ok;
    }
  }
  if (!runtime_on) return files_ok;
  const KrishLivePlan plan = plan_krish_live(kind, nr_on, *runtime_on);
  if (plan.reload_cycle) {
    if (!apply_neural_rendering_state(exe_dir, false, runtime_on)) return false;
    Sleep(120);  // one Present between edges or Krish sees a single toggle
    if (!apply_neural_rendering_state(exe_dir, true, runtime_on)) return false;
    return files_ok;
  }
  return apply_neural_rendering_state(exe_dir, nr_on, runtime_on) && files_ok;
}

}  // namespace nexuscap
