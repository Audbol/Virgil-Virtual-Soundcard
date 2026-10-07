// Virgil Control: opens the virgild control panel in the default browser.
//
// If no virgild is answering and a virgild binary sits next to this program (the
// portable downloads), it is started first, using virgil.conf from the same
// folder when present. Installed setups run virgild as a service, so the panel
// is normally already up.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "virgil/platform.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace {

#if defined(_WIN32)
using Sock = SOCKET;
const Sock kBad = INVALID_SOCKET;
void close_sock(Sock s) { closesocket(s); }
#else
using Sock = int;
const Sock kBad = -1;
void close_sock(Sock s) { ::close(s); }
#endif

// True if a virgild control panel answers on 127.0.0.1:port.
bool panel_up(unsigned port) {
  Sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBad) return false;
#if defined(_WIN32)
  DWORD tv = 1000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
  timeval tv{1, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<unsigned short>(port));
  sa.sin_addr.s_addr = htonl(0x7F000001);
  bool ok = false;
  if (connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) {
    const std::string req = "GET /api/status HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                            "\r\nConnection: close\r\n\r\n";
    send(s, req.data(), static_cast<int>(req.size()), 0);
    char buf[64] = {};
    const int n = recv(s, buf, sizeof buf - 1, 0);
    ok = n > 12 && std::strncmp(buf, "HTTP/1.1 200", 12) == 0;
  }
  close_sock(s);
  return ok;
}

std::string exe_dir() {
  std::string p;
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  p.assign(buf, n);
#elif defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0) {
    char real[4096];
    p = realpath(buf, real) ? real : buf;
  }
#else
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) p.assign(buf, size_t(n));
#endif
  const size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

bool exists(const std::string& path) {
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

#if defined(_WIN32)
const char kSep = '\\';
const char* kDaemon = "virgild.exe";
#else
const char kSep = '/';
const char* kDaemon = "virgild";
#endif

// Locate a portable virgild: next to us, or next to the .app bundle on macOS.
std::string find_daemon_dir() {
  const std::string here = exe_dir();
  std::vector<std::string> dirs = {here};
#if defined(__APPLE__)
  dirs.push_back(here + "/../../..");  // Virgil Control.app/Contents/MacOS -> folder
#endif
  for (const auto& d : dirs)
    if (exists(d + kSep + kDaemon)) return d;
  return {};
}

bool start_daemon(const std::string& dir, unsigned port) {
  const std::string daemon = dir + kSep + kDaemon;
  const std::string conf = dir + kSep + "virgil.conf";
  const std::string log = dir + kSep + "virgild.log";
  std::vector<std::string> args = {daemon, "--log", log, "--control-port", std::to_string(port)};
  if (exists(conf)) {
    args.push_back("-c");
    args.push_back(conf);
  }
#if defined(_WIN32)
  std::string cmd;
  for (const auto& a : args) cmd += "\"" + a + "\" ";
  STARTUPINFOA si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr,
                      dir.c_str(), &si, &pi))
    return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    signal(SIGHUP, SIG_IGN);
    if (fork() != 0) _exit(0);  // detach completely
    const int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    if (chdir(dir.c_str()) != 0) _exit(1);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execv(daemon.c_str(), argv.data());
    _exit(127);
  }
  return true;
#endif
}

void open_url(const std::string& url) {
#if defined(_WIN32)
  ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
  const char* opener = "/usr/bin/open";
#else
  const char* opener = "xdg-open";
#endif
  const pid_t pid = fork();
  if (pid == 0) {
    execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
#endif
}

void message(const std::string& text) {
#if defined(_WIN32)
  MessageBoxA(nullptr, text.c_str(), "Virgil Control", MB_OK | MB_ICONINFORMATION);
#else
  std::fprintf(stderr, "%s\n", text.c_str());
#if defined(__APPLE__)
  // Double-clicked apps have no terminal: show a dialog too.
  std::string esc;
  for (char c : text) esc += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
  const std::string cmd = "/usr/bin/osascript -e 'display dialog \"" + esc +
                          "\" with title \"Virgil Control\" buttons {\"OK\"}' >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0) { /* dialog unavailable: stderr already has it */ }
#endif
#endif
}

int run(int argc, char** argv) {
  unsigned port = 8480;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--port") && i + 1 < argc) port = unsigned(std::atoi(argv[++i]));
    else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
      std::printf("usage: virgil-control [--port N]\nOpens the Virgil control panel in your browser.\n");
      return 0;
    }
  }
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";

  if (!panel_up(port)) {
    const std::string dir = find_daemon_dir();
    if (dir.empty()) {
      message("The Virgil service is not running.\n\nStart it (Windows: Start menu > Virgil > Restart "
              "Virgil service; macOS: sudo launchctl kickstart -k system/org.virgil.virgild; Linux: sudo "
              "systemctl start virgild) and open Virgil Control again.");
      return 1;
    }
    if (!start_daemon(dir, port)) {
      message("Could not start " + dir + kSep + kDaemon);
      return 1;
    }
    bool up = false;
    for (int i = 0; i < 80 && !(up = panel_up(port)); ++i) virgil::sleep_until_ns(virgil::mono_ns() + 100000000LL);
    if (!up) {
      message("virgild was started but its control panel did not come up.\nSee " + dir + kSep +
              "virgild.log for details.");
      return 1;
    }
  }
  open_url(url);
  return 0;
}

}  // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return run(__argc, __argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
