#pragma once

namespace gdvr {

// Appends a printf-style line to %LOCALAPPDATA%\gdvr\gdvr.log (thread-safe).
void Log(const char* fmt, ...);

}  // namespace gdvr
