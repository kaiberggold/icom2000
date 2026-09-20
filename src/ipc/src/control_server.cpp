#include "icom/ipc/control_server.hpp"
#include "icom/core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace icom::ipc {

namespace {

core::Logger& log = core::getLogger("ipc.control_server");

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

void throwErrno(std::string_view what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

} // namespace

ControlServer::ControlServer(std::string socketPath, core::EventLoop& loop)
    : socketPath_(std::move(socketPath)), loop_(loop) {}

ControlServer::~ControlServer() {
    if (listenFd_ >= 0) {
        loop_.removeFd(listenFd_);
        ::close(listenFd_);
        ::unlink(socketPath_.c_str());
    }
    for (auto& [fd, buf] : readBuffers_) {
        loop_.removeFd(fd);
        ::close(fd);
    }
}

void ControlServer::registerCommand(std::string name, CommandHandler handler) {
    handlers_[toUpper(std::move(name))] = std::move(handler);
}

void ControlServer::start() {
    listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listenFd_ < 0) {
        throwErrno("socket(AF_UNIX) failed");
    }

    // A stale socket file from an unclean previous shutdown must not block
    // bind() -- systemd normally handles this via RuntimeDirectory, but a
    // manual `intercomd` invocation during development won't have that.
    ::unlink(socketPath_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("control socket path too long: " + socketPath_);
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throwErrno("bind(" + socketPath_ + ") failed");
    }

    // Group-writable so a non-root operator in the `icom2000` group can run
    // intercomctl; systemd's RuntimeDirectoryMode covers the parent dir.
    ::chmod(socketPath_.c_str(), 0660);

    if (::listen(listenFd_, 4) != 0) {
        throwErrno("listen() failed");
    }

    loop_.addFd(listenFd_, POLLIN, [this](short revents) { onListenReadable(revents); });
    log.info("listening on " + socketPath_);
}

void ControlServer::onListenReadable(short /*revents*/) {
    for (;;) {
        const int clientFd = ::accept4(listenFd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (clientFd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log.warn(std::string("accept() failed: ") + std::strerror(errno));
            }
            return;
        }
        readBuffers_[clientFd] = {};
        loop_.addFd(clientFd, POLLIN, [this, clientFd](short revents) {
            onClientReadable(clientFd, revents);
        });
    }
}

void ControlServer::onClientReadable(int clientFd, short revents) {
    if (revents & (POLLHUP | POLLERR)) {
        closeClient(clientFd);
        return;
    }

    char buf[512];
    const ssize_t n = ::read(clientFd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        closeClient(clientFd);
        return;
    }
    if (n == 0) {
        closeClient(clientFd);
        return;
    }

    std::string& pending = readBuffers_[clientFd];
    pending.append(buf, static_cast<std::size_t>(n));

    std::size_t newline;
    while ((newline = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        handleLine(clientFd, line);
        // A command handler (e.g. a future "SHUTDOWN") could close/stop the
        // loop out from under us; bail rather than touch freed state.
        if (!readBuffers_.contains(clientFd)) {
            return;
        }
    }
}

void ControlServer::closeClient(int clientFd) {
    loop_.removeFd(clientFd);
    ::close(clientFd);
    readBuffers_.erase(clientFd);
}

void ControlServer::handleLine(int clientFd, std::string_view line) {
    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
        return;
    }

    const std::string command = toUpper(tokens.front());
    const std::vector<std::string> args(tokens.begin() + 1, tokens.end());

    CommandResult result;
    auto it = handlers_.find(command);
    if (it == handlers_.end()) {
        result = CommandResult::failure("unknown command: " + tokens.front());
    } else {
        try {
            result = it->second(args);
        } catch (const std::exception& e) {
            result = CommandResult::failure(std::string("handler threw: ") + e.what());
        }
    }

    const std::string response = formatResponse(result);
    // Best-effort write: on a local Unix socket with short responses this
    // will not partial-write in practice; a general-purpose server would
    // still buffer and retry on POLLOUT.
    (void)::write(clientFd, response.data(), response.size());
}

} // namespace icom::ipc
