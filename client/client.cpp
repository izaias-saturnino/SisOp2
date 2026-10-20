#include "client.hpp"

#include "../common/log.hpp"
#include "../common/net.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <map>
#include <utility>

namespace {

constexpr int CONNECT_TIMEOUT_MS = 1500;
constexpr int LOGIN_TIMEOUT_MS = 5000;
constexpr int REPLY_TIMEOUT_MS = 15000;
constexpr int UPLOAD_REPLY_TIMEOUT_MS = 30000; // the primary replicates before answering
constexpr int PING_TIMEOUT_MS = 3000;
constexpr int COMMAND_WAIT_MS = 15000;         // how long a command waits for a reconnect
constexpr auto PING_INTERVAL = std::chrono::seconds(1);

bool same_content(const FileInfo& a, const FileInfo& b)
{
    return a.hash == b.hash && a.size == b.size;
}

std::string trim(const std::string& text)
{
    std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string file_table(const std::vector<FileInfo>& files)
{
    if (files.empty()) {
        return "  (no files)";
    }
    std::string table;
    char line[MAX_PATH_LENGTH + 128];
    std::snprintf(line, sizeof(line), "  %-48s %10s  %s", "PATH", "SIZE", "MODIFIED");
    table += line;
    for (const FileInfo& file : files) {
        std::snprintf(line, sizeof(line), "\n  %-48s %10s  %s", file.name.c_str(),
                      format_size(file.size).c_str(), format_time(file.mtime_ns).c_str());
        table += line;
    }
    return table;
}

const char* const HELP =
    "Commands:\n"
    "  upload <path>     send a file to the server; it then shows up in every synced sync_dir\n"
    "  download <path>   copy a file from the server into the current directory\n"
    "  delete <path>     delete a file from the server and from every synced sync_dir\n"
    "  list_server       list your files on the server\n"
    "  list_client       list the files in your sync_dir\n"
    "  sync              compare sync_dir with the server again (get_sync_dir also works)\n"
    "  exit              log out and quit\n"
    "Files you add, change or delete in sync_dir (subfolders included) are synced automatically.";

} // namespace

Client::Client(ClientOptions options)
    : options_(std::move(options)),
      state_(join_path(options_.sync_dir, std::string(RESERVED_PREFIX) + "_state")),
      watcher_(options_.sync_dir, [this](const std::string& name) { enqueue(name); })
{
}

Client::~Client()
{
    shutdown();
}

void Client::say(const std::string& text)
{
    std::lock_guard<std::mutex> lock(print_mutex_);
    std::cout << text << std::endl;
}

int Client::run()
{
    if (!make_dirs(options_.sync_dir)) {
        say("Could not create the sync directory " + options_.sync_dir);
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        state_.load();
    }

    ConnectResult first = connect_once();
    if (first != ConnectResult::Connected) {
        if (first == ConnectResult::Unreachable) {
            say("Could not connect to " + options_.host + ":" + std::to_string(options_.port));
        }
        return 1;
    }
    if (!watcher_.start()) {
        say("Could not watch " + options_.sync_dir + " for changes");
        shutdown();
        return 1;
    }
    supervisor_ = std::thread(&Client::supervisor_loop, this);
    worker_ = std::thread(&Client::worker_loop, this);

    say("Type 'help' for the list of commands.");
    command_loop();
    shutdown();
    return 0;
}

// ------------------------------------------------------------- connection

Client::ConnectResult Client::connect_once()
{
    // Try the servers we know about, newest primary candidates (highest id)
    // first, then the address given on the command line. Redirects to the
    // primary are tried right after the server that gave them.
    std::vector<std::pair<std::string, uint16_t>> candidates;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        std::vector<ServerAddress> known = servers_;
        std::sort(known.begin(), known.end(),
                  [](const ServerAddress& a, const ServerAddress& b) { return a.id > b.id; });
        for (const ServerAddress& server : known) {
            candidates.emplace_back(server.host, server.port);
        }
    }
    candidates.emplace_back(options_.host, options_.port);

