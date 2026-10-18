#pragma once
// Adds the launcher to Steam as a non-Steam (VR) game, or removes it, by editing each Steam account's
// userdata\<id>\config\shortcuts.vdf. Steam rewrites that file when it exits, so it must not be running.

#include <filesystem>
#include <string>

namespace gdvr::steam {

enum class Result { Ok, SteamRunning, NoSteam, Failed };

// Adds (or updates) the "Grim Dawn VR" entry for `exe` in every Steam account on this PC.
Result AddShortcut(const std::filesystem::path& exe, std::wstring& detail);

// Removes the entries that start `exe`.
Result RemoveShortcut(const std::filesystem::path& exe, std::wstring& detail);

// Reads and rewrites a shortcuts.vdf into `out` without changes (tests the reader/writer).
bool RoundTrip(const std::filesystem::path& in, const std::filesystem::path& out);

}  // namespace gdvr::steam
