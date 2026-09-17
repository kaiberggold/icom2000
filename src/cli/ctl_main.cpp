// intercomctl: talks the line protocol in icom/ipc/protocol.hpp to a
// running intercomd over its Unix domain socket. Deliberately just a thin
// socket client -- all the logic lives in the daemon, so this stays usable
// from a shell script (or `socat -` / `nc -U`, for that matter) without
// needing to track daemon-internal changes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

constexpr const char* kDefaultSocketPath = "/run/icom2000.sock";

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [-s SOCKET] COMMAND [ARG ...]\n"
              << "  -s SOCKET   control socket path (default: " << kDefaultSocketPath
              << ", overridable via ICOM2000_SOCKET)\n"
              << "examples:\n"
              << "  " << argv0 << " bell ring\n"
              << "  " << argv0 << " line status\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string socket_path = kDefaultSocketPath;
    if (const char* env = std::getenv("ICOM2000_SOCKET")) {
        socket_path = env;
    }

    int arg_index = 1;
    if (argc >= 3 && std::strcmp(argv[1], "-s") == 0) {
        socket_path = argv[2];
        arg_index = 3;
    }

    if (arg_index >= argc) {
        print_usage(argv[0]);
        return 2;
    }

    std::string request;
    for (int i = arg_index; i < argc; ++i) {
        if (i > arg_index) {
            request += ' ';
        }
        request += argv[i];
    }
    request += '\n';

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return 2;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::perror(("connect: " + socket_path).c_str());
        ::close(fd);
        return 2;
    }

    if (::write(fd, request.data(), request.size()) < 0) {
        std::perror("write");
        ::close(fd);
        return 2;
    }

    std::string response;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        response.append(buf, static_cast<std::size_t>(n));
        if (response.find('\n') != std::string::npos) {
            break;
        }
    }
    ::close(fd);

    std::cout << response;
    if (response.empty()) {
        std::cerr << "intercomctl: no response from daemon\n";
        return 2;
    }
    return response.rfind("OK", 0) == 0 ? 0 : 1;
}
