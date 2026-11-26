#ifndef _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_
#define _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <unistd.h>

// Helpers shared by the beast tests.

// Each test writes beast.conf and asserts on the endpoint it reads back, so it
// needs a config directory of its own. It must never use one it inherited: run
// by hand with XDG_CONFIG_HOME set, that would rewrite the user's fcitx
// configuration.
class TestConfigHome {
public:
    TestConfigHome() {
        dir_ = std::filesystem::temp_directory_path() /
               ("fcitx5-beast-test-" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        ::setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
    }

    ~TestConfigHome() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    TestConfigHome(const TestConfigHome &) = delete;
    TestConfigHome &operator=(const TestConfigHome &) = delete;

private:
    std::filesystem::path dir_;
};

// The server runs on its own thread, so listeners appear and disappear
// asynchronously; a check has to wait for the expected state rather than read
// it once.
inline bool pollCommand(bool expected, const char *command,
                        int timeoutMs = 3000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if ((std::system(command) == 0) == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// The status a request on a unix socket endpoint is answered with, or -1 when
// it could not be made at all.
inline int httpStatus(const std::string &socketPath, const std::string &target,
                      int timeoutSeconds = 5) {
    const std::string command = "curl -s -o /dev/null -w '%{http_code}' "
                                "--max-time " +
                                std::to_string(timeoutSeconds) +
                                " --unix-socket " + socketPath +
                                " http://fcitx" + target;
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return -1;
    }
    char buffer[8] = {};
    const auto read = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
    ::pclose(pipe);
    return read == 0 ? -1 : std::atoi(buffer);
}

// The body of a successful request, or an empty string when it failed; --fail
// makes a non-2xx status a failure rather than a body.
inline std::string httpBody(const std::string &socketPath,
                            const std::string &target, int timeoutSeconds = 5) {
    const std::string command = "curl -sSf --max-time " +
                                std::to_string(timeoutSeconds) +
                                " --unix-socket " + socketPath +
                                " http://fcitx" + target + " 2>/dev/null";
    FILE *pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    std::string body;
    char buffer[256];
    while (const auto read = std::fread(buffer, 1, sizeof(buffer), pipe)) {
        body.append(buffer, read);
    }
    ::pclose(pipe);
    return body;
}

#endif // _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_
