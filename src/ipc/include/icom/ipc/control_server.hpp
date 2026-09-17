#pragma once

#include "icom/core/event_loop.hpp"
#include "icom/ipc/protocol.hpp"

#include <string>
#include <unordered_map>

namespace icom::ipc {

// The "controlable from bash" half of the daemon: a Unix domain socket
// (SOCK_STREAM) that intercomctl, or any shell via `socat -`, can talk the
// line protocol in protocol.hpp to. Commands are registered by name rather
// than switched-on in this class, so adding e.g. "LINE STATUS" later means
// registering a handler at daemon wiring time (src/app/daemon_main.cpp),
// not touching this file.
class ControlServer {
public:
    ControlServer(std::string socket_path, core::EventLoop& loop);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Case-insensitive; last registration for a given name wins. Safe to
    // call before or after start().
    void register_command(std::string name, CommandHandler handler);

    // Creates, binds (removing a stale socket file first) and listens on
    // socket_path, and registers the listening fd with the EventLoop.
    void start();

private:
    void on_listen_readable(short revents);
    void on_client_readable(int client_fd, short revents);
    void close_client(int client_fd);
    void handle_line(int client_fd, std::string_view line);

    std::string socket_path_;
    core::EventLoop& loop_;
    int listen_fd_ = -1;
    std::unordered_map<std::string, CommandHandler> handlers_;
    std::unordered_map<int, std::string> read_buffers_;
};

} // namespace icom::ipc
