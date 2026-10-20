#pragma once

#include <openssl/ssl.h>

#include <string>

// Every connection is TLS. The certificates come from a private certificate
// authority made for the cluster (tools/make-certs.sh):
//
//  - Servers present a certificate signed by that CA. Clients verify it,
//    including that the host name they connected to is listed in it.
//  - Servers also present their certificate when they connect to each other
//    (mutual TLS). That's how a server tells another server from a client:
//    only connections with a valid certificate may send cluster messages.
//  - Clients have no certificate; they authenticate with a password.

struct TlsConfig {
    std::string ca_file;   // CA used to verify peers
    std::string cert_file; // servers only: their certificate...
    std::string key_file;  // ...and its private key
};

// Loads the certificates. Call once at startup, before any connection.
bool tls_init(const TlsConfig& config, std::string& error);

SSL_CTX* tls_client_context(); // for connections this process opens
SSL_CTX* tls_server_context(); // for connections it accepts (servers only)

// The last OpenSSL error, as text.
std::string tls_error();
