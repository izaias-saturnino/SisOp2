#include "net.hpp"

#include "log.hpp"
#include "tls.hpp"

#include <openssl/err.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

std::string peer_name(const sockaddr_in& addr)
{
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// Returns a connected, non-blocking socket, or -1.
int connect_with_timeout(const sockaddr_in& addr, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        return -1;
    }
    int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        rc = poll(&pfd, 1, timeout_ms) == 1 ? 0 : -1;
        if (rc == 0) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
            rc = error == 0 ? 0 : -1;
        }
    }
    if (rc != 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

// Drives SSL_connect/SSL_accept on a non-blocking socket until done.
bool handshake(SSL* ssl, int fd, bool is_client, int timeout_ms)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        ERR_clear_error();
        int rc = is_client ? SSL_connect(ssl) : SSL_accept(ssl);
        if (rc == 1) {
            return true;
        }
        int error = SSL_get_error(ssl, rc);
        short events;
        if (error == SSL_ERROR_WANT_READ) events = POLLIN;
        else if (error == SSL_ERROR_WANT_WRITE) events = POLLOUT;
        else return false;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd pfd{fd, events, 0};
        if (left <= 0 || poll(&pfd, 1, static_cast<int>(left)) <= 0) {
            return false;
        }
    }
}

bool is_ipv4(const std::string& host)
{
    in_addr ignored{};
    return inet_pton(AF_INET, host.c_str(), &ignored) == 1;
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
    int fd = -1;
    for (addrinfo* ai = results; ai != nullptr && fd < 0; ai = ai->ai_next) {
        fd = connect_with_timeout(*reinterpret_cast<const sockaddr_in*>(ai->ai_addr), timeout_ms);
    }
    freeaddrinfo(results);
    if (fd < 0) {
        return nullptr;
    }

    SSL* ssl = SSL_new(tls_client_context());
    SSL_set_fd(ssl, fd);
    // The certificate must name the host we meant to reach.
    if (is_ipv4(host)) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
    } else {
        SSL_set_tlsext_host_name(ssl, host.c_str());
        SSL_set1_host(ssl, host.c_str());
    }
    if (!handshake(ssl, fd, true, timeout_ms + 5000)) {
        long verify = SSL_get_verify_result(ssl);
        if (verify != X509_V_OK) {
            LOG_WARN("could not verify the certificate of " << host << ":" << port << ": "
                                                            << X509_verify_cert_error_string(verify));
        }
        SSL_free(ssl);
        close(fd);
        return nullptr;
    }
    return std::make_shared<Connection>(fd, ssl, host + ":" + service, true);
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

int accept_socket(int listen_fd, std::string& peer)
{
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        int fd = accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_CLOEXEC);
        if (fd >= 0) {
            peer = peer_name(addr);
            set_nonblocking(fd);
            return fd;
        }
        // Transient errors (aborted handshakes, signals) shouldn't stop the server.
        if (errno == EINTR || errno == ECONNABORTED || errno == EPROTO) {
            continue;
        }
        return -1;
    }
}

ConnectionPtr tls_accept(int fd, const std::string& peer, int timeout_ms)
{
    SSL* ssl = SSL_new(tls_server_context());
    SSL_set_fd(ssl, fd);
    if (!handshake(ssl, fd, false, timeout_ms)) {
        LOG_DEBUG("TLS handshake with " << peer << " failed: " << tls_error());
        SSL_free(ssl);
        close(fd);
        return nullptr;
    }
    // A certificate that failed verification would have failed the handshake,
    // so any certificate here was signed by the cluster CA.
    X509* cert = SSL_get1_peer_certificate(ssl);
    bool is_server = cert != nullptr && SSL_get_verify_result(ssl) == X509_V_OK;
    X509_free(cert);
    return std::make_shared<Connection>(fd, ssl, peer, is_server);
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