    ConnectResult result = ConnectResult::Unreachable;
    for (std::size_t i = 0; i < candidates.size() && i < 20 && !stopping_; ++i) {
        const std::string host = candidates[i].first;
        const uint16_t port = candidates[i].second;

        ConnectionPtr cmd = connect_to(host, port, CONNECT_TIMEOUT_MS);
        if (!cmd) {
            continue;
        }
        Writer login;
        login.str(options_.user).str(options_.password);
        Message reply;
        CallResult login_result = call(*cmd, Msg::Login, login.data(), Msg::LoginOk, reply, LOGIN_TIMEOUT_MS);
        if (login_result == CallResult::Failed) {
            continue;
        }
        try {
            if (login_result == CallResult::Rejected) {
                if (reply.type == Msg::Redirect) {
                    Reader r(reply.payload);
                    std::string primary_host = r.str();
                    uint16_t primary_port = r.u16();
                    if (!primary_host.empty()) {
                        candidates.insert(candidates.begin() + static_cast<long>(i) + 1,
                                          {primary_host, primary_port});
                    }
                    continue;
                }
                say("Login refused by " + host + ":" + std::to_string(port) + ": " + error_reason(reply));
                result = ConnectResult::Refused;
                continue;
            }

            Reader r(reply.payload);
            std::string token = r.str();
            std::vector<ServerAddress> servers = read_servers(r);

            ConnectionPtr sync = connect_to(host, port, CONNECT_TIMEOUT_MS);
            Writer attach;
            attach.str(token);
            Message ack;
            if (!sync || call(*sync, Msg::SyncAttach, attach.data(), Msg::Ok, ack, LOGIN_TIMEOUT_MS) !=
                             CallResult::Ok) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                cmd_ = cmd;
                sync_ = sync;
                servers_ = servers;
                connected_ = true;
            }
            state_cv_.notify_all();
            say("Connected to " + host + ":" + std::to_string(port) + " as " + options_.user + ".");
            return ConnectResult::Connected;
        } catch (const ProtocolError&) {
            continue;
        }
    }
    return result;
}

bool Client::reconnect()
{
    int attempts = 0;
    while (!stopping_) {
        if (connect_once() == ConnectResult::Connected) {
            return true;
        }
        if (++attempts % 10 == 0) {
            say("Still trying to reach a server...");
        }
        std::unique_lock<std::mutex> lock(state_mutex_);
        state_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_.load(); });
    }
    return false;
}

void Client::supervisor_loop()
{
    bool first = true;
    while (!stopping_) {
        if (!first) {
            say("Connection to the server lost. Reconnecting...");
            if (!reconnect()) {
                return;
            }
        }
        first = false;

        ConnectionPtr cmd;
        ConnectionPtr sync;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            cmd = cmd_;
            sync = sync_;
        }
        // Catch up with whatever changed on either side while disconnected.
        if (reconcile(true)) {
            receive_loop(sync);
        }
        mark_disconnected(cmd);
    }
}

void Client::receive_loop(const ConnectionPtr& sync)
{
    auto last_ping = std::chrono::steady_clock::now();
    while (!stopping_) {
        Message msg;
        RecvStatus status = sync->recv(msg, 250);
        if (status == RecvStatus::Closed) {
            return;
        }
        if (status == RecvStatus::Ok && !handle_push(*sync, msg)) {
            return;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= PING_INTERVAL) {
            last_ping = now;
            if (!ping()) {
                return;
            }
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!connected_) {
            return; // another thread saw the connection fail
        }
    }
}

bool Client::ping()
{
    // A busy command connection is clearly alive; don't queue behind it.
    std::unique_lock<std::mutex> busy(cmd_mutex_, std::try_to_lock);
    if (!busy.owns_lock()) {
        return true;
    }
    ConnectionPtr cmd;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cmd = connected_ ? cmd_ : nullptr;
    }
    Message reply;
    return cmd && call(*cmd, Msg::Ping, "", Msg::Pong, reply, PING_TIMEOUT_MS) == CallResult::Ok;
}

