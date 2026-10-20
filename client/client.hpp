#pragma once

#include "../common/connection.hpp"
#include "../common/files.hpp"
#include "sync_state.hpp"
#include "watcher.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct ClientOptions {
    std::string user;
    std::string password;
    std::string host;
    uint16_t port = 4000;
    std::string sync_dir = "sync_dir";
    std::string ca_file = "certs/ca.pem"; // to verify the server's certificate
};

// The client keeps `sync_dir` in sync with the user's files on the server.
//
// Threads:
//  - main: reads and runs the user's commands.
//  - supervisor: owns the connection. Logs in, reconciles sync_dir with the
//    server, then applies the changes the server pushes and pings it every
//    second. When the connection fails it reconnects (to another server if
//    needed, following redirects to the primary) and reconciles again.
//  - worker: turns watcher events into uploads and deletions.
//  - watcher: inotify on sync_dir (see Watcher).
//
// Two connections per session: the command connection carries
// request/response exchanges, one at a time (cmd_mutex_); the sync connection
// only carries pushes from the server, read by the supervisor.
class Client {
public:
    explicit Client(ClientOptions options);
    ~Client();
    int run();

private:
    enum class ConnectResult { Connected, Unreachable, Refused };
    using Exchange = std::function<CallResult(Connection&, std::string& error)>;

    // Connection management.
    ConnectResult connect_once();
    bool reconnect();
    void supervisor_loop();
    void receive_loop(const ConnectionPtr& sync);
    bool ping();
    void mark_disconnected(const ConnectionPtr& cmd);
    ConnectionPtr wait_for_connection(int timeout_ms);

    // One request/response exchange on the command connection. On anything
    // but Ok, `error` explains what went wrong; Failed also means the
    // connection was dropped and will be re-established.
    CallResult transact(const Exchange& exchange, int wait_ms, std::string& error);
    CallResult upload(const std::string& path, const std::string& name, Origin origin,
                      std::string& error, int wait_ms);
    CallResult download(const std::string& name, const std::string& dir, FileInfo& got,
                        std::string& temp, std::string& error, int wait_ms);
    CallResult remove_remote(const std::string& name, Origin origin, std::string& error, int wait_ms);
    CallResult list_remote(std::vector<FileInfo>& files, std::string& error, int wait_ms);

    // Synchronization; these take sync_mutex_.
    bool reconcile(bool report);
    void sync_local_change(const std::string& name);
    bool handle_push(Connection& sync, const Message& msg);
    void install(const std::string& temp, const std::string& name, const FileInfo& info);
    bool fill_hash(FileInfo& file);
    bool local_file(const std::string& name, FileInfo& info);
    void rescan_local();
    std::string conflict_name(const std::string& name) const;
    void enqueue(const std::string& name);
    void worker_loop();

    // Commands.
    void command_loop();
    bool run_command(const std::string& line); // false: quit
    void cmd_upload(const std::string& path);
    void cmd_download(const std::string& name);
    void cmd_delete(const std::string& name);
    void cmd_list_server();
    void cmd_list_client();
    void cmd_sync();
    void shutdown();

    void say(const std::string& text);

    ClientOptions options_;

    std::mutex state_mutex_; // guards the connection state below
    std::condition_variable state_cv_;
    ConnectionPtr cmd_;
    ConnectionPtr sync_;
    bool connected_ = false;
    std::vector<ServerAddress> servers_;
    std::mutex cmd_mutex_;
    std::atomic<bool> stopping_{false};

    std::mutex sync_mutex_; // guards state_ and changes inside sync_dir
    SyncState state_;

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> queue_;
    std::set<std::string> queued_;

    Watcher watcher_;
    std::thread supervisor_;
    std::thread worker_;
    std::mutex print_mutex_;
};
