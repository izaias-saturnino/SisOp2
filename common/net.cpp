#include "net.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string peer_name(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

int connect_with_timeout(const sockaddr_in& addr, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        rc = poll(&pfd, 1, timeout_ms);
        if (rc == 1) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            rc = error == 0 ? 0 : -1;
        } else {
            rc = -1;
        }
    }
    if (rc != 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    return fd;
}

} // namespace

ConnectionPtr connect_to(const std::string& host, uint16_t port, int timeout_ms)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &results) != 0) {
        return nullptr;
    }

    ConnectionPtr conn;
    for (addrinfo* ai = results; ai != nullptr && !conn; ai = ai->ai_next) {
        const sockaddr_in& addr = *reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
        int fd = connect_with_timeout(addr, timeout_ms);
        if (fd >= 0) {
            conn = std::make_shared<Connection>(fd, host + ":" + service);
        }
    }
    freeaddrinfo(results);
    return conn;
}

int listen_on(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd, 64) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

ConnectionPtr accept_from(int listen_fd)
{
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_CLOEXEC);
        if (fd >= 0) {
            return std::make_shared<Connection>(fd, peer_name(addr));
        }
        // Transient errors (aborted handshakes, signals) shouldn't stop the server.
        if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
            continue;
        }
        return nullptr;
    }
}

bool parse_port(const std::string& text, uint16_t& port)
{
    if (text.empty() || text.size() > 5 || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    unsigned long value = std::stoul(text);
    if (value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool parse_endpoint(const std::string& text, std::string& host, uint16_t& port)
{
    std::size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    host = text.substr(0, colon);
    return parse_port(text.substr(colon + 1), port);
}
