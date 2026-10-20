#pragma once

#include "connection.hpp"

#include <cstdint>
#include <string>

// Resolves `host` (a name or a dotted IPv4 address) and connects, giving up
// after timeout_ms. Returns nullptr on failure.
ConnectionPtr connect_to(const std::string& host, uint16_t port, int timeout_ms);

// Listens on every interface. Sets SO_REUSEADDR so a restarted server can bind
// again right away. Returns the socket, or -1 (with errno set) on failure.
int listen_on(uint16_t port);

// Blocks until a client connects. Returns nullptr on error.
ConnectionPtr accept_from(int listen_fd);

bool parse_port(const std::string& text, uint16_t& port);
bool parse_endpoint(const std::string& text, std::string& host, uint16_t& port); // "host:port"
