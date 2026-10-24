// Running a command line without a window and reading what it printed: how
// the launcher asks Python for its version and the server to install a pack.

#pragma once

#include <string>

// Runs a command line without a window and returns its stdout and stderr; false
// if it did not start. A command still running after timeoutMs is cut off.
bool RunCapture(const std::wstring& line, std::string& out, unsigned long timeoutMs);