void Client::mark_disconnected(const ConnectionPtr& cmd)
{
    ConnectionPtr old_cmd;
    ConnectionPtr old_sync;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!connected_ || (cmd && cmd != cmd_)) {
            return;
        }
        connected_ = false;
        old_cmd = cmd_;
        old_sync = sync_;
    }
    if (old_cmd) old_cmd->shutdown();
    if (old_sync) old_sync->shutdown();
    state_cv_.notify_all();
}

ConnectionPtr Client::wait_for_connection(int timeout_ms)
{
    std::unique_lock<std::mutex> lock(state_mutex_);
    auto ready = [this] { return connected_ || stopping_.load(); };
    if (timeout_ms < 0) {
        state_cv_.wait(lock, ready);
    } else {
        state_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
    }
    return connected_ && !stopping_ ? cmd_ : nullptr;
}

CallResult Client::transact(const Exchange& exchange, int wait_ms, std::string& error)
{
    ConnectionPtr cmd = wait_for_connection(wait_ms);
    if (!cmd) {
        error = "not connected to a server";
        return CallResult::Failed;
    }
    CallResult result;
    {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        try {
            result = exchange(*cmd, error);
        } catch (const ProtocolError&) {
            result = CallResult::Failed;
        }
    }
    if (result == CallResult::Failed) {
        if (error.empty()) {
            error = "lost the connection to the server";
        }
        mark_disconnected(cmd);
    }
    return result;
}

CallResult Client::upload(const std::string& path, const std::string& name, Origin origin,
                          std::string& error, int wait_ms)
{
    return transact(
        [&](Connection& conn, std::string& err) {
            Writer prefix;
            prefix.u8(static_cast<uint8_t>(origin));
            TransferResult sent = send_file(conn, Msg::Upload, prefix.data(), name, path);
            if (sent == TransferResult::NotSent) {
                err = "cannot read " + path;
                return CallResult::Rejected;
            }
            if (sent == TransferResult::ConnectionLost) {
                return CallResult::Failed;
            }
            Message reply;
            if (conn.recv(reply, UPLOAD_REPLY_TIMEOUT_MS) != RecvStatus::Ok) {
                return CallResult::Failed;
            }
            if (reply.type != Msg::Ok) {
                err = sent == TransferResult::FileError ? path + " changed while it was being read"
                                                        : error_reason(reply);
                return CallResult::Rejected;
            }
            return CallResult::Ok;
        },
        wait_ms, error);
}

CallResult Client::download(const std::string& name, const std::string& dir, FileInfo& got,
                            std::string& temp, std::string& error, int wait_ms)
{
    return transact(
        [&](Connection& conn, std::string& err) {
            Writer request;
            request.str(name);
            if (!conn.send(Msg::Download, request.data())) {
                return CallResult::Failed;
            }
            Message reply;
            if (conn.recv(reply, REPLY_TIMEOUT_MS) != RecvStatus::Ok) {
                return CallResult::Failed;
            }
            if (reply.type == Msg::Error) {
                err = error_reason(reply);
                return CallResult::Rejected;
            }
            if (reply.type != Msg::File) {
                return CallResult::Failed;
            }
            Reader r(reply.payload);
            got = read_file_header(r);
            temp = make_temp_path(dir);
            TransferResult received = recv_file_body(conn, temp, got.size, &got.hash);
            if (received == TransferResult::ConnectionLost) {
                return CallResult::Failed;
            }
            if (received != TransferResult::Ok) {
                err = "the download of " + name + " was not completed";
                return CallResult::Rejected;
            }
            return CallResult::Ok;
        },
        wait_ms, error);
}

CallResult Client::remove_remote(const std::string& name, Origin origin, std::string& error, int wait_ms)
{
    return transact(
        [&](Connection& conn, std::string& err) {
            Writer request;
            request.u8(static_cast<uint8_t>(origin)).str(name);
            Message reply;
            CallResult result = call(conn, Msg::Delete, request.data(), Msg::Ok, reply, REPLY_TIMEOUT_MS);
            if (result == CallResult::Rejected) {
                err = error_reason(reply);
            }
            return result;
        },
        wait_ms, error);
}

