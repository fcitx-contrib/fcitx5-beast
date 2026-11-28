#include "beast.h"
#include <boost/asio/generic/stream_protocol.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <fcitx-config/iniparser.h>

#include <chrono>

#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
// For unlink(2)
#include <unistd.h>
#endif

#define REMOTE_PREFIX "/remote/"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace fcitx {

// A restart waits for the requests in flight, so a client that stalls must not
// hold its connection, and with it the restart, forever.
constexpr std::chrono::seconds kRequestTimeout{5};

// Hooks are shared, not copied: a call has to run the callable the host
// installed, with its own state, and keep it while a newer one replaces it.
std::shared_ptr<const ConfigSetter> configSetter_ =
    std::make_shared<const ConfigSetter>([](const char *, const char *) {});
std::shared_ptr<const ConfigGetter> configGetter_ =
    std::make_shared<const ConfigGetter>([](const char *) { return ""; });
std::shared_ptr<const RemoteHandler> remoteHandler_ =
    std::make_shared<const RemoteHandler>(
        [](const std::string_view,
           const char *) -> std::pair<bool, std::string> {
            return {false, ""};
        });

namespace {

// The host installs the hooks after the addon exists, while the server thread
// is already serving.
std::mutex hooksMutex;

template <typename Hook>
std::shared_ptr<const Hook> useHook(const std::shared_ptr<const Hook> &hook) {
    std::lock_guard<std::mutex> lock(hooksMutex);
    return hook;
}

} // namespace

void Beast::setConfigGetter(ConfigGetter getter) {
    std::lock_guard<std::mutex> lock(hooksMutex);
    configGetter_ = std::make_shared<const ConfigGetter>(std::move(getter));
}

void Beast::setConfigSetter(ConfigSetter setter) {
    std::lock_guard<std::mutex> lock(hooksMutex);
    configSetter_ = std::make_shared<const ConfigSetter>(std::move(setter));
}

void Beast::setRemoteHandler(RemoteHandler handler) {
    std::lock_guard<std::mutex> lock(hooksMutex);
    remoteHandler_ = std::make_shared<const RemoteHandler>(std::move(handler));
}

Beast::Beast(Instance *instance) : instance_(instance) { reloadConfig(); }

Beast::~Beast() {
    alive_->store(false);
    stopThread();
    // Drop the handlers before the members they report back to are gone.
    ioc.reset();
}

void Beast::setConfig(const RawConfig &config) {
    config_.load(config);
    safeSaveAsIni(config_, ConfPath);
    reloadConfig();
}

void Beast::reloadConfig() {
    readAsIni(config_, ConfPath);
    restartWhenIdle();
}

bool Beast::requestStarted() {
    std::lock_guard<std::mutex> lock(restartMutex_);
    if (restarting_) {
        return false;
    }
    ++inflight_;
    return true;
}

void Beast::requestFinished() {
    bool restart;
    {
        std::lock_guard<std::mutex> lock(restartMutex_);
        restart = (--inflight_ == 0) && restartPending_;
    }

    // The addon may be going away, and the connections dropped with its
    // io_context end up here.
    if (!restart || !alive_->load()) {
        return;
    }

    // The server thread cannot restart itself; the fcitx loop can, once this
    // response is out.
    auto alive = alive_;
    instance_->eventDispatcher().schedule([this, alive] {
        if (alive->load()) {
            restartIfPending();
        }
    });
}

void Beast::restartIfPending() {
    {
        std::lock_guard<std::mutex> lock(restartMutex_);
        // A reload may have restarted meanwhile, and a second restart would
        // close admission on a listener that is already current.
        if (!restartPending_) {
            return;
        }
        restartPending_ = false;
    }

    restartWhenIdle();
}

void Beast::restartWhenIdle() {
    {
        std::lock_guard<std::mutex> lock(restartMutex_);
        // Admission stays closed for the whole window, or new requests would
        // keep inflight_ above zero and postpone the restart indefinitely.
        restarting_ = true;
        if (inflight_ > 0) {
            // The handler is on the thread we would join, most likely waiting
            // for this thread to apply the config it sent.
            restartPending_ = true;
            return;
        }
        restartPending_ = false;
    }

    restartServer();

    std::lock_guard<std::mutex> lock(restartMutex_);
    restarting_ = false;
}

void Beast::restartServer() {
    if (serverThread_.joinable()) {
        stopThread();
    }
    startThread();
}

