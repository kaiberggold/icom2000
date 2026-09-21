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
    ControlServer(std::string socketPath, core::EventLoop& loop);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Case-insensitive; last registration for a given name wins. Safe to
    // call before or after start().
    void registerCommand(std::string name, CommandHandler handler);

    // Creates, binds (removing a stale socket file first) and listens on
    // socketPath, and registers the listening fd with the EventLoop.
    void start();

private:
    void onListenReadable(short revents);
    void onClientReadable(int clientFd, short revents);
    void closeClient(int clientFd);
    void handleLine(int clientFd, std::string_view line);

    std::string socketPath_;
    core::EventLoop& loop_;
    int listenFd_ = -1;
    std::unordered_map<std::string, CommandHandler> handlers_;
    std::unordered_map<int, std::string> readBuffers_;
};

} // namespace icom::ipc
