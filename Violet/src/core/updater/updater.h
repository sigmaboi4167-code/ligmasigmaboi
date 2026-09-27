#pragma once
#include <string>
#include <vector>
#include <functional>

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

// Granular API used by the graphical launcher.
bool FetchRemoteVersion(std::string& out); // trimmed version.txt, "" on failure
bool FetchChangelog(std::string& out);     // trimmed + capped notes, "" on failure
// Downloads the latest exe. progress gets 0..1 (or <0 when size unknown).
bool DownloadLatest(std::vector<char>& exe, std::function<void(float)> progress = {});
// Swaps the downloaded exe over the running binary and relaunches it.
bool InstallAndRelaunch(const std::vector<char>& exe);

// Legacy all-in-one console flow. Returns true if an update was installed
// and the app should exit immediately (the new binary takes over).
bool CheckAndUpdate();
} // namespace Updater
