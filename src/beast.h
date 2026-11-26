#ifndef _FCITX5_MODULES_BEAST_BEAST_H_
#define _FCITX5_MODULES_BEAST_BEAST_H_

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#define FCITX5_BEAST_HAS_UNIX_SOCKET
#endif

#include <atomic>
#include <boost/asio.hpp>
#include <fcitx-utils/i18n.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/instance.h>
#include <memory>
#include <mutex>
#include <thread>

namespace asio = boost::asio;

// fcitx in numpad
#define DEFAULT_PORT 32489
#define DEFAULT_UNIX_SOCKET_PATH "/tmp/fcitx5.sock"

using ConfigGetter = std::function<std::string(const char *)>;
using ConfigSetter = std::function<void(const char *, const char *)>;
using RemoteHandler = std::function<std::pair<bool, std::string>(
    const std::string_view, const char *)>;

namespace fcitx {

FCITX_CONFIGURATION(BeastTcpConfig, Option<int, IntConstrain> port{
                                        this, "Port", _("Port"), DEFAULT_PORT,
                                        IntConstrain(1024, 65535)};);

FCITX_CONFIGURATION(BeastUnixSocketConfig,
                    Option<std::string> path{this, "Path", _("Path"),
                                             DEFAULT_UNIX_SOCKET_PATH};);

enum class BeastCommunication { UnixSocket, TCP };
FCITX_CONFIG_ENUM_NAME(BeastCommunication,
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
                       "Unix Socket",
#endif
                       "TCP");

FCITX_CONFIGURATION(BeastConfig,
                    Option<BeastCommunication> communication{
                        this, "Communication", _("Communication"),
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
                        BeastCommunication::UnixSocket
#else
                        BeastCommunication::TCP
#endif
                    };
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
                    Option<BeastUnixSocketConfig> unix_socket{this,
                                                              "Unix Socket",
                                                              "Unix Socket",
                                                              {}};
#endif
                    Option<BeastTcpConfig> tcp{this, "TCP", "TCP", {}};);

extern std::shared_ptr<const ConfigGetter> configGetter_;
extern std::shared_ptr<const ConfigSetter> configSetter_;
extern std::shared_ptr<const RemoteHandler> remoteHandler_;

class Beast : public AddonInstance {
public:
    Beast(Instance *instance);
    ~Beast();

    Instance *instance() { return instance_; }

    const Configuration *getConfig() const override { return &config_; }
    void setConfig(const RawConfig &config) override;

    void reloadConfig() override;
    // The host installs these after the addon is created, which is after the
    // server thread has started, so installing them is synchronised.
    void setConfigGetter(ConfigGetter getter);
    void setConfigSetter(ConfigSetter setter);
    void setRemoteHandler(RemoteHandler handler);

    // A request is a connection: beast answers one request per connection and
    // closes it. Counting them is what lets a restart wait for the handler that
    // is still on the server thread. A false start means the request has to be
    // refused; every true one needs a matching requestFinished().
    bool requestStarted();
    void requestFinished();

private:
    static const inline std::string ConfPath = "conf/beast.conf";
    void startThread();
    void stopThread();
    void startServer(const std::shared_ptr<asio::io_context> &context,
                     BeastCommunication communication,
                     const std::string &socketPath, int port);
    // Restarts the server now, or once the requests in flight are answered.
    void restartWhenIdle();
    // Consumes a deferred restart, unless a newer reload already performed one.
    void restartIfPending();
    void restartServer();
    Instance *instance_;
    BeastConfig config_;
    std::shared_ptr<asio::io_context> ioc;
    std::thread serverThread_;

    // A config change usually restarts the server, but a request handler on the
    // server thread may be blocked waiting for the fcitx thread that is
    // applying the change; joining that thread from there would hang both.
    // These guard that hand-off.
    std::mutex restartMutex_;
    int inflight_ = 0;
    bool restarting_ = false;
    bool restartPending_ = false;

    // Lets a restart scheduled on the event loop notice that the addon is
    // already gone.
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
};

class BeastFactory : public AddonFactory {
public:
    AddonInstance *create(AddonManager *manager) override {
        registerDomain("fcitx5-beast", FCITX_INSTALL_LOCALEDIR);
        return new Beast(manager->instance());
    }
};
} // namespace fcitx

#endif // _FCITX5_MODULES_BEAST_BEAST_H_
