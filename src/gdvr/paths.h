#pragma once
// Where the mod's files are.

#include <string>

namespace gdvr::paths {

// The mod's install folder (skybox/ and other shipped files): in development builds the repo, otherwise
// the folder the launcher names (GDVR_HOME, as the DLL runs from a copy) or the folder of gdvr.dll.
std::wstring InstallDir();

// Per-user files: %LOCALAPPDATA%\gdvr (settings, log, sky captures). Created on first use.
std::wstring UserDir();

}  // namespace gdvr::paths
