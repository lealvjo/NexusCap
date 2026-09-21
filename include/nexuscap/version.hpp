#pragma once

#include <string>

namespace nexuscap {

// Single source of truth for product version (UI, Analyze, installer, VERSIONINFO).
inline constexpr int kAppVersionMajor = 4;
inline constexpr int kAppVersionMinor = 0;
inline constexpr int kAppVersionPatch = 0;
inline constexpr const char* kAppVersion = "4.0.0";
inline constexpr const char* kAppDisplayName = "Nexus Cap";
inline constexpr const char* kAppNameWithVersion = "Nexus Cap  v4.0.0";
inline constexpr const char* kAppAnalyzeBuild = "NexusCap-4.0.0";

inline bool app_version_is(int major, int minor, int patch) {
  return major == kAppVersionMajor && minor == kAppVersionMinor && patch == kAppVersionPatch;
}

// ASCII-only window titles (no em-dash) — avoids mojibake when MSVC source encoding != UTF-8.
inline std::string app_window_title_session() {
  return std::string("Nexus Cap v") + kAppVersion;
}
inline std::string app_window_title_lobby() {
  return std::string("Nexus Cap v") + kAppVersion + " - Console de captura";
}

}  // namespace nexuscap
