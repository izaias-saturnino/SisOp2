#include "tls.hpp"

#include <openssl/err.h>

#include <csignal>

namespace {

SSL_CTX* client_ctx = nullptr;
SSL_CTX* server_ctx = nullptr;

bool configure_common(SSL_CTX* ctx, const TlsConfig& config, std::string& error)
{
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    // Connections use non-blocking sockets and retry partial writes.
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    if (SSL_CTX_load_verify_locations(ctx, config.ca_file.c_str(), nullptr) != 1) {
        error = "cannot read the CA certificate " + config.ca_file + ": " + tls_error();
        return false;
    }
    if (!config.cert_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_file.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, config.key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            error = "cannot load the certificate " + config.cert_file + " and key " + config.key_file +
                    ": " + tls_error();
            return false;
        }
    }
    return true;
}

} // namespace

bool tls_init(const TlsConfig& config, std::string& error)
{
    // SSL_write can't be told MSG_NOSIGNAL, so a write to a closed socket
    // would raise SIGPIPE; ignore it and let the write fail instead.
    std::signal(SIGPIPE, SIG_IGN);

    client_ctx = SSL_CTX_new(TLS_client_method());
    if (client_ctx == nullptr || !configure_common(client_ctx, config, error)) {
        return false;
    }
    SSL_CTX_set_verify(client_ctx, SSL_VERIFY_PEER, nullptr);

    if (config.cert_file.empty()) {
        return true; // a client: it never accepts connections
    }
    server_ctx = SSL_CTX_new(TLS_server_method());
    if (server_ctx == nullptr || !configure_common(server_ctx, config, error)) {
        return false;
    }
    // Ask for a certificate but don't require one: other servers send
    // theirs, clients don't. An invalid certificate still fails the handshake.
    SSL_CTX_set_verify(server_ctx, SSL_VERIFY_PEER, nullptr);
    if (STACK_OF(X509_NAME)* names = SSL_load_client_CA_file(config.ca_file.c_str())) {
        SSL_CTX_set_client_CA_list(server_ctx, names);
    }
    SSL_CTX_set_num_tickets(server_ctx, 0); // no session resumption needed
    return true;
}

SSL_CTX* tls_client_context()
{
    return client_ctx;
}

SSL_CTX* tls_server_context()
{
    return server_ctx;
}

std::string tls_error()
{
    unsigned long code = ERR_get_error();
    if (code == 0) {
        return "unknown TLS error";
    }
    char text[256];
    ERR_error_string_n(code, text, sizeof(text));
    ERR_clear_error();
    return text;
}