CallResult Client::list_remote(std::vector<FileInfo>& files, std::string& error, int wait_ms)
{
    return transact(
        [&](Connection& conn, std::string& err) {
            Message reply;
            CallResult result = call(conn, Msg::ListFiles, "", Msg::FileList, reply, REPLY_TIMEOUT_MS);
            if (result == CallResult::Rejected) {
                err = error_reason(reply);
            } else if (result == CallResult::Ok) {
                Reader r(reply.payload);
                uint32_t count = r.u32();
                files.clear();
                for (uint32_t i = 0; i < count; ++i) {
                    files.push_back(read_file_info(r));
                }
            }
            return result;
        },
        wait_ms, error);
}

// ------------------------------------------------------------ synchronization

// Three-way comparison of every file between sync_dir (local), the server
// (remote) and the state saved at the last sync (base):
//  - same on both sides: nothing to do;
//  - changed on one side only (created, modified or deleted): apply it to
//    the other side;
//  - modified on both: the server's version takes the name and the local one
//    is kept as a conflicted copy (see install), so neither edit is lost;
//  - modified on one side, deleted on the other: the file is kept.
// Returns false if the connection failed.
bool Client::reconcile(bool report)
{
    std::lock_guard<std::mutex> lock(sync_mutex_);

    std::vector<FileInfo> remote_files;
    std::string error;
    CallResult listed = list_remote(remote_files, error, 0);
    if (listed != CallResult::Ok) {
        if (listed == CallResult::Rejected) {
            say("Could not list the files on the server: " + error);
        }
        return listed != CallResult::Failed;
    }

    std::map<std::string, FileInfo> local;
    std::map<std::string, FileInfo> remote;
    std::set<std::string> names;
    for (FileInfo file : list_tree(options_.sync_dir)) {
        if (fill_hash(file)) {
            local[file.name] = file;
            names.insert(file.name);
        }
    }
    for (const FileInfo& file : remote_files) {
        if (is_valid_path(file.name)) {
            remote[file.name] = file;
            names.insert(file.name);
        }
    }
    for (const std::string& name : state_.names()) {
        names.insert(name);
    }

    int uploaded = 0, downloaded = 0, deleted_here = 0, deleted_there = 0;
    for (const std::string& name : names) {
        auto l = local.find(name);
        auto r = remote.find(name);
        const FileInfo* here = l == local.end() ? nullptr : &l->second;
        const FileInfo* there = r == remote.end() ? nullptr : &r->second;
        FileInfo base;
        bool has_base = state_.get(name, base);

        if (here && there && same_content(*here, *there)) {
            state_.set(*here);
            continue;
        }
        if (!here && !there) {
            state_.erase(name);
            continue;
        }

        bool changed_here = here ? !(has_base && same_content(base, *here)) : has_base;
        bool changed_there = there ? !(has_base && same_content(base, *there)) : has_base;
        bool push;
        if (changed_here != changed_there) {
            push = changed_here;
        } else if (here && there) {
            push = false; // both modified: take the server's, keep ours as a conflicted copy
        } else {
            push = here != nullptr;
        }

        std::string path = join_path(options_.sync_dir, name);
        CallResult result = CallResult::Ok;
        if (push && here) {
            result = upload(path, name, Origin::Sync, error, 0);
            if (result == CallResult::Ok) {
                state_.set(*here);
                ++uploaded;
            }
        } else if (push) {
            result = remove_remote(name, Origin::Sync, error, 0);
            if (result != CallResult::Failed) {
                state_.erase(name);
                ++deleted_there;
            }
        } else if (there) {
            FileInfo got;
            std::string temp;
            result = download(name, options_.sync_dir, got, temp, error, 0);
            if (result == CallResult::Ok) {
                install(temp, name, got);
                ++downloaded;
            }
        } else {
            remove_file(path);
            remove_empty_parents(options_.sync_dir, name);
            state_.erase(name);
            ++deleted_here;
        }

        if (result == CallResult::Failed) {
            state_.save();
            return false;
        }
        if (result == CallResult::Rejected) {
            say("Could not sync " + name + ": " + error);
        }
    }
    state_.save();

    if (report && uploaded + downloaded + deleted_here + deleted_there > 0) {
        say("Synchronized with the server: " + std::to_string(uploaded) + " uploaded, " +
            std::to_string(downloaded) + " downloaded, " + std::to_string(deleted_there) +
            " deleted on the server, " + std::to_string(deleted_here) + " deleted locally.");
    }
    return true;
}