template <class Protocol>
class http_connection
    : public std::enable_shared_from_this<http_connection<Protocol>> {
public:
    using Stream = beast::basic_stream<Protocol>;

    http_connection(typename Protocol::socket socket, Beast *owner)
        : stream_(std::move(socket)), owner_(owner),
          accepted_(owner->requestStarted()) {}

    ~http_connection() {
        if (accepted_) {
            owner_->requestFinished();
        }
    }

    // Initiate the asynchronous operations associated with the connection.
    void start() {
        if (!accepted_) {
            refuse();
            return;
        }
        read_request();
    }

private:
    // The socket for the currently connected client. Each operation is on a
    // deadline so that a stalled client cannot hold the connection open.
    Stream stream_;

    // The addon that counts this connection as an in-flight request.
    Beast *owner_;
    const bool accepted_;

    // The buffer for performing reads.
    beast::flat_buffer buffer_{8192};

    // The request message.
    http::request<http::string_body> request_;

    // The response message.
    http::response<http::dynamic_body> response_;

    // Serving a connection that arrived while the server is restarting would
    // have its socket closed mid-response.
    void refuse() {
        response_.version(11);
        response_.result(http::status::service_unavailable);
        response_.set(http::field::content_type, "text/plain");
        response_.keep_alive(false);
        beast::ostream(response_.body()) << "Beast is restarting\r\n";
        write_response();
    }

    // Asynchronously receive a complete request message.
    void read_request() {
        auto self = this->shared_from_this();

        stream_.expires_after(kRequestTimeout);
        http::async_read(
            stream_, buffer_, request_,
            [self](beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (!ec)
                    self->process_request();
            });
    }

    // Determine what needs to be done with the request message.
    void process_request() {
        response_.version(request_.version());
        response_.keep_alive(false);

        switch (request_.method()) {
        case http::verb::get:
        case http::verb::post:
            response_.result(http::status::ok);
            response_.set(http::field::server, "Beast");
            try {
                create_response();
            } catch (const std::exception &e) {
                response_.result(http::status::internal_server_error);
                response_.set(http::field::content_type, "text/plain");
                beast::ostream(response_.body())
                    << "An error occurred: " << e.what();
            }
            break;

        default:
            // We return responses indicating an error if
            // we do not recognize the request method.
            response_.result(http::status::bad_request);
            response_.set(http::field::content_type, "text/plain");
            beast::ostream(response_.body())
                << "Invalid request-method '"
                << std::string(request_.method_string()) << "'";
            break;
        }

        write_response();
    }

    // Construct a response message based on the program state.
    void create_response() {
        if (request_.target().starts_with("/config/")) {
            std::string uri = "fcitx:/";
            uri += request_.target();
            if (request_.method() == http::verb::get) {
                response_.result(http::status::ok);
                response_.set(http::field::content_type, "application/json");
                beast::ostream(response_.body())
                    << (*useHook(configGetter_))(uri.c_str());
            } else if (request_.method() == http::verb::post) {
                response_.result(http::status::ok);
                response_.set(http::field::content_type, "text/plain");
                beast::ostream(response_.body()) << "";
                (*useHook(configSetter_))(uri.c_str(), request_.body().data());
            }
        } else if (request_.target().starts_with(REMOTE_PREFIX) &&
                   request_.method() == http::verb::post) {
            std::string_view command = request_.target();
            stringutils::consumePrefix(command, REMOTE_PREFIX);
            auto handler = useHook(remoteHandler_);
            auto res = (*handler)(command, request_.body().data());
            response_.set(http::field::content_type, "text/plain");
            response_.result(res.first ? http::status::ok
                                       : http::status::bad_request);
            beast::ostream(response_.body()) << res.second;
        } else {
            response_.result(http::status::not_found);
            response_.set(http::field::content_type, "text/plain");
            beast::ostream(response_.body()) << "File not found\r\n";
        }
    }

    // Asynchronously transmit the response message.
    void write_response() {
        auto self = this->shared_from_this();

        response_.content_length(response_.body().size());

        // Re-armed for the write: a handler can be slow, and a read deadline
        // left over would fail the response.
        stream_.expires_after(kRequestTimeout);
        http::async_write(stream_, response_,
                          [self](beast::error_code ec, std::size_t) {
                              self->stream_.socket().shutdown(
                                  Protocol::socket::shutdown_send, ec);
                          });
        FCITX_INFO() << response_.result_int() << " "
                     << request_.method_string() << " " << request_.target();
    }
};

// "Loop" forever accepting new connections.
template <class Acceptor>
void http_server(Acceptor &acceptor,
                 typename Acceptor::protocol_type::socket &socket,
                 Beast *owner) {
    acceptor.async_accept(socket, [&acceptor, &socket,
                                   owner](beast::error_code ec) {
        if (!ec)
            std::make_shared<http_connection<typename Acceptor::protocol_type>>(
                std::move(socket), owner)
                ->start();
        http_server(acceptor, socket, owner);
    });
}

void Beast::startServer(const std::shared_ptr<asio::io_context> &context,
                        BeastCommunication communication,
                        const std::string &socketPath, int port) {
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
    if (communication == BeastCommunication::UnixSocket) {
        (void)::unlink(socketPath.c_str());
        auto const ep = asio::local::stream_protocol::endpoint{socketPath};
        asio::local::stream_protocol::acceptor acceptor{*context, ep};
        asio::local::stream_protocol::socket socket{*context};
        http_server(acceptor, socket, this);
        context->run();
    } else {
#endif
        auto const address = asio::ip::make_address("127.0.0.1");
        tcp::acceptor acceptor{*context, {address, (unsigned short)port}};
        tcp::socket socket{*context};
        http_server(acceptor, socket, this);
        context->run();
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
    }
#endif
}

void Beast::startThread() {
    // Read on the fcitx thread, so the server never touches config_ while
    // setConfig() rewrites it.
    const auto communication = config_.communication.value();
#ifdef FCITX5_BEAST_HAS_UNIX_SOCKET
    const auto socketPath = communication == BeastCommunication::UnixSocket
                                ? config_.unix_socket.value().path.value()
                                : std::string();
#else
    const auto socketPath = std::string();
#endif
    const auto port = config_.tcp.value().port.value();

    ioc = std::make_shared<asio::io_context>();
    auto context = ioc;
    serverThread_ =
        std::thread([this, context, communication, socketPath, port] {
            try {
                startServer(context, communication, socketPath, port);
            } catch (const std::exception &e) {
                FCITX_ERROR() << "Error in Beast: " << e.what();
            }
        });
}

void Beast::stopThread() {
    if (ioc) {
        ioc->stop();
    }
    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}
} // namespace fcitx

FCITX_ADDON_FACTORY_V2(beast, fcitx::BeastFactory)
