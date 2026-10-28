#pragma once
// Running helper programs (curl, xdg-open, xdg-mime) without a shell: the
// arguments go to the program as they are, so a name or path with quotes or
// $ in it can never become a command.
#include <string>
#include <vector>

namespace tonic {

// Runs ARGV[0] (looked up in PATH) and returns what it wrote to stdout
// (stderr is dropped). Gives up after TIMEOUT_S seconds (killed). STATUS:
// the exit code, or -1 when it could not run or was killed.
std::string spawn_read(const std::vector<std::string>& argv, int timeout_s = 15, int* status = nullptr);

// Starts ARGV[0] and does not wait for it (a file manager, say).
void spawn_detached(const std::vector<std::string>& argv);

}  // namespace tonic
