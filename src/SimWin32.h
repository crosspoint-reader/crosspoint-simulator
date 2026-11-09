#pragma once

// Windows-only helpers that need <windows.h>. They live in their own
// translation unit so the Windows macros (ERROR, IGNORE, DELETE, ...) never
// reach headers that firmware code includes.
#ifdef _WIN32

#include <string>
#include <vector>

namespace sim_win32 {

// Runs `program` with `args`, without a shell, and collects what it writes to
// stdout. Arguments are UTF-8; each reaches the program as one argv entry, so
// quotes, '&' or '%' in it mean nothing. Returns the program's exit code
// (a crash code is negative), or -1 if it cannot be started.
int runCapture(const std::string &program,
               const std::vector<std::string> &args, std::string &stdoutText);

// Converts a string in the C runtime's narrow code page (for example a path
// built from getenv("TEMP")) to UTF-8, for runCapture.
std::string narrowToUtf8(const std::string &narrow);

// Moves `from` over `to` in one step, replacing `to` if it exists. When the
// move fails, `to` is left as it was. Paths use the same code page as the C
// runtime's narrow file functions the rest of the simulator calls.
bool replaceFile(const std::string &from, const std::string &to);

} // namespace sim_win32

#endif
