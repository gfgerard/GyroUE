#pragma once
#include <string>

namespace gyroue {

// Plain-text log beside the mod (GyroUE\GyroUE.log), one line per call, flushed immediately.
void log_open(const std::wstring& path);
void log(const char* fmt, ...);

// Probe results go to their own file (GyroUE\GyroUE-probe.log), recreated each session with a header
// that explains how to read it. probe_log falls back to GyroUE.log if the file couldn't be opened.
bool probe_log_open(const std::wstring& path, const std::string& header);
void probe_log(const char* fmt, ...);

std::string narrow(const std::wstring& text);

}  // namespace gyroue
