#include "server.hpp"

#include "../common/log.hpp"
#include "../common/net.hpp"

#include <cstring>
#include <thread>

namespace {

constexpr int FIRST_MESSAGE_TIMEOUT_MS = 10000;
// Clients ping every second; this much silence means the client is gone.
constexpr int CLIENT_IDLE_TIMEOUT_MS = 30000;

std::string short_token(uint64_t token)
{
    char text[8];
    std::snprintf(text, sizeof(text), "%04llx", static_cast<unsigned long long>(token & 0xFFFF));
    return text;
}

} // namespace

Server::Server(ServerOptions options)
    : options_(std::move(options)),
      storage_(options_.data_dir.empty() ? "server_data_" + std::to_string(options_.port)
                                         : options_.data_dir),
      cluster_(options_.host, options_.port, storage_, write_mutex_)
{
}

int Server::run()
{
    if (!storage_.init()) {
        LOG_ERROR("could not create the data directory " << storage_.root());
        return 1;
    }
    int listen_fd = listen_on(options_.port);
    if (listen_fd < 0) {
        LOG_ERROR("could not listen on port " << options_.port << ": " << std::strerror(errno));
        return 1;
    }
    cluster_.set_membership_listener(
        [this](const std::vector<ServerAddress>& servers) { push_server_list(servers); });

    // Joining needs the listening socket already open: the primary connects
    // back to this server as soon as it accepts the join (the connection
    // waits in the backlog until the accept loop below picks it up).
    if (options_.join_host.empty()) {
        cluster_.start_as_primary();
    } else if (!cluster_.join(options_.join_host, options_.join_port)) {
        LOG_ERROR("could not join the cluster at " << options_.join_host << ":" << options_.join_port);
        return 1;
    }
    cluster_.start_monitor();
    LOG_INFO("listening on port " << options_.port << ", data in " << storage_.root());

    while (true) {
        ConnectionPtr conn = accept_from(listen_fd);
        if (!conn) {
            LOG_ERROR("accept failed: " << std::strerror(errno));
            return 1;
        }
        std::thread(&Server::handle_connection, this, conn).detach();
    }
}

void Server::handle_connection(ConnectionPtr conn)
{
    Message first;
    if (conn->recv(first, FIRST_MESSAGE_TIMEOUT_MS) != RecvStatus::Ok) {
        return;
    }
    try {
        switch (first.type) {
        case Msg::Login: serve_client(conn, first); break;
        case Msg::SyncAttach: serve_sync(conn, first); break;
        case Msg::Join: cluster_.handle_join(conn, first); break;
        case Msg::Election: cluster_.handle_election(conn, first); break;
        case Msg::Coordinator: cluster_.handle_coordinator(conn, first); break;
        case Msg::ReplicaAttach: cluster_.handle_replica_attach(conn, first); break;
        default:
            LOG_WARN(conn->peer() << " opened with unexpected " << msg_name(first.type));
            send_error(*conn, "unexpected message");
        }
    } catch (const ProtocolError& e) {
        LOG_WARN("malformed message from " << conn->peer() << ": " << e.what());
    }
}

void Server::send_redirect(Connection& conn)
{
    ServerAddress primary;
    Writer w;
    if (cluster_.primary_address(primary)) {
        w.str(primary.host).u16(primary.port);
    } else {
        w.str("").u16(0);
    }
    conn.send(Msg::Redirect, w.data());
}

// ---------------------------------------------------------------- client side

void Server::serve_client(const ConnectionPtr& conn, const Message& login)
{
    Reader r(login.payload);
    std::string user = r.str();

    if (!cluster_.is_primary()) {
        send_redirect(*conn);
        return;
    }
    if (!is_valid_username(user)) {
        send_error(*conn, "invalid username (use letters, digits, '_' or '-', up to 32 characters)");
        return;
    }
    if (!storage_.ensure_user(user)) {
        send_error(*conn, "the server could not create the user's directory");
        return;
    }
    uint64_t token = sessions_.open(user, conn);
    if (token == 0) {
        send_error(*conn, "this user already has " +
                              std::to_string(SessionManager::MAX_SESSIONS_PER_USER) +
                              " active sessions");
        return;
    }

    Writer w;
    w.u64(token);
    write_servers(w, cluster_.members());
    if (conn->send(Msg::LoginOk, w.data())) {
        LOG_INFO(user << " logged in from " << conn->peer() << " (session " << short_token(token) << ")");
        while (true) {
            Message msg;
            if (conn->recv(msg, CLIENT_IDLE_TIMEOUT_MS) != RecvStatus::Ok) {
                LOG_INFO(user << " disconnected (session " << short_token(token) << ")");
                break;
            }
            if (!cluster_.is_primary()) {
                // Demoted after an election: make the client reconnect, it
                // will be redirected to the new primary.
                send_error(*conn, "this server is no longer the primary");
                break;
            }
            bool keep_going = false;
            try {
                keep_going = handle_request(conn, user, token, msg);
            } catch (const ProtocolError& e) {
                LOG_WARN("malformed request from " << user << ": " << e.what());
            }
            if (!keep_going) {
                break;
            }
        }
    }
    sessions_.close(token);
}

