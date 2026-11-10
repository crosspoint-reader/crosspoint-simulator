#pragma once

// POSIX file calls the simulator's storage shims use that native Windows
// (MSYS2 UCRT64 / MinGW-w64) spells differently. Empty on macOS and Linux.
#ifdef _WIN32

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>

// mkdir(path, mode): Windows has no permission bits, the mode is ignored.
inline int mkdir(const char *path, int /*mode*/) { return ::_mkdir(path); }

#include <cstdlib>

// setenv()/unsetenv() are POSIX; the CRT has _putenv_s(), where an empty
// value removes the variable.
inline int setenv(const char *name, const char *value, int overwrite) {
  if (!overwrite && std::getenv(name) != nullptr) return 0;
  return ::_putenv_s(name, value ? value : "") == 0 ? 0 : -1;
}
inline int unsetenv(const char *name) { return ::_putenv_s(name, "") == 0 ? 0 : -1; }

// fsync() flushes a descriptor to disk; the CRT calls it _commit().
inline int simFsync(int fd) { return ::_commit(fd); }
#define fsync simFsync

// Descriptors open in text mode by default and would translate CR/LF in
// binary files (BMP, EPUB, caches); every simulator open() is binary.
#define SIM_OPEN_BINARY O_BINARY

#else

#define SIM_OPEN_BINARY 0

#endif
