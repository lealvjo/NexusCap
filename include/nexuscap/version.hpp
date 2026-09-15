#pragma once

namespace nexuscap {

// Single source of truth for product version (UI, Analyze, installer, VERSIONINFO).
inline constexpr int kAppVersionMajor = 1;
inline constexpr int kAppVersionMinor = 1;
inline constexpr int kAppVersionPatch = 2;
inline constexpr const char* kAppVersion = "1.1.2";
inline constexpr const char* kAppDisplayName = "Nexus Cap";
inline constexpr const char* kAppNameWithVersion = "Nexus Cap  v1.1.2";
inline constexpr const char* kAppAnalyzeBuild = "NexusCap-1.1.2";

inline bool app_version_is(int major, int minor, int patch) {
  return major == kAppVersionMajor && minor == kAppVersionMinor && patch == kAppVersionPatch;
}

}  // namespace nexuscap
