#pragma once
// Update check: at most once a day, the launcher asks GitHub for the newest Grim Dawn VR release. If it
// is newer, the player is asked; on yes the installer is downloaded, checked against GitHub's SHA-256
// and run (it upgrades in place, keeps the settings, and starts the game again afterwards).
//
// Settings in %LOCALAPPDATA%\gdvr\launcher.ini, section [update]: check=0 turns it off; skip=<version>
// is a version the player said not to ask about again; last_check is when it last asked GitHub.

#include <filesystem>

namespace gdvr::update {

// True if an update is being installed: the launcher should exit (the installer starts the game again).
bool OfferUpdate(const std::filesystem::path& userDir, const wchar_t* currentVersion);

// Test: logs what the check finds on GitHub as if this were `asVersion`. True if that is newer.
bool Probe(const std::filesystem::path& userDir, const wchar_t* asVersion, bool download);

}  // namespace gdvr::update
