// test_cli.h — drive a built binary from a test and get what the CLI smokes
// need: its exit code, its combined output, and a scratch directory for the
// files the command reads and writes.
//
// The child is run through the shell because the commands are the ones a human
// types. POSIX: popen/pclose, where pclose yields a wait status. Windows:
// _popen/_pclose, where _pclose yields the exit code itself. Output comes back
// through the pipe (`2>&1`), so no check needs `grep` or `/dev/null` on the
// command line — MSVC and MinGW have neither <sys/wait.h> nor a grep.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace testcli {

struct Result {
  int code = -1; // the child's exit code; -1 when it did not exit normally
  std::string out; // its stdout and stderr, combined
};

// `cmd` is the full command line, as the shell would see it.
inline Result run(const std::string &cmd) {
  Result r;
#if defined(_WIN32)
  FILE *p = _popen((cmd + " 2>&1").c_str(), "r");
#else
  FILE *p = popen((cmd + " 2>&1").c_str(), "r");
#endif
  if (!p) return r;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p)) r.out += buf;
#if defined(_WIN32)
  r.code = _pclose(p);
#else
  const int st = pclose(p);
  r.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
  return r;
}

// Quote a path for the shell when it contains a space (Windows temp/build
// paths do); untouched otherwise.
inline std::string arg(const std::string &path) {
  return path.find(' ') == std::string::npos ? path : "\"" + path + "\"";
}

// <temp>/qtest-<name>-<pid>, created (no trailing separator), so a stale file
// from an earlier run can never masquerade as a pass. Empty on failure — the
// caller reports it; every later write into it fails loudly anyway.
inline std::string scratch(const std::string &name) {
#if defined(_WIN32)
  const long pid = ::_getpid();
#else
  const long pid = ::getpid();
#endif
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec) /
                              ("qtest-" + name + "-" + std::to_string(pid));
  if (ec) return {};
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::is_directory(dir)) return {};
  return dir.string();
}

} // namespace testcli
