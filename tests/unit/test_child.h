// test_child.h — run a crash-prone check in a child process so a segfault
// cannot take down the rest of the suite.
//
// The child is the SAME executable re-invoked as:
//     <exe> --child <name>
// and must exit 42 on success, non-zero on failure. runSelfChild() returns
// the child's exit code, or -1 if the child crashed (signal on POSIX,
// STATUS_* on Windows).
//
// POSIX: fork + execl.  Windows: CreateProcess + WaitForSingleObject.

#pragma once

#include <cstddef>

namespace testchild {

// `exePath` is argv[0] (used on POSIX; ignored on Windows, where the
// module path is resolved via GetModuleFileNameA).
int runSelfChild(const char *exePath, const char *name);

}  // namespace testchild

#if defined(_WIN32)

#include <windows.h>

#include <string>

namespace testchild {

inline int runSelfChild(const char *, const char *name) {
  char exePath[MAX_PATH] = {0};
  if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    return -1;
  std::string cmd = std::string("\"") + exePath + "\" --child " + name;
  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(exePath, const_cast<char *>(cmd.c_str()), nullptr,
                      nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
    return -1;
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  bool ok = GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  if (!ok || (code & 0x80000000u) != 0)  // STATUS_* => crash
    return -1;
  return static_cast<int>(code);
}

}  // namespace testchild

#else  // POSIX

#include <sys/wait.h>
#include <unistd.h>

namespace testchild {

inline int runSelfChild(const char *exePath, const char *name) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    execl(exePath, exePath, "--child", name, (char *)nullptr);
    _exit(127);  // execl failed
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0)
    return -1;
  if (WIFSIGNALED(st))
    return -1;
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  return -1;
}

}  // namespace testchild

#endif  // _WIN32
