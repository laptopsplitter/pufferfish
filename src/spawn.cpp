#include "spawn.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <thread>

extern char** environ;

namespace tonic {

namespace {

bool start(const std::vector<std::string>& args, int out_fd, pid_t& pid) {
  if (args.empty()) return false;
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (out_fd >= 0) posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
  else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_setsigmask(&at, &none);  // (tonic's threads may block some)
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
  const int r = posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), environ);
  posix_spawnattr_destroy(&at);
  posix_spawn_file_actions_destroy(&fa);
  return r == 0;
}

}  // namespace

std::string spawn_read(const std::vector<std::string>& argv, int timeout_s, int* status) {
  if (status) *status = -1;
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) return {};
  pid_t pid = 0;
  const bool ok = start(argv, p[1], pid);
  close(p[1]);
  if (!ok) {
    close(p[0]);
    return {};
  }
  std::string out;
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  bool killed = false;
  char buf[4096];
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      kill(pid, SIGKILL);
      killed = true;
      break;
    }
    pollfd pf{p[0], POLLIN, 0};
    if (poll(&pf, 1, static_cast<int>(left)) <= 0) continue;
    const ssize_t n = read(p[0], buf, sizeof buf);
    if (n <= 0) break;
    if (out.size() < (16u << 20)) out.append(buf, static_cast<size_t>(n));  // (16 MB is plenty)
  }
  close(p[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  if (status && !killed) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return out;
}

void spawn_detached(const std::vector<std::string>& argv) {
  pid_t pid = 0;
  if (!start(argv, -1, pid)) return;
  std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();  // reaped, no zombie
}

}  // namespace tonic
