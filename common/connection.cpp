#include "connection.hpp"

#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

// Once a frame has started, the rest of it must arrive within this time.
constexpr int FRAME_TIMEOUT_MS = 15000;
// A send that can't make progress for this long means the peer is gone.
constexpr int SEND_TIMEOUT_S = 15;

bool wait_readable(int fd, int timeout_ms)
{
    pollfd pfd{fd, POLLIN, 0};
    while (true) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) return true;
        if (rc == 0) return false;
        if (errno != EINTR) return false;
    }
}

} // namespace

Connection::Connection(int fd, std::string peer) : fd_(fd), peer_(std::move(peer))
{
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    timeval send_timeout{SEND_TIMEOUT_S, 0};
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
}

Connection::~Connection()
{
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
    // One buffer so small messages go out in a single segment.
    std::string frame = header.data();
    frame += payload;
    return write_all(frame.data(), frame.size());
}

bool Connection::write_all(const char* data, std::size_t size)
{
    std::size_t sent = 0;
    while (sent < size) {
        // MSG_NOSIGNAL: a dead peer gives EPIPE instead of killing the process.
        ssize_t n = ::send(fd_, data + sent, size - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool Connection::read_exact(char* data, std::size_t size)
{
    std::size_t got = 0;
    while (got < size) {
        if (!wait_readable(fd_, FRAME_TIMEOUT_MS)) {
            return false;
        }
        ssize_t n = ::recv(fd_, data + got, size - got, 0);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false; // 0 = peer closed
        }
    }
    return true;
}

RecvStatus Connection::recv(Message& out, int timeout_ms)
{
    if (!wait_readable(fd_, timeout_ms)) {
        // poll also reports readable on close/error, so a false here really
        // is a timeout unless poll itself failed.
        return timeout_ms < 0 ? RecvStatus::Closed : RecvStatus::Timeout;
    }

    std::string header(HEADER_SIZE, '\0');
    if (!read_exact(&header[0], HEADER_SIZE)) {
        return RecvStatus::Closed;
    }
    Reader r(header);
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