bool Server::handle_request(const ConnectionPtr& conn, const std::string& user, uint64_t token,
                            const Message& msg)
{
    switch (msg.type) {
    case Msg::Ping:
        return conn->send(Msg::Pong);
    case Msg::Logout:
        LOG_INFO(user << " logged out (session " << short_token(token) << ")");
        conn->send(Msg::Ok);
        return false;
    case Msg::ListFiles:
        return handle_list(*conn, user);
    case Msg::Upload:
        return handle_upload(*conn, user, token, msg);
    case Msg::Download:
        return handle_download(*conn, user, msg);
    case Msg::Delete:
        return handle_delete(*conn, user, token, msg);
    default:
        send_error(*conn, std::string("unexpected ") + msg_name(msg.type));
        return false;
    }
}

bool Server::handle_list(Connection& conn, const std::string& user)
{
    std::vector<FileInfo> files = storage_.list(user, true);
    Writer w;
    w.u32(static_cast<uint32_t>(files.size()));
    for (const FileInfo& file : files) {
        write_file_info(w, file);
    }
    if (w.data().size() > MAX_PAYLOAD) {
        return send_error(conn, "too many files to list");
    }
    return conn.send(Msg::FileList, w.data());
}

bool Server::handle_upload(Connection& conn, const std::string& user, uint64_t token,
                           const Message& msg)
{
    Reader r(msg.payload);
    Origin origin = static_cast<Origin>(r.u8());
    FileInfo info = read_file_header(r);

    // Always read the body, even for a bad name, so the stream stays in sync.
    std::string temp = storage_.new_temp_path();
    TransferResult result = recv_file_body(conn, temp, info.size);
    if (result == TransferResult::ConnectionLost) {
        return false;
    }
    if (result != TransferResult::Ok) {
        return send_error(conn, "the upload was not completed");
    }
    if (!is_valid_file_name(info.name)) {
        remove_file(temp);
        return send_error(conn, "invalid file name");
    }

    bool stored;
    {
        std::lock_guard<std::mutex> write(write_mutex_);
        stored = storage_.commit(user, info.name, temp, info.mtime_ns);
        if (stored) {
            cluster_.replicate_put(user, info.name);
        }
    }
    if (!stored) {
        remove_file(temp);
        return send_error(conn, "the server could not store the file");
    }
    LOG_INFO("stored " << user << "/" << info.name << " (" << format_size(info.size) << ")");
    if (!conn.send(Msg::Ok)) {
        return false;
    }
    // A change made in this session's own sync_dir doesn't go back to it.
    push_file(user, info.name, origin == Origin::Sync ? token : 0);
    return true;
}

bool Server::handle_download(Connection& conn, const std::string& user, const Message& msg)
{
    Reader r(msg.payload);
    std::string name = r.str();
    if (!is_valid_file_name(name)) {
        return send_error(conn, "invalid file name");
    }
    TransferResult result = send_file(conn, Msg::File, "", name, storage_.file_path(user, name));
    if (result == TransferResult::NotSent) {
        return send_error(conn, "no such file on the server");
    }
    return result != TransferResult::ConnectionLost;
}

bool Server::handle_delete(Connection& conn, const std::string& user, uint64_t token,
                           const Message& msg)
{
    Reader r(msg.payload);
    Origin origin = static_cast<Origin>(r.u8());
    std::string name = r.str();
    if (!is_valid_file_name(name)) {
        return send_error(conn, "invalid file name");
    }

    bool existed;
    {
        std::lock_guard<std::mutex> write(write_mutex_);
        existed = storage_.remove(user, name);
        if (existed) {
            cluster_.replicate_delete(user, name);
        }
    }
    if (!existed) {
        return send_error(conn, "no such file on the server");
    }
    LOG_INFO("deleted " << user << "/" << name);
    if (!conn.send(Msg::Ok)) {
        return false;
    }
    push_delete(user, name, origin == Origin::Sync ? token : 0);
    return true;
}

// ------------------------------------------------------------------ sync side

void Server::serve_sync(const ConnectionPtr& conn, const Message& attach)
{
    Reader r(attach.payload);
    uint64_t token = r.u64();
    {
        // Hold the send lock so no push can get ahead of the Ok.
        auto lock = conn->lock_send();
        if (!cluster_.is_primary() || !sessions_.attach_sync(token, conn)) {
            Writer w;
            w.str("unknown session");
            conn->send_locked(Msg::Error, w.data());
            return;
        }
        conn->send_locked(Msg::Ok);
    }
    // Clients never send on this connection; reading only tells us when it
    // closes (the session closing it, or the client going away).
    Message ignored;
    while (conn->recv(ignored) == RecvStatus::Ok) {
    }
    sessions_.detach_sync(token, conn);
}

void Server::push_file(const std::string& user, const std::string& name, uint64_t except)
{
    std::string path = storage_.file_path(user, name);
    for (const ConnectionPtr& sync : sessions_.sync_connections(user, except)) {
        if (send_file(*sync, Msg::PushFile, "", name, path) == TransferResult::ConnectionLost) {
            sync->shutdown();
        }
    }
}

void Server::push_delete(const std::string& user, const std::string& name, uint64_t except)
{
    Writer w;
    w.str(name);
    for (const ConnectionPtr& sync : sessions_.sync_connections(user, except)) {
        if (!sync->send(Msg::PushDelete, w.data())) {
            sync->shutdown();
        }
    }
}

void Server::push_server_list(const std::vector<ServerAddress>& servers)
{
    Writer w;
    write_servers(w, servers);
    for (const ConnectionPtr& sync : sessions_.all_sync_connections()) {
        sync->send(Msg::ServerList, w.data());
    }
}
