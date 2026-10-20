#include "connection.hpp"

#include <openssl/err.h>

#include <cerrno>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// Once a frame has started, the rest of it must arrive within this time.
constexpr int FRAME_TIMEOUT_MS = 15000;
// A send that can't make progress for this long means the peer is gone.
constexpr int SEND_TIMEOUT_MS = 15000;

using Clock = std::chrono::steady_clock;

// Milliseconds left until `deadline`, or -1 for "no deadline".
int remaining_ms(bool has_deadline, Clock::time_point deadline)
{
    if (!has_deadline) {
        return -1;
    }
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// Waits until the socket is ready for `events`. False on timeout or error.
bool wait_for(int fd, short events, int timeout_ms)
{
    pollfd pfd{fd, events, 0};
    while (true) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) return true;
        if (rc == 0) return false;
        if (errno != EINTR) return false;
    }
}

} // namespace

Connection::Connection(int fd, SSL* ssl, std::string peer, bool peer_is_server)
    : fd_(fd), ssl_(ssl), peer_(std::move(peer)), peer_is_server_(peer_is_server)
{
}

Connection::~Connection()
{
    SSL_free(ssl_);
    close(fd_);
}

std::unique_lock<std::mutex> Connection::lock_send()
{
    return std::unique_lock<std::mutex>(send_mutex_);
}

bool Connection::send(Msg type, const std::string& payload)
{
    std::lock_guard<std::mutex> lock(send_mutex_);
    return send_locked(type, payload);
}

bool Connection::send_locked(Msg type, const std::string& payload)
{
    if (payload.size() > MAX_PAYLOAD) {
        return false;
    }
    Writer header;
    header.u16(static_cast<uint16_t>(type)).u16(0).u32(static_cast<uint32_t>(payload.size()));
    // One buffer so small messages go out in a single TLS record.
    std::string frame = header.data();
    frame += payload;
    return write_all(frame.data(), frame.size());
}

bool Connection::write_all(const char* data, std::size_t size)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(SEND_TIMEOUT_MS);
    std::size_t sent = 0;
    while (sent < size) {
        int written;
        int error;
        {
            std::lock_guard<std::mutex> lock(ssl_mutex_);
            ERR_clear_error();
            written = SSL_write(ssl_, data + sent, static_cast<int>(size - sent));
            error = written > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, written);
        }
        if (written > 0) {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        short events;
        if (error == SSL_ERROR_WANT_WRITE) events = POLLOUT;
        else if (error == SSL_ERROR_WANT_READ) events = POLLIN;
        else return false;
        if (!wait_for(fd_, events, remaining_ms(true, deadline))) {
            return false;
        }
    }
    return true;
}

Connection::IoStatus Connection::read_some(char* data, std::size_t size, std::size_t& got,
                                           int timeout_ms)
{
    bool has_deadline = timeout_ms >= 0;
    auto deadline = Clock::now() + std::chrono::milliseconds(has_deadline ? timeout_ms : 0);
    while (true) {
        int n;
        int error;
        {
            std::lock_guard<std::mutex> lock(ssl_mutex_);
            ERR_clear_error();
            n = SSL_read(ssl_, data, static_cast<int>(size));
            error = n > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, n);
        }
        if (n > 0) {
            got = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        short events;
        if (error == SSL_ERROR_WANT_READ) events = POLLIN;
        else if (error == SSL_ERROR_WANT_WRITE) events = POLLOUT;
        else return IoStatus::Closed; // closed by the peer, or a TLS/socket error
        if (!wait_for(fd_, events, remaining_ms(has_deadline, deadline))) {
            return has_deadline ? IoStatus::Timeout : IoStatus::Closed;
        }
    }
}

bool Connection::read_exact(char* data, std::size_t size)
{
    std::size_t total = 0;
    while (total < size) {
        std::size_t got = 0;
        if (read_some(data + total, size - total, got, FRAME_TIMEOUT_MS) != IoStatus::Ok) {
            return false;
        }
        total += got;
    }
    return true;
}

RecvStatus Connection::recv(Message& out, int timeout_ms)
{
    char header[HEADER_SIZE];
    std::size_t got = 0;
    IoStatus first = read_some(header, HEADER_SIZE, got, timeout_ms);
    if (first == IoStatus::Timeout) {
        return RecvStatus::Timeout;
    }
    if (first == IoStatus::Closed || !read_exact(header + got, HEADER_SIZE - got)) {
        return RecvStatus::Closed;
    }

    std::string header_bytes(header, HEADER_SIZE);
    Reader r(header_bytes);
    out.type = static_cast<Msg>(r.u16());
    r.u16(); // reserved
    uint32_t length = r.u32();
    if (length > MAX_PAYLOAD) {
        return RecvStatus::Closed;
    }
    out.payload.assign(length, '\0');
    if (length > 0 && !read_exact(&out.payload[0], length)) {
        return RecvStatus::Closed;
    }
    return RecvStatus::Ok;
}

void Connection::shutdown()
{
    ::shutdown(fd_, SHUT_RDWR);
}

CallResult call(Connection& conn, Msg request, const std::string& payload, Msg expected,
                Message& reply, int timeout_ms)
{
    if (!conn.send(request, payload) || conn.recv(reply, timeout_ms) != RecvStatus::Ok) {
        return CallResult::Failed;
    }
    return reply.type == expected ? CallResult::Ok : CallResult::Rejected;
}

std::string error_reason(const Message& reply)
{
    if (reply.type == Msg::Error) {
        try {
            Reader r(reply.payload);
            return r.str();
        } catch (const ProtocolError&) {
        }
    }
    return std::string("unexpected reply ") + msg_name(reply.type);
}

bool send_error(Connection& conn, const std::string& reason)
{
    Writer w;
    w.str(reason);
    return conn.send(Msg::Error, w.data());
}
