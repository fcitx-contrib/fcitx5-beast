#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>

#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include "testutil.h"

using namespace fcitx;
namespace http = boost::beast::http;
using local = boost::asio::local::stream_protocol;

FCITX_DEFINE_STATIC_ADDON_REGISTRY(getStaticAddon)
FCITX_IMPORT_ADDON_FACTORY(getStaticAddon, beast);

namespace {

Instance *instance_;
EventDispatcher *dispatcher_;

const std::string configTarget = "/config/addon/beast";

// The hand-off fcitx5-macos uses: the request runs on the server thread, has
// the write applied on the fcitx thread, and waits for it.
void onFcitx(const std::function<void()> &work) {
    onEventLoop(*dispatcher_, work);
}

// Moves the server to another unix socket, the way a host applies a write.
void moveTo(const std::string &path) {
    auto *addon = instance_->addonManager().addon("beast");
    RawConfig raw;
    addon->getConfig()->dumpDescription(raw);
    BeastConfig config;
    config.communication.setValue(BeastCommunication::UnixSocket);
    config.unix_socket.mutableValue()->path.setValue(path);
    config.save(raw);
    addon->setConfig(raw);
}

} // namespace

int main() {
    char arg0[] = "testbeastmt";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=beast";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=5");
    TestConfigHome configHome;

    const std::string firstSocket = configHome.path("beast.sock").string();
    const std::string movedPath = configHome.path("moved.sock").string();
    seedBeastSocket(firstSocket);

    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance_ = &instance;
    instance.addonManager().registerDefaultLoader(&getStaticAddon());

    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    dispatcher_ = &dispatcher;

    instance.initialize();
    auto *beast = dynamic_cast<Beast *>(instance.addonManager().addon("beast"));
    CHECK(beast);

    const local::endpoint socketEndpoint{firstSocket};
    const local::endpoint movedEndpoint{movedPath};

    beast->setConfigSetter([](const char *, const char *) {
        onFcitx([] {
            auto *addon = instance_->addonManager().addon("beast");
            RawConfig raw;
            addon->getConfig()->save(raw);
            addon->setConfig(raw);
        });
    });

    std::thread loop([&] { instance.eventLoop().exec(); });
    CHECK(waitUntil([&] { return serving(socketEndpoint, configTarget); }));

    // Writing the config of the addon that is answering the request restarts
    // its own listener. The request must still be answered: before the fix it
    // waited for the fcitx thread, which was waiting to join it.
    const auto written =
        request(socketEndpoint, http::verb::post, configTarget);
    CHECK(written && written->status == 200);
    CHECK(waitUntil([&] { return serving(socketEndpoint, configTarget); }));

    // A handler slower than the read deadline must still get its answer out:
    // the write re-arms the deadline instead of inheriting the one the read
    // left behind, which has already expired by then.
    beast->setConfigGetter([](const char *) {
        std::this_thread::sleep_for(std::chrono::seconds(6));
        return std::string("slow");
    });
    const auto slow = request(socketEndpoint, http::verb::get, configTarget,
                              std::chrono::seconds(15));
    CHECK(slow && slow->status == 200 && slow->body == "slow");
    beast->setConfigGetter([](const char *) { return std::string(); });

    // A connection that sends nothing is in flight all the same. The restart
    // waits for it, refuses what arrives meanwhile, and still runs once the
    // connection's read deadline passes.
    {
        boost::asio::io_context ioc;
        local::socket idle{ioc};
        idle.connect(socketEndpoint);
        // The server has run its accept loop since the idle connection arrived.
        CHECK(serving(socketEndpoint, configTarget));

        onFcitx([&movedPath] { moveTo(movedPath); });

        const auto during =
            request(socketEndpoint, http::verb::get, configTarget);
        CHECK(during && during->status == 503);

        CHECK(waitUntil([&] { return serving(movedEndpoint, configTarget); },
                        15000));
        CHECK(waitUntil(
            [&] {
                return !request(socketEndpoint, http::verb::get, configTarget);
            },
            5000));
    }

    dispatcher.schedule([&instance] { instance.eventLoop().exit(); });
    loop.join();
    return 0;
}
