#pragma once

namespace gdvr {

// Appends a printf-style line to %LOCALAPPDATA%\gdvr\gdvr.log (thread-safe). Each game session starts a fresh
// log (the one before is kept as gdvr.prev.log); it stops at 32 MB.
void Log(const char* fmt, ...);

}  // namespace gdvr
