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

core::Logger& kLog = core::get_logger("ipc.control_server");

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

void throw_errno(std::string_view what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}

} // namespace

ControlServer::ControlServer(std::string socket_path, core::EventLoop& loop)
    : socket_path_(std::move(socket_path)), loop_(loop) {}

ControlServer::~ControlServer() {
    if (listen_fd_ >= 0) {
        loop_.remove_fd(listen_fd_);
        ::close(listen_fd_);
        ::unlink(socket_path_.c_str());
    }
    for (auto& [fd, buf] : read_buffers_) {
        loop_.remove_fd(fd);
        ::close(fd);
    }
}

void ControlServer::register_command(std::string name, CommandHandler handler) {
    handlers_[to_upper(std::move(name))] = std::move(handler);
}

void ControlServer::start() {
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listen_fd_ < 0) {
        throw_errno("socket(AF_UNIX) failed");
    }

    // A stale socket file from an unclean previous shutdown must not block
    // bind() -- systemd normally handles this via RuntimeDirectory, but a
    // manual `intercomd` invocation during development won't have that.
    ::unlink(socket_path_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("control socket path too long: " + socket_path_);
    }
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw_errno("bind(" + socket_path_ + ") failed");
    }

    // Group-writable so a non-root operator in the `icom2000` group can run
    // intercomctl; systemd's RuntimeDirectoryMode covers the parent dir.
    ::chmod(socket_path_.c_str(), 0660);

    if (::listen(listen_fd_, 4) != 0) {
        throw_errno("listen() failed");
    }

    loop_.add_fd(listen_fd_, POLLIN, [this](short revents) { on_listen_readable(revents); });
    kLog.info("listening on " + socket_path_);
}

void ControlServer::on_listen_readable(short /*revents*/) {
    for (;;) {
        const int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                kLog.warn(std::string("accept() failed: ") + std::strerror(errno));
            }
            return;
        }
        read_buffers_[client_fd] = {};
        loop_.add_fd(client_fd, POLLIN, [this, client_fd](short revents) {
            on_client_readable(client_fd, revents);
        });
    }
}

void ControlServer::on_client_readable(int client_fd, short revents) {
    if (revents & (POLLHUP | POLLERR)) {
        close_client(client_fd);
        return;
    }

    char buf[512];
    const ssize_t n = ::read(client_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_client(client_fd);
        return;
    }
    if (n == 0) {
        close_client(client_fd);
        return;
    }

    std::string& pending = read_buffers_[client_fd];
    pending.append(buf, static_cast<std::size_t>(n));

    std::size_t newline;
    while ((newline = pending.find('\n')) != std::string::npos) {
        const std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        handle_line(client_fd, line);
        // A command handler (e.g. a future "SHUTDOWN") could close/stop the
        // loop out from under us; bail rather than touch freed state.
        if (!read_buffers_.contains(client_fd)) {
            return;
        }
    }
}

void ControlServer::close_client(int client_fd) {
    loop_.remove_fd(client_fd);
    ::close(client_fd);
    read_buffers_.erase(client_fd);
}

void ControlServer::handle_line(int client_fd, std::string_view line) {
    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
        return;
    }

    const std::string command = to_upper(tokens.front());
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

    const std::string response = format_response(result);
    // Best-effort write: on a local Unix socket with short responses this
    // will not partial-write in practice; a general-purpose server would
    // still buffer and retry on POLLOUT.
    (void)::write(client_fd, response.data(), response.size());
}

} // namespace icom::ipc
