#pragma once

#include "connection.hpp"

#include <cstdint>
#include <string>

// Resolves `host` (a name or an IPv4 address), connects and completes the TLS
// handshake, giving up after timeout_ms. The server's certificate must be
// signed by the cluster CA and list `host`. Returns nullptr on failure.
ConnectionPtr connect_to(const std::string& host, uint16_t port, int timeout_ms);

// Listens on every interface. Sets SO_REUSEADDR so a restarted server can bind
// again right away. Returns the socket, or -1 (with errno set) on failure.
int listen_on(uint16_t port);

// Blocks until a client connects; returns the new socket (or -1) and fills
// `peer` with its address. The TLS handshake is done later by tls_accept, in
// the connection's own thread, so a slow client can't stall the accept loop.
int accept_socket(int listen_fd, std::string& peer);
ConnectionPtr tls_accept(int fd, const std::string& peer, int timeout_ms);

bool parse_port(const std::string& text, uint16_t& port);
bool parse_endpoint(const std::string& text, std::string& host, uint16_t& port); // "host:port"
