#pragma once

#include "protocol.hpp"

#include <openssl/ssl.h>

#include <memory>
#include <mutex>
#include <string>

enum class RecvStatus {
    Ok,
    Timeout, // nothing arrived in time; the connection is still usable
    Closed,  // peer closed, I/O error, or a malformed frame; stop using it
};

// One TLS connection carrying framed messages (see net.hpp for opening one).
//
// Sending is thread-safe: send() holds the send lock for one message, and a
// multi-message sequence (a file transfer) holds lock_send() for its whole
// duration and uses send_locked(), so other senders can't interleave with it.
// Receiving is not: each connection must have a single reader at a time.
//
// OpenSSL doesn't allow two threads to use the same SSL object at once, so
// the socket is non-blocking and ssl_mutex_ is held only for the duration of
// a single SSL_read/SSL_write call, never while waiting for the network.
class Connection {
public:
    Connection(int fd, SSL* ssl, std::string peer, bool peer_is_server);
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
    // True if the peer proved it is a cluster server (a certificate signed by the CA).
    bool peer_is_server() const { return peer_is_server_; }

private:
    enum class IoStatus { Ok, Timeout, Closed };
    IoStatus read_some(char* data, std::size_t size, std::size_t& got, int timeout_ms);
    bool read_exact(char* data, std::size_t size);
    bool write_all(const char* data, std::size_t size);

    int fd_;
    SSL* ssl_;
    std::string peer_;
    bool peer_is_server_;
    std::mutex ssl_mutex_;
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
