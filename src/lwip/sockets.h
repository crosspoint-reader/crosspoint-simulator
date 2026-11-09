#pragma once

#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The simulator's own socket code closes descriptors through this name so the
// native Windows build can map it to closesocket().
inline int simCloseSocket(int fd) { return ::close(fd); }
// A second descriptor for the same connection (see the Windows version).
inline int simDupSocket(int fd) { return ::dup(fd); }

#else  // Native Windows (MSYS2 UCRT64): lwip's descriptor-style sockets on Winsock.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// <windows.h> declares the function Sleep(); firmware code uses Sleep as an
// enumerator name (a Home menu item), so the Windows one is renamed while
// Winsock's headers are read.
#define Sleep SimWindowsSleep
#include <winsock2.h>
#include <ws2tcpip.h>
#undef Sleep

// <windows.h> (pulled in by Winsock) defines macros with ordinary names that
// the firmware uses as enumerators, e.g. SHORT_PWRBTN::IGNORE. Nothing here
// needs them.
#undef IGNORE
#undef ERROR
#undef DELETE
#undef TRANSPARENT
#undef OPAQUE
#undef IN
#undef OUT
#undef OPTIONAL

#include <fcntl.h>
#include <sys/types.h>

#include <cstddef>
#include <cstdint>

// Winsock needs WSAStartup() before the first socket(); an inline variable
// runs it once per program, before main().
inline const bool simWinsockStarted = [] {
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}();

inline int simCloseSocket(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }

// dup() works on C runtime file descriptors only; a Winsock socket is not one,
// so dup() of it fails and a response written through the copy is lost.
// Winsock duplicates a socket through its protocol info instead. A socket made
// by socket()/accept() must be duplicated with WSA_FLAG_OVERLAPPED, or its
// send/receive timeouts never take effect (a blocked call never times out).
inline int simDupSocket(int fd) {
  WSAPROTOCOL_INFOW info;
  if (WSADuplicateSocketW(static_cast<SOCKET>(fd), GetCurrentProcessId(), &info) != 0)
    return -1;
  const SOCKET copy = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                                 WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
  return copy == INVALID_SOCKET ? -1 : static_cast<int>(copy);
}

// shutdown() directions: Winsock names them SD_*.
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif
#ifndef SHUT_WR
#define SHUT_WR SD_SEND
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif

// lwip/POSIX callers pass byte pointers and size_t lengths; Winsock takes
// char pointers and int lengths. These overloads are exact matches for those
// calls, so they win over the Winsock declarations.
inline ssize_t send(int fd, const char *data, size_t len, int flags) {
  return ::send(static_cast<SOCKET>(fd), data, static_cast<int>(len), flags);
}
inline ssize_t send(int fd, const uint8_t *data, size_t len, int flags) {
  return ::send(static_cast<SOCKET>(fd), reinterpret_cast<const char *>(data), static_cast<int>(len), flags);
}
inline ssize_t recv(int fd, char *data, size_t len, int flags) {
  return ::recv(static_cast<SOCKET>(fd), data, static_cast<int>(len), flags);
}
inline ssize_t recv(int fd, uint8_t *data, size_t len, int flags) {
  return ::recv(static_cast<SOCKET>(fd), reinterpret_cast<char *>(data), static_cast<int>(len), flags);
}

// Winsock takes SO_RCVTIMEO / SO_SNDTIMEO as a DWORD of milliseconds, POSIX
// as a timeval; every other option keeps its bytes.
inline int setsockopt(int fd, int level, int name, const void *value, size_t len) {
  if (level == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == sizeof(timeval)) {
    const auto *tv = static_cast<const timeval *>(value);
    const DWORD ms = static_cast<DWORD>(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    return ::setsockopt(static_cast<SOCKET>(fd), level, name, reinterpret_cast<const char *>(&ms),
                        static_cast<int>(sizeof(ms)));
  }
  return ::setsockopt(static_cast<SOCKET>(fd), level, name, static_cast<const char *>(value), static_cast<int>(len));
}

// The firmware makes its UDP discovery socket non-blocking with fcntl(), as
// lwip allows; only that use is mapped.
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
inline int fcntl(int fd, int cmd, int arg = 0) {
  if (cmd == F_GETFL) return 0;
  if (cmd != F_SETFL) return -1;
  u_long nonBlocking = (arg & O_NONBLOCK) ? 1 : 0;
  return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonBlocking) == 0 ? 0 : -1;
}

#endif  // _WIN32