// Caller holds sync_mutex_. Fills in the content hash of a local file, reusing
// the saved one when size and modification time haven't changed since the
// last sync (like git's index), so unchanged files aren't read again.
bool Client::fill_hash(FileInfo& file)
{
    FileInfo base;
    if (state_.get(file.name, base) && base.size == file.size && base.mtime_ns == file.mtime_ns) {
        file.hash = base.hash;
        return true;
    }
    return hash_file(join_path(options_.sync_dir, file.name), file.hash);
}

// Caller holds sync_mutex_. False if `name` isn't a regular file in sync_dir.
bool Client::local_file(const std::string& name, FileInfo& info)
{
    info.name = name;
    return stat_file(join_path(options_.sync_dir, name), info, false) && fill_hash(info);
}

// A path for keeping a local version that would otherwise be overwritten:
// "docs/notes (conflicted copy 2026-10-20 14.03.07).txt".
std::string Client::conflict_name(const std::string& name) const
{
    std::string dir = parent_path(name);
    std::string file = base_name(name);
    std::size_t dot = file.rfind('.');
    if (dot == 0 || dot == std::string::npos) {
        dot = file.size();
    }
    std::string stem = file.substr(0, dot);
    std::string extension = file.substr(dot);

    std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", &local);

    for (int attempt = 1;; ++attempt) {
        std::string tag = " (conflicted copy " + std::string(stamp) +
                          (attempt > 1 ? " " + std::to_string(attempt) : "") + ")";
        std::size_t room = MAX_NAME_LENGTH - tag.size() - std::min(extension.size(), std::size_t(32));
        std::string candidate = stem.substr(0, room) + tag + extension.substr(0, 32);
        if (!dir.empty()) {
            candidate = dir + "/" + candidate;
        }
        FileInfo ignored;
        if (!stat_file(join_path(options_.sync_dir, candidate), ignored, false)) {
            return candidate;
        }
    }
}

// Caller holds sync_mutex_. Moves a received file into sync_dir. If the
// local file has changes that were never synced (it differs from both the
// base and the incoming version), it is kept as a conflicted copy first; the
// watcher then reports the copy and it gets uploaded like any new file.
void Client::install(const std::string& temp, const std::string& name, const FileInfo& info)
{
    std::string path = join_path(options_.sync_dir, name);
    FileInfo current;
    FileInfo base;
    if (local_file(name, current) && !same_content(current, info) &&
        !(state_.get(name, base) && same_content(base, current))) {
        std::string copy = conflict_name(name);
        if (replace_file(path, join_path(options_.sync_dir, copy))) {
            say("[sync] " + name + " was changed both here and elsewhere; your version was kept as \"" +
                copy + "\"");
        }
    }

    set_mtime(temp, info.mtime_ns);
    FileInfo entry = info;
    entry.name = name;
    // Record it before the rename, so the watcher event the rename causes is
    // recognized as already in sync instead of being uploaded back.
    state_.set(entry);
    if (!make_parent_dirs(options_.sync_dir, name) || !replace_file(temp, path)) {
        remove_file(temp);
        state_.erase(name);
        say("Could not write " + path + " (a file and a folder can't have the same name)");
    }
    state_.save();
}

