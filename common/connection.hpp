#pragma once

#include "protocol.hpp"

#include <memory>
#include <mutex>
#include <string>

enum class RecvStatus {
    Ok,
    Timeout, // nothing arrived in time; the connection is still usable
    Closed,  // peer closed, I/O error, or a malformed frame; stop using it
};

// One TCP connection carrying framed messages.
//
// Sending is thread-safe: send() holds the send lock for one message, and a
// multi-message sequence (a file transfer) holds lock_send() for its whole
// duration and uses send_locked(), so other senders can't interleave with it.
// Receiving is not: each connection must have a single reader at a time.
class Connection {
public:
    Connection(int fd, std::string peer);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool send(Msg type, const std::string& payload = std::string());
    std::unique_lock<std::mutex> lock_send();
    bool send_locked(Msg type, const std::string& payload = std::string());

    // Waits up to timeout_ms for a message (negative: forever). Once a message
    // has started arriving, the rest of it must follow within a few seconds.
    RecvStatus recv(Message& out, int timeout_ms = -1);

    // Unblocks any thread waiting on this connection; later calls fail.
    void shutdown();

    const std::string& peer() const { return peer_; }

private:
    bool write_all(const char* data, std::size_t size);
    bool read_exact(char* data, std::size_t size);

    int fd_;
    std::string peer_;
    std::mutex send_mutex_;
};

using ConnectionPtr = std::shared_ptr<Connection>;

enum class CallResult {
    Ok,       // the reply has the expected type
    Rejected, // the peer answered with something else (usually Error); still usable
    Failed,   // send failed, timed out or the connection closed
};

// Sends a request and waits for its reply, which is stored in `reply`.
CallResult call(Connection& conn, Msg request, const std::string& payload, Msg expected,
                Message& reply, int timeout_ms);

// The reason carried by an Error reply, or a generic text for anything else.
std::string error_reason(const Message& reply);
bool send_error(Connection& conn, const std::string& reason);
