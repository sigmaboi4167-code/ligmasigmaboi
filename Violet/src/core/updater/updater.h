#pragma once
#include <string>

namespace Updater {
// Bump BOTH this and version.txt when shipping — CI rebuilds the exe and
// clients pull it on next launch.
inline constexpr const char* kLocalVersion = "1.0.2";
inline constexpr const char* kVersionUrl =
    "https://raw.githubusercontent.com/sigmaboi4167-code/ligmasigmaboi/main/version.txt";
inline constexpr const char* kChangelogUrl =
    "https://raw.githubusercontent.com/sigmaboi4167-code/ligmasigmaboi/main/changelog.txt";
inline constexpr const char* kExeUrl =
    "https://github.com/sigmaboi4167-code/ligmasigmaboi/releases/latest/download/Violet.exe";

// Checks GitHub for a newer version. If one is found it downloads Violet.exe,
// swaps it over the running binary and relaunches. Returns true when an
// update was installed (caller should exit immediately).
bool CheckAndUpdate();
} // namespace Updater