void Client::sync_local_change(const std::string& name)
{
    std::lock_guard<std::mutex> lock(sync_mutex_);
    std::string path = join_path(options_.sync_dir, name);
    FileInfo here;
    bool exists = local_file(name, here);
    FileInfo base;
    bool has_base = state_.get(name, base);

    // Already in sync: our own download, a duplicate event, or a file
    // rewritten with the same contents.
    if (exists ? (has_base && same_content(base, here)) : !has_base) {
        return;
    }

    std::string error;
    if (exists) {
        CallResult result = upload(path, name, Origin::Sync, error, 0);
        if (result == CallResult::Ok) {
            state_.set(here);
            state_.save();
            say("[sync] uploaded " + name);
        } else if (result == CallResult::Rejected) {
            say("[sync] could not upload " + name + ": " + error);
        }
        // Failed: the reconcile after reconnecting will pick it up.
    } else {
        CallResult result = remove_remote(name, Origin::Sync, error, 0);
        if (result != CallResult::Failed) {
            state_.erase(name);
            state_.save();
            if (result == CallResult::Ok) {
                say("[sync] deleted " + name + " on the server");
            }
        }
    }
}

// Returns false if the sync connection can't be trusted any more.
bool Client::handle_push(Connection& sync, const Message& msg)
{
    try {
        switch (msg.type) {
        case Msg::PushFile: {
            Reader r(msg.payload);
            FileInfo info = read_file_header(r);
            std::string temp = make_temp_path(options_.sync_dir);
            TransferResult received = recv_file_body(sync, temp, info.size, &info.hash);
            if (received == TransferResult::ConnectionLost) {
                return false;
            }
            if (received != TransferResult::Ok || !is_valid_path(info.name)) {
                remove_file(temp);
                return true;
            }
            {
                std::lock_guard<std::mutex> lock(sync_mutex_);
                install(temp, info.name, info);
            }
            say("[sync] received " + info.name + " (" + format_size(info.size) + ")");
            return true;
        }
        case Msg::PushDelete: {
            Reader r(msg.payload);
            std::string name = r.str();
            if (is_valid_path(name)) {
                std::lock_guard<std::mutex> lock(sync_mutex_);
                state_.erase(name);
                state_.save();
                if (remove_file(join_path(options_.sync_dir, name))) {
                    remove_empty_parents(options_.sync_dir, name);
                    say("[sync] " + name + " was deleted on the server");
                }
            }
            return true;
        }
        case Msg::ServerList: {
            Reader r(msg.payload);
            std::vector<ServerAddress> servers = read_servers(r);
            std::lock_guard<std::mutex> lock(state_mutex_);
            servers_ = servers;
            return true;
        }
        default:
            LOG_WARN("unexpected " << msg_name(msg.type) << " on the sync connection");
            return true;
        }
    } catch (const ProtocolError& e) {
        LOG_WARN("malformed push: " << e.what());
        return false;
    }
}

void Client::enqueue(const std::string& name)
{
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (!queued_.insert(name).second) {
            return;
        }
        queue_.push_back(name);
    }
    queue_cv_.notify_one();
}

void Client::worker_loop()
{
    while (true) {
        std::string name;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            name = queue_.front();
            queue_.pop_front();
            queued_.erase(name);
        }
        // Wait for a connection before taking the sync lock: the reconcile
        // that follows a reconnect needs that lock.
        if (!wait_for_connection(-1)) {
            continue;
        }
        if (name.empty()) {
            rescan_local();
        } else {
            sync_local_change(name);
        }
    }
}

// Checks every file against the saved state. Used when a whole folder was
// created, moved or deleted (inotify reports only the folder, not its
// contents) and when the watcher lost events. Unchanged files only cost a
// stat, thanks to fill_hash.
void Client::rescan_local()
{
    std::set<std::string> names;
    {
        std::lock_guard<std::mutex> lock(sync_mutex_);
        for (const FileInfo& file : list_tree(options_.sync_dir)) {
            names.insert(file.name);
        }
        for (const std::string& name : state_.names()) {
            names.insert(name);
        }
    }
    for (const std::string& name : names) {
        sync_local_change(name);
    }
}

// ------------------------------------------------------------------- commands

void Client::command_loop()
{
    std::string line;
    while (!stopping_ && std::getline(std::cin, line)) {
        if (!run_command(line)) {
            break;
        }
    }
}

