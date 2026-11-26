#include "../src/beast.h"
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

#include <cassert>
#include <future>
#include <thread>

#include <boost/asio/io_context.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include "testutil.h"

using namespace fcitx;
namespace net = boost::asio;

FCITX_DEFINE_STATIC_ADDON_REGISTRY(getStaticAddon)
FCITX_IMPORT_ADDON_FACTORY(getStaticAddon, beast);

namespace {

Instance *instance_;
EventDispatcher *dispatcher_;

// The hand-off fcitx5-macos uses: the request runs on the beast thread and
// marshals to the fcitx thread, waiting for the answer.
template <class F, class T = std::invoke_result_t<F>>
T withFcitx(F func) {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();
    dispatcher_->schedule([promise, func = std::move(func)]() {
        if constexpr (std::is_void_v<T>) {
            func();
            promise->set_value();
        } else {
            promise->set_value(func());
        }
    });
    return future.get();
}

} // namespace

int main() {
    char arg0[] = "testbeastmt";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=beast";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=5");
    TestConfigHome configHome;

    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance_ = &instance;
    instance.addonManager().registerDefaultLoader(&getStaticAddon());

    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    dispatcher_ = &dispatcher;

    instance.initialize();
    auto *beast = dynamic_cast<Beast *>(instance.addonManager().addon("beast"));
    assert(beast);

    // The host applies the write on the fcitx thread while the request still
    // occupies the beast thread, so the restart has to wait until the response
    // is out. Before that, this POST never returned: the beast thread waited
    // for the fcitx thread, which was waiting for the right to join it.
    beast->setConfigSetter([](const char *, const char *) {
        withFcitx([] {
            auto *addon = instance_->addonManager().addon("beast");
            RawConfig raw;
            addon->getConfig()->save(raw);
            addon->setConfig(raw);
        });
    });

    // The setter needs a running fcitx loop, so the loop gets its own thread
    // and the main thread plays the HTTP client.
    std::thread loop([&] { instance.eventLoop().exec(); });

    assert(pollCommand(true, "lsof /tmp/fcitx5.sock"));

    // The timeout is the assertion: a hung restart never answers. --fail makes
    // a 503 during the restart a retry rather than a success, so neither check
    // passes against the listener that is going away.
    const char *write = "curl -sSf --max-time 5 -o /dev/null "
                        "--unix-socket /tmp/fcitx5.sock -X POST "
                        "http://fcitx/config/addon/beast";
    const char *read = "curl -sSf --max-time 5 -o /dev/null "
                       "--unix-socket /tmp/fcitx5.sock "
                       "http://fcitx/config/addon/beast";

    // The timeout is the assertion: a hung restart never answers.
    assert(std::system(write) == 0);
    assert(pollCommand(true, read));

    // A connection that sends nothing is in flight all the same, and a restart
    // waits for the ones in flight. Two things have to hold: it must not block
    // the restart forever, and a connection arriving meanwhile must be refused
    // rather than served by a server that is about to disappear.
    {
        net::io_context client;
        net::local::stream_protocol::socket idle{client};
        boost::system::error_code ec;
        idle.connect({"/tmp/fcitx5.sock"}, ec);
        assert(!ec);

        withFcitx([&] {
            auto *addon = instance.addonManager().addon("beast");
            RawConfig raw;
            addon->getConfig()->dumpDescription(raw);
            BeastConfig cfg;
            cfg.communication.setValue(BeastCommunication::UnixSocket);
            cfg.unix_socket.mutableValue()->path.setValue(
                "/tmp/fcitx5-idle.sock");
            cfg.save(raw);
            addon->setConfig(raw);
        });

        assert(httpStatus("/tmp/fcitx5.sock", "/remote/c") == 503);

        // Reading a request that never comes is bounded, so the listener moves
        // on its own; without that bound this would never happen.
        assert(pollCommand(true, "lsof /tmp/fcitx5-idle.sock", 15000));
        assert(pollCommand(false, "lsof /tmp/fcitx5.sock"));

        // And the new listener serves: a restart that ran twice would leave
        // admission closed and answer 503 here.
        assert(httpStatus("/tmp/fcitx5-idle.sock", "/config/addon/beast") ==
               200);
    }

    // exit() only sets a flag in the backend, so the loop has to be woken to
    // see it; scheduling it puts the call on the loop's own thread.
    dispatcher.schedule([&instance] { instance.eventLoop().exit(); });
    loop.join();
    return 0;
}
