#ifndef _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_
#define _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_

#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/http.hpp>
#include <fcitx-config/iniparser.h>
#include <fcitx-utils/eventdispatcher.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <unistd.h>

#include "../src/beast.h"

// assert() is compiled out in a release build; a check that has to hold must
// not be.
#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,        \
                         __LINE__, #condition);                                \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

// beast keeps beast.conf in the fcitx configuration directory, so a test needs
// one of its own. FCITX_CONFIG_HOME and XDG_CONFIG_HOME both have to point into
// it: the first wins when it is set, and run by hand either would otherwise
// rewrite the configuration of whoever started the test.
class TestConfigHome {
public:
    TestConfigHome() {
        char pattern[] = "/tmp/fcitx5-beast-test-XXXXXX";
        const auto *dir = ::mkdtemp(pattern);
        CHECK(dir);
        dir_ = dir;
        const auto home = dir_ / "fcitx5";
        std::error_code ec;
        std::filesystem::create_directories(home, ec);
        CHECK(!ec);
        CHECK(::setenv("FCITX_CONFIG_HOME", home.c_str(), 1) == 0);
        CHECK(::setenv("XDG_CONFIG_HOME", dir_.c_str(), 1) == 0);
    }

    ~TestConfigHome() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    TestConfigHome(const TestConfigHome &) = delete;
    TestConfigHome &operator=(const TestConfigHome &) = delete;

    // A path inside the directory, for files and sockets that must not collide
    // with what is already running.
    std::filesystem::path path(const std::string &name) const {
        return dir_ / name;
    }

private:
    std::filesystem::path dir_;
};

// Points beast's first listener into the test's own directory. The default path
// may belong to a daemon that is running, and startServer unlinks it.
inline void seedBeastSocket(const std::filesystem::path &socketPath) {
    fcitx::BeastConfig config;
    config.communication.setValue(fcitx::BeastCommunication::UnixSocket);
    config.unix_socket.mutableValue()->path.setValue(socketPath.string());
    CHECK(fcitx::safeSaveAsIni(config, "conf/beast.conf"));
}

// Ports that are free right now, so the test does not take one a running daemon
// is listening on.
inline std::vector<boost::asio::ip::port_type> freePorts(std::size_t count) {
    boost::asio::io_context ioc;
    std::vector<boost::asio::ip::tcp::acceptor> acceptors;
    std::vector<boost::asio::ip::port_type> ports;
    for (std::size_t i = 0; i < count; ++i) {
        acceptors.emplace_back(
            ioc, boost::asio::ip::tcp::endpoint{boost::asio::ip::tcp::v4(), 0});
        ports.push_back(acceptors.back().local_endpoint().port());
    }
    return ports;
}

struct Reply {
    unsigned status;
    std::string body;
};

// One request on a new connection to a unix socket or a TCP port, or nothing
// when nothing is listening. A timeout throws: a hang is a failure, while a
// refused or dropped connection is simply unavailable.
template <class Endpoint>
std::optional<Reply>
request(const Endpoint &endpoint, boost::beast::http::verb verb,
        const std::string &target,
        std::chrono::seconds timeout = std::chrono::seconds(5)) {
    using Protocol = typename Endpoint::protocol_type;
    namespace http = boost::beast::http;

    boost::asio::io_context ioc;
    boost::beast::basic_stream<Protocol> stream{ioc};
    boost::system::error_code failure;

    http::request<http::string_body> message{verb, target, 11};
    message.set(http::field::host, "fcitx");
    message.prepare_payload();
    http::response<http::string_body> response;
    boost::beast::flat_buffer buffer;

    stream.expires_after(timeout);
    stream.async_connect(endpoint, [&](boost::system::error_code ec) {
        if (ec) {
            failure = ec;
            return;
        }
        http::async_write(
            stream, message, [&](boost::system::error_code ec, std::size_t) {
                if (ec) {
                    failure = ec;
                    return;
                }
                http::async_read(stream, buffer, response,
                                 [&](boost::system::error_code ec,
                                     std::size_t) { failure = ec; });
            });
    });
    ioc.run();

    if (failure == boost::beast::error::timeout ||
        failure == boost::asio::error::timed_out) {
        throw boost::system::system_error(failure);
    }
    if (failure) {
        return std::nullopt;
    }
    return Reply{response.result_int(), response.body()};
}

template <class Endpoint>
bool serving(const Endpoint &endpoint, const std::string &target) {
    const auto reply = request(endpoint, boost::beast::http::verb::get, target);
    return reply && reply->status == 200;
}

// Runs work on the fcitx thread and waits for it, the way fcitx5 does, so that
// what the addon defers to its event loop actually runs.
inline void onEventLoop(fcitx::EventDispatcher &dispatcher,
                        const std::function<void()> &work) {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    dispatcher.schedule([promise, work] {
        work();
        promise->set_value();
    });
    future.get();
}

// The server runs on its own thread, so the listener it moves to appears
// asynchronously: a check has to wait for the expected state.
inline bool waitUntil(const std::function<bool()> &ready,
                      int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ready();
}

#endif // _FCITX5_MODULES_BEAST_TEST_TESTUTIL_H_
