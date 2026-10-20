#pragma once

#include "../common/connection.hpp"
#include "cluster.hpp"
#include "sessions.hpp"
#include "storage.hpp"

#include <cstdint>
#include <mutex>
#include <string>

struct ServerOptions {
    std::string host = "127.0.0.1"; // address other servers and clients use to reach this one
    uint16_t port = 4000;
    std::string data_dir;           // default: server_data_<port>
    std::string join_host;          // empty: start a new cluster
    uint16_t join_port = 0;
};

// Accepts connections and serves them, one thread per connection. The first
// message on a connection says what it is: a client login, a client's sync
// connection, or a message from another server.
class Server {
public:
    explicit Server(ServerOptions options);
    int run();

private:
    void handle_connection(ConnectionPtr conn);

    void serve_client(const ConnectionPtr& conn, const Message& login);
    void serve_sync(const ConnectionPtr& conn, const Message& attach);
    // Returns false when the session should end.
    bool handle_request(const ConnectionPtr& conn, const std::string& user, uint64_t token,
                        const Message& msg);
    bool handle_list(Connection& conn, const std::string& user);
    bool handle_upload(Connection& conn, const std::string& user, uint64_t token, const Message& msg);
    bool handle_download(Connection& conn, const std::string& user, const Message& msg);
    bool handle_delete(Connection& conn, const std::string& user, uint64_t token, const Message& msg);

    // Send a change to the user's sync connections, skipping session `except`.
    void push_file(const std::string& user, const std::string& name, uint64_t except);
    void push_delete(const std::string& user, const std::string& name, uint64_t except);
    void push_server_list(const std::vector<ServerAddress>& servers);
    void send_redirect(Connection& conn);

    ServerOptions options_;
    Storage storage_;
    std::mutex write_mutex_; // serializes changes to storage and their replication
    SessionManager sessions_;
    Cluster cluster_;
};