bool Client::run_command(const std::string& line)
{
    std::string text = trim(line);
    if (text.empty()) {
        return true;
    }
    std::size_t space = text.find_first_of(" \t");
    std::string command = text.substr(0, space);
    std::string argument = space == std::string::npos ? "" : trim(text.substr(space));

    if (command == "exit" || command == "quit") {
        return false;
    } else if (command == "help") {
        say(HELP);
    } else if (command == "upload") {
        cmd_upload(argument);
    } else if (command == "download") {
        cmd_download(argument);
    } else if (command == "delete") {
        cmd_delete(argument);
    } else if (command == "list_server") {
        cmd_list_server();
    } else if (command == "list_client") {
        cmd_list_client();
    } else if (command == "sync" || command == "get_sync_dir") {
        cmd_sync();
    } else {
        say("Unknown command '" + command + "'. Type 'help' for the list of commands.");
    }
    return true;
}

void Client::cmd_upload(const std::string& path)
{
    std::string name = base_name(path);
    if (path.empty() || !is_valid_file_name(name)) {
        say("Usage: upload <path to a file>");
        return;
    }
    FileInfo info;
    if (!stat_file(path, info, false)) {
        say("Cannot read " + path + " (it must be an existing regular file)");
        return;
    }
    std::string error;
    if (upload(path, name, Origin::Command, error, COMMAND_WAIT_MS) == CallResult::Ok) {
        say("Uploaded " + name + " (" + format_size(info.size) + ").");
    } else {
        say("Upload failed: " + error);
    }
}

void Client::cmd_download(const std::string& name)
{
    if (!is_valid_path(name)) {
        say("Usage: download <file path, as shown by list_server>");
        return;
    }
    FileInfo got;
    std::string temp;
    std::string error;
    if (download(name, ".", got, temp, error, COMMAND_WAIT_MS) != CallResult::Ok) {
        say("Download failed: " + error);
        return;
    }
    set_mtime(temp, got.mtime_ns);
    std::string local = base_name(name);
    if (replace_file(temp, local)) {
        say("Downloaded " + name + " (" + format_size(got.size) + ") to ./" + local);
    } else {
        remove_file(temp);
        say("Could not write ./" + local);
    }
}

void Client::cmd_delete(const std::string& name)
{
    if (!is_valid_path(name)) {
        say("Usage: delete <file path, as shown by list_server>");
        return;
    }
    std::string error;
    if (remove_remote(name, Origin::Command, error, COMMAND_WAIT_MS) == CallResult::Ok) {
        say("Deleted " + name + " on the server.");
    } else {
        say("Delete failed: " + error);
    }
}

void Client::cmd_list_server()
{
    std::vector<FileInfo> files;
    std::string error;
    if (list_remote(files, error, COMMAND_WAIT_MS) == CallResult::Ok) {
        say("Files on the server:\n" + file_table(files));
    } else {
        say("Could not list the server's files: " + error);
    }
}

void Client::cmd_list_client()
{
    say("Files in " + options_.sync_dir + ":\n" + file_table(list_tree(options_.sync_dir)));
}

void Client::cmd_sync()
{
    if (!wait_for_connection(COMMAND_WAIT_MS)) {
        say("Not connected to a server.");
    } else if (reconcile(true)) {
        say("The sync directory is up to date.");
    } else {
        say("Lost the connection while synchronizing; it will be retried after reconnecting.");
    }
}

void Client::shutdown()
{
    if (stopping_.exchange(true)) {
        return;
    }
    ConnectionPtr cmd;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        cmd = connected_ ? cmd_ : nullptr;
    }
    if (cmd) {
        std::lock_guard<std::mutex> lock(cmd_mutex_);
        Message reply;
        call(*cmd, Msg::Logout, "", Msg::Ok, reply, 2000);
    }
    mark_disconnected(nullptr);
    state_cv_.notify_all();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
    }
    queue_cv_.notify_all();
    watcher_.stop();
    if (supervisor_.joinable()) {
        supervisor_.join();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    if (cmd) {
        say("Logged out.");
    }
}
