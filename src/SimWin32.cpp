#ifdef _WIN32

#include "SimWin32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

namespace {

std::wstring widen(const std::string &utf8) {
  if (utf8.empty())
    return {};
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), length);
  return wide;
}

// Quotes one argument so that a program splitting its command line by the
// Microsoft C runtime rules (CommandLineToArgvW) gets it back unchanged:
// backslashes are literal unless they precede a quote.
std::wstring quoteArgument(const std::wstring &argument) {
  if (!argument.empty() &&
      argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return argument;
  std::wstring quoted = L"\"";
  for (auto it = argument.begin();; ++it) {
    size_t backslashes = 0;
    while (it != argument.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == argument.end()) {
      quoted.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      quoted.append(backslashes * 2 + 1, L'\\');
    } else {
      quoted.append(backslashes, L'\\');
    }
    quoted.push_back(*it);
  }
  quoted.push_back(L'"');
  return quoted;
}

} // namespace

namespace sim_win32 {

int runCapture(const std::string &program,
               const std::vector<std::string> &args, std::string &stdoutText) {
  SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0))
    return -1;
  // Of the pipe, only the write end goes to the child (it also inherits the
  // process's other inheritable handles, as with _popen()).
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  std::wstring commandLine = quoteArgument(widen(program));
  for (const std::string &arg : args) {
    commandLine += L' ';
    commandLine += quoteArgument(widen(arg));
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = writeEnd;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION child{};
  const BOOL started =
      CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
  const DWORD startError = started ? 0 : GetLastError();
  // The child holds its own copy; closing ours lets ReadFile see the end.
  CloseHandle(writeEnd);
  if (!started) {
    CloseHandle(readEnd);
    std::fprintf(stderr, "[SIM] Cannot start %s (error %lu)\n",
                 program.c_str(), startError);
    return -1;
  }

  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
    stdoutText.append(buffer, read);
  }
  CloseHandle(readEnd);

  WaitForSingleObject(child.hProcess, INFINITE);
  DWORD status = 1;
  GetExitCodeProcess(child.hProcess, &status);
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  return static_cast<int>(status);
}

std::string narrowToUtf8(const std::string &narrow) {
  if (narrow.empty())
    return {};
  const int wideLength = MultiByteToWideChar(
      CP_ACP, 0, narrow.data(), static_cast<int>(narrow.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(wideLength), L'\0');
  MultiByteToWideChar(CP_ACP, 0, narrow.data(),
                      static_cast<int>(narrow.size()), wide.data(),
                      wideLength);
  const int utf8Length =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0,
                          nullptr, nullptr);
  std::string utf8(static_cast<size_t>(utf8Length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, utf8.data(),
                      utf8Length, nullptr, nullptr);
  return utf8;
}

bool replaceFile(const std::string &from, const std::string &to) {
  return MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace sim_win32

#endif
