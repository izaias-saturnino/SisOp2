#include "cluster.hpp"

#include "../common/log.hpp"
#include "../common/net.hpp"

#include <algorithm>
#include <utility>

namespace {

constexpr uint32_t FIRST_ID = 1000;

constexpr auto HEARTBEAT_INTERVAL = std::chrono::milliseconds(500);
constexpr auto PRIMARY_TIMEOUT = std::chrono::milliseconds(3000);
constexpr auto ATTACH_GRACE = std::chrono::milliseconds(5000);
constexpr auto COORDINATOR_WAIT = std::chrono::seconds(5);

constexpr int CONNECT_TIMEOUT_MS = 1500;
constexpr int REPLY_TIMEOUT_MS = 15000;
constexpr int ATTACH_TIMEOUT_MS = 60000; // the backup hashes all its files first
constexpr int HEARTBEAT_TIMEOUT_MS = 2000;
constexpr int BACKUP_IDLE_TIMEOUT_MS = 5000;

using FileKey = std::pair<std::string, std::string>; // user, file name

void upsert(std::vector<ServerAddress>& list, const ServerAddress& server)
{
    for (ServerAddress& existing : list) {
        if (existing.id == server.id) {
            existing = server;
            return;
        }
    }
    list.push_back(server);
}

} // namespace

Cluster::Cluster(std::string host, uint16_t port, Storage& storage, Accounts& accounts,
                 std::mutex& write_mutex)
    : storage_(storage), accounts_(accounts), write_mutex_(write_mutex)
{
    self_.host = std::move(host);
    self_.port = port;
}

Cluster::~Cluster()
{
    stopping_ = true;
    if (monitor_.joinable()) {
        monitor_.join();
    }
}

void Cluster::set_membership_listener(MembershipListener listener)
{
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
}

void Cluster::start_as_primary()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        self_.id = FIRST_ID;
        lowest_id_ = FIRST_ID;
        role_ = Role::Primary;
        primary_ = self_;
        primary_known_ = true;
        members_ = {self_};
    }
    LOG_INFO("started a new cluster; this server is the primary " << describe(self()));
}

bool Cluster::join(const std::string& host, uint16_t port)
{
    std::string target_host = host;
    uint16_t target_port = port;

    for (int attempt = 0; attempt < 10 && !stopping_; ++attempt) {
        if (ConnectionPtr conn = connect_to(target_host, target_port, CONNECT_TIMEOUT_MS)) {
            Writer request;
            ServerAddress me = self();
            request.str(me.host).u16(me.port);
            Message reply;
            CallResult result = call(*conn, Msg::Join, request.data(), Msg::JoinOk, reply, REPLY_TIMEOUT_MS);

            if (result == CallResult::Ok) {
                Reader r(reply.payload);
                uint32_t id = r.u32();
                uint32_t primary_id = r.u32();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    self_.id = id;
                    role_ = Role::Backup;
                    primary_ = ServerAddress{primary_id, target_host, target_port};
                    primary_known_ = true;
                    members_ = {primary_, self_};
                    // The primary attaches its replica link right after this.
                    last_heard_ = Clock::now() + ATTACH_GRACE;
                }
                LOG_INFO("joined the cluster as " << describe(self()) << "; the primary is "
                                                  << target_host << ":" << target_port);
                return true;
            }
            if (result == CallResult::Rejected && reply.type == Msg::Redirect) {
                Reader r(reply.payload);
                std::string redirect_host = r.str();
                uint16_t redirect_port = r.u16();
                if (!redirect_host.empty()) {
                    LOG_INFO("redirected to the primary at " << redirect_host << ":" << redirect_port);
                    target_host = redirect_host;
                    target_port = redirect_port;
                    continue;
                }
                LOG_INFO("the cluster has no primary right now; retrying");
            } else if (result == CallResult::Rejected) {
                LOG_WARN("join refused: " << error_reason(reply));
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return false;
}

void Cluster::start_monitor()
{
    monitor_ = std::thread(&Cluster::monitor_loop, this);
}

bool Cluster::is_primary() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return role_ == Role::Primary;
}

bool Cluster::primary_address(ServerAddress& out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!primary_known_) {
        return false;
    }
    out = primary_;
    return true;
}

std::vector<ServerAddress> Cluster::members() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return members_;
}

ServerAddress Cluster::self() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return self_;
}

void Cluster::add_member(const ServerAddress& member)
{
    std::lock_guard<std::mutex> lock(mutex_);
    upsert(members_, member);
    lowest_id_ = std::min(lowest_id_ == 0 ? member.id : lowest_id_, member.id);
}

void Cluster::remove_member(uint32_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [id](const ServerAddress& m) { return m.id == id; }),
                   members_.end());
}

void Cluster::notify_membership()
{
    MembershipListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        listener = listener_;
    }
    if (listener) {
        listener(members());
    }
}

// ---------------------------------------------------------------- primary side

void Cluster::handle_join(const ConnectionPtr& conn, const Message& first)
{
    Reader r(first.payload);
    ServerAddress member;
    member.host = r.str();
    member.port = r.u16();

    bool primary_here;
    ServerAddress primary;
    bool known;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        primary_here = role_ == Role::Primary;
        primary = primary_;
        known = primary_known_;
        if (primary_here) {
            // A server coming back on the same address keeps its id.
            for (const ServerAddress& existing : members_) {
                if (existing.host == member.host && existing.port == member.port) {
                    member.id = existing.id;
                }
            }
            if (member.id == 0) {
                for (const ServerAddress& existing : members_) {
                    lowest_id_ = std::min(lowest_id_, existing.id);
                }
                member.id = lowest_id_ - 1;
            }
        }
    }

    if (!primary_here) {
        Writer w;
        w.str(known ? primary.host : std::string()).u16(known ? primary.port : 0);
        conn->send(Msg::Redirect, w.data());
        return;
    }
    if (member.id == 0) {
        send_error(*conn, "no more server ids available");
        return;
    }

    add_member(member);
    Writer w;
    w.u32(member.id).u32(self().id);
    conn->send(Msg::JoinOk, w.data());
    LOG_INFO("server " << describe(member) << " is joining");

    if (!attach_replica(member)) {
        LOG_WARN("could not bring " << describe(member) << " up to date; removing it");
        remove_member(member.id);
    }
    broadcast_membership();
}

bool Cluster::attach_replica(const ServerAddress& member)
{
    // Holding the write mutex means no change can slip in between comparing
    // the file lists and the link going live.
    std::lock_guard<std::mutex> write(write_mutex_);

    // Hash our own files first so the backup isn't left waiting on the link.
    std::map<FileKey, FileInfo> local;
    for (const std::string& user : storage_.users()) {
        for (const FileInfo& file : storage_.list(user)) {
            local[{user, file.name}] = file;
        }
    }
    std::vector<std::pair<std::string, std::string>> local_accounts = accounts_.all();

    ConnectionPtr conn = connect_to(member.host, member.port, CONNECT_TIMEOUT_MS);
    if (!conn) {
        return false;
    }
    auto link = std::make_shared<Link>();
    link->member = member;
    link->conn = conn;
    std::lock_guard<std::mutex> txn(link->txn);

    Writer attach;
    write_server(attach, self());
    write_servers(attach, members());
    Message manifest;
    if (call(*conn, Msg::ReplicaAttach, attach.data(), Msg::ReplicaManifest, manifest,
             ATTACH_TIMEOUT_MS) != CallResult::Ok) {
        return false;
    }

    std::map<std::string, std::string> remote_accounts;
    std::map<FileKey, FileInfo> remote;
    try {
        Reader r(manifest.payload);
        uint32_t accounts = r.u32();
        for (uint32_t i = 0; i < accounts; ++i) {
            std::string user = r.str();
            remote_accounts[user] = r.str();
        }
        uint32_t files = r.u32();
        for (uint32_t i = 0; i < files; ++i) {
            std::string user = r.str();
            FileInfo file = read_file_info(r);
            remote[{user, file.name}] = file;
        }
    } catch (const ProtocolError&) {
        return false;
    }

    for (const auto& account : local_accounts) {
        auto it = remote_accounts.find(account.first);
        if ((it == remote_accounts.end() || it->second != account.second) &&
            !account_on_link(*link, account.first, account.second)) {
            return false;
        }
    }
    int sent = 0;
    int deleted = 0;
    for (const auto& entry : local) {
        auto it = remote.find(entry.first);
        if (it != remote.end() && it->second.hash == entry.second.hash &&
            it->second.size == entry.second.size) {
            continue;
        }
        if (!put_on_link(*link, entry.first.first, entry.first.second)) {
            return false;
        }
        ++sent;
    }
    for (const auto& entry : remote) {
        if (local.count(entry.first) == 0) {
            if (!delete_on_link(*link, entry.first.first, entry.first.second)) {
                return false;
            }
            ++deleted;
        }
    }

    LinkPtr previous;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(member.id);
        if (it != links_.end()) {
            previous = it->second;
        }
        links_[member.id] = link;
    }
    if (previous) {
        previous->conn->shutdown();
    }
    LOG_INFO("backup " << describe(member) << " is up to date (" << sent << " file(s) sent, "
                       << deleted << " deleted)");
    return true;
}

// Caller holds link.txn.
bool Cluster::put_on_link(Link& link, const std::string& user, const std::string& name)
{
    Writer prefix;
    prefix.str(user);
    TransferResult result =
        send_file(*link.conn, Msg::ReplPut, prefix.data(), name, storage_.file_path(user, name));
    if (result == TransferResult::NotSent) {
        return true; // the file is gone; its delete is replicated separately
    }
    if (result == TransferResult::ConnectionLost) {
        return false;
    }
    Message reply;
    if (link.conn->recv(reply, REPLY_TIMEOUT_MS) != RecvStatus::Ok) {
        return false;
    }
    // After an aborted transfer the backup answers Error, which is expected.
    return reply.type == Msg::Ok || result == TransferResult::FileError;
}

// Caller holds link.txn.
bool Cluster::delete_on_link(Link& link, const std::string& user, const std::string& name)
{
    Writer w;
    w.str(user).str(name);
    Message reply;
    return call(*link.conn, Msg::ReplDelete, w.data(), Msg::Ok, reply, REPLY_TIMEOUT_MS) ==
           CallResult::Ok;
}

// Caller holds link.txn.
bool Cluster::account_on_link(Link& link, const std::string& user, const std::string& record)
{
    Writer w;
    w.str(user).str(record);
    Message reply;
    return call(*link.conn, Msg::ReplAccount, w.data(), Msg::Ok, reply, REPLY_TIMEOUT_MS) ==
           CallResult::Ok;
}

void Cluster::replicate_account(const std::string& user, const std::string& record)
{
    std::vector<uint32_t> failed;
    for (const LinkPtr& link : links_snapshot()) {
        std::lock_guard<std::mutex> txn(link->txn);
        if (!account_on_link(*link, user, record)) {
            failed.push_back(link->member.id);
        }
    }
    for (uint32_t id : failed) {
        drop_link(id, "could not replicate an account");
    }
}

void Cluster::replicate_put(const std::string& user, const std::string& name)
{
    std::vector<uint32_t> failed;
    for (const LinkPtr& link : links_snapshot()) {
        std::lock_guard<std::mutex> txn(link->txn);
        if (!put_on_link(*link, user, name)) {
            failed.push_back(link->member.id);
        }
    }
    for (uint32_t id : failed) {
        drop_link(id, "could not replicate a change");
    }
}

void Cluster::replicate_delete(const std::string& user, const std::string& name)
{
    std::vector<uint32_t> failed;
    for (const LinkPtr& link : links_snapshot()) {
        std::lock_guard<std::mutex> txn(link->txn);
        if (!delete_on_link(*link, user, name)) {
            failed.push_back(link->member.id);
        }
    }
    for (uint32_t id : failed) {
        drop_link(id, "could not replicate a change");
    }
}

std::vector<Cluster::LinkPtr> Cluster::links_snapshot()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LinkPtr> links;
    for (const auto& entry : links_) {
        links.push_back(entry.second);
    }
    return links;
}

void Cluster::drop_link(uint32_t id, const std::string& reason)
{
    LinkPtr link;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = links_.find(id);
        if (it == links_.end()) {
            return;
        }
        link = it->second;
        links_.erase(it);
    }
    remove_member(id);
    link->conn->shutdown();
    LOG_WARN("dropped backup " << describe(link->member) << ": " << reason);
    broadcast_membership();
}

void Cluster::broadcast_membership()
{
    Writer w;
    write_servers(w, members());
    std::vector<uint32_t> failed;
    for (const LinkPtr& link : links_snapshot()) {
        std::lock_guard<std::mutex> txn(link->txn);
        Message reply;
        if (call(*link->conn, Msg::Membership, w.data(), Msg::Ok, reply, REPLY_TIMEOUT_MS) !=
            CallResult::Ok) {
            failed.push_back(link->member.id);
        }
    }
    notify_membership();
    for (uint32_t id : failed) {
        drop_link(id, "membership update failed");
    }
}

void Cluster::heartbeat_links()
{
    std::vector<uint32_t> failed;
    for (const LinkPtr& link : links_snapshot()) {
        // A busy link is clearly alive; don't wait behind a file transfer.
        std::unique_lock<std::mutex> txn(link->txn, std::try_to_lock);
        if (!txn.owns_lock()) {
            continue;
        }
        Message reply;
        if (call(*link->conn, Msg::Heartbeat, "", Msg::Ok, reply, HEARTBEAT_TIMEOUT_MS) !=
            CallResult::Ok) {
            failed.push_back(link->member.id);
        }
    }
    for (uint32_t id : failed) {
        drop_link(id, "heartbeat failed");
    }
}

// ----------------------------------------------------------------- backup side

void Cluster::handle_replica_attach(const ConnectionPtr& conn, const Message& first)
{
    Reader r(first.payload);
    ServerAddress primary = read_server(r);
    std::vector<ServerAddress> members = read_servers(r);

    std::vector<LinkPtr> stale_links;
    ConnectionPtr stale_primary_link;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ == Role::Primary) {
            // Another server took over; stop acting as primary.
            for (const auto& entry : links_) {
                stale_links.push_back(entry.second);
            }
            links_.clear();
        }
        stale_primary_link = primary_link_;
        primary_link_ = conn;
        role_ = Role::Backup;
        primary_ = primary;
        primary_known_ = true;
        coordinator_seen_ = true;
        members_ = members;
        upsert(members_, self_);
        building_manifest_ = true;
    }
    coordinator_cv_.notify_all();
    for (const LinkPtr& link : stale_links) {
        link->conn->shutdown();
    }
    if (stale_primary_link && stale_primary_link != conn) {
        stale_primary_link->shutdown();
    }
    LOG_INFO("replica link from the primary " << describe(primary));
    notify_membership();

    std::string manifest = build_manifest();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        building_manifest_ = false;
        last_heard_ = Clock::now();
    }
    if (!conn->send(Msg::ReplicaManifest, manifest)) {
        LOG_WARN("could not send the file manifest to the primary");
    } else {
        while (!stopping_) {
            Message msg;
            if (conn->recv(msg, BACKUP_IDLE_TIMEOUT_MS) != RecvStatus::Ok) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (primary_link_ != conn) {
                    break; // replaced by a newer primary
                }
                last_heard_ = Clock::now();
            }
            bool ok = false;
            try {
                ok = apply_replicated(*conn, msg);
            } catch (const ProtocolError& e) {
                LOG_WARN("malformed message from the primary: " << e.what());
            }
            if (!ok) {
                break;
            }
        }
    }

    bool lost_primary;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lost_primary = primary_link_ == conn;
        if (lost_primary) {
            primary_link_.reset();
        }
    }
    conn->shutdown();
    LOG_INFO("replica link from " << describe(primary) << " closed");
    // Don't wait for the silence timeout: if the primary is alive it answers
    // the election and attaches a new link; if not, failover starts now.
    if (lost_primary && !stopping_) {
        start_election();
    }
}

// Returns false when the link should be closed.
bool Cluster::apply_replicated(Connection& conn, const Message& msg)
{
    switch (msg.type) {
    case Msg::ReplPut: {
        Reader r(msg.payload);
        std::string user = r.str();
        FileInfo info = read_file_header(r);
        std::string temp = storage_.new_temp_path();
        TransferResult result = recv_file_body(conn, temp, info.size);
        if (result == TransferResult::ConnectionLost) {
            return false;
        }
        bool ok = result == TransferResult::Ok && is_valid_username(user) &&
                  is_valid_path(info.name);
        if (ok) {
            std::lock_guard<std::mutex> write(write_mutex_);
            ok = storage_.commit(user, info.name, temp, info.mtime_ns);
        }
        if (!ok) {
            remove_file(temp);
            return send_error(conn, "could not store the replicated file");
        }
        LOG_INFO("replicated " << user << "/" << info.name << " (" << format_size(info.size) << ")");
        return conn.send(Msg::Ok);
    }
    case Msg::ReplDelete: {
        Reader r(msg.payload);
        std::string user = r.str();
        std::string name = r.str();
        if (is_valid_username(user) && is_valid_path(name)) {
            std::lock_guard<std::mutex> write(write_mutex_);
            if (storage_.remove(user, name)) {
                LOG_INFO("replicated delete of " << user << "/" << name);
            }
        }
        return conn.send(Msg::Ok);
    }
    case Msg::Membership: {
        Reader r(msg.payload);
        std::vector<ServerAddress> members = read_servers(r);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            members_ = members;
            upsert(members_, self_);
        }
        notify_membership();
        return conn.send(Msg::Ok);
    }
    case Msg::ReplAccount: {
        Reader r(msg.payload);
        std::string user = r.str();
        std::string record = r.str();
        if (!is_valid_username(user) || !Accounts::is_valid_record(record) || !accounts_.put(user, record)) {
            return send_error(conn, "could not store the replicated account");
        }
        LOG_INFO("replicated the account of " << user);
        return conn.send(Msg::Ok);
    }
    case Msg::Heartbeat:
        return conn.send(Msg::Ok);
    default:
        LOG_WARN("unexpected " << msg_name(msg.type) << " on the replica link");
        return false;
    }
}

std::string Cluster::build_manifest()
{
    std::vector<std::pair<std::string, FileInfo>> entries;
    for (const std::string& user : storage_.users()) {
        for (const FileInfo& file : storage_.list(user)) {
            entries.emplace_back(user, file);
        }
    }
    std::vector<std::pair<std::string, std::string>> accounts = accounts_.all();
    Writer w;
    w.u32(static_cast<uint32_t>(accounts.size()));
    for (const auto& account : accounts) {
        w.str(account.first).str(account.second);
    }
    w.u32(static_cast<uint32_t>(entries.size()));
    for (const auto& entry : entries) {
        w.str(entry.first);
        write_file_info(w, entry.second);
    }
    return w.data();
}

// -------------------------------------------------------------------- election

void Cluster::monitor_loop()
{
    while (!stopping_) {
        std::this_thread::sleep_for(HEARTBEAT_INTERVAL);
        Role role;
        bool primary_silent;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            role = role_;
            primary_silent = role_ == Role::Backup && !building_manifest_ &&
                             Clock::now() - last_heard_ > PRIMARY_TIMEOUT;
        }
        if (role == Role::Primary) {
            heartbeat_links();
        } else if (primary_silent && !electing_) {
            LOG_WARN("no word from the primary; starting an election");
            start_election();
        }
    }
}

void Cluster::start_election()
{
    bool expected = false;
    if (!electing_.compare_exchange_strong(expected, true)) {
        return;
    }
    std::thread([this] {
        run_election();
        electing_ = false;
    }).detach();
}

void Cluster::run_election()
{
    while (!stopping_) {
        ServerAddress me;
        std::vector<ServerAddress> higher;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (role_ == Role::Primary) {
                return;
            }
            me = self_;
            primary_known_ = false; // don't redirect clients to a primary we think is dead
            coordinator_seen_ = false;
            for (const ServerAddress& member : members_) {
                if (member.id > me.id) {
                    higher.push_back(member);
                }
            }
        }

        bool higher_alive = false;
        for (const ServerAddress& member : higher) {
            ConnectionPtr conn = connect_to(member.host, member.port, CONNECT_TIMEOUT_MS);
            if (!conn) {
                continue;
            }
            Writer w;
            write_server(w, me);
            Message reply;
            if (call(*conn, Msg::Election, w.data(), Msg::Ok, reply, HEARTBEAT_TIMEOUT_MS) ==
                CallResult::Ok) {
                LOG_INFO(describe(member) << " outranks this server and will take over");
                higher_alive = true;
            }
        }
        if (!higher_alive) {
            become_primary();
            return;
        }

        bool announced;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            announced = coordinator_cv_.wait_for(lock, COORDINATOR_WAIT, [this] {
                return coordinator_seen_ || stopping_.load();
            });
        }
        if (announced) {
            return;
        }
        LOG_WARN("no new primary announced itself; restarting the election");
    }
}

void Cluster::become_primary()
{
    std::vector<ServerAddress> others;
    ConnectionPtr stale_primary_link;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint32_t old_primary = primary_.id;
        role_ = Role::Primary;
        primary_ = self_;
        primary_known_ = true;
        if (old_primary != self_.id) {
            members_.erase(std::remove_if(members_.begin(), members_.end(),
                                          [old_primary](const ServerAddress& m) {
                                              return m.id == old_primary;
                                          }),
                           members_.end());
        }
        lowest_id_ = self_.id;
        for (const ServerAddress& member : members_) {
            lowest_id_ = std::min(lowest_id_, member.id);
            if (member.id != self_.id) {
                others.push_back(member);
            }
        }
        stale_primary_link = primary_link_;
        primary_link_.reset();
    }
    if (stale_primary_link) {
        stale_primary_link->shutdown();
    }
    LOG_INFO("this server is now the primary " << describe(self()));

    for (const ServerAddress& member : others) {
        announce_to(member);
    }
    for (const ServerAddress& member : others) {
        if (!attach_replica(member)) {
            LOG_WARN("could not reach backup " << describe(member) << "; removing it");
            remove_member(member.id);
        }
    }
    broadcast_membership();
}

void Cluster::announce_to(const ServerAddress& member)
{
    ConnectionPtr conn = connect_to(member.host, member.port, CONNECT_TIMEOUT_MS);
    if (!conn) {
        return;
    }
    Writer w;
    write_server(w, self());
    Message reply;
    call(*conn, Msg::Coordinator, w.data(), Msg::Ok, reply, HEARTBEAT_TIMEOUT_MS);
}

void Cluster::handle_election(const ConnectionPtr& conn, const Message& first)
{
    Reader r(first.payload);
    ServerAddress candidate = read_server(r);
    conn->send(Msg::Ok);

    if (!is_primary()) {
        start_election();
        return;
    }
    // A backup suspects us, but we're alive: tell it so, and make sure it has
    // a working replica link (its old one may be what broke).
    LOG_INFO(describe(candidate) << " suspects the primary; reassuring it");
    std::thread([this, candidate] {
        announce_to(candidate);
        bool linked;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            linked = links_.count(candidate.id) > 0;
        }
        if (!linked) {
            add_member(candidate);
            if (attach_replica(candidate)) {
                broadcast_membership();
            } else {
                remove_member(candidate.id);
            }
        }
    }).detach();
}

// Tells another server which cluster this one belongs to (see HealthMonitor).
void Cluster::handle_status(const ConnectionPtr& conn)
{
    Writer w;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t role = role_ == Role::Primary ? 2 : role_ == Role::Backup ? 1 : 0;
        w.u8(role).u8(primary_known_ ? 1 : 0);
        write_server(w, primary_known_ ? primary_ : ServerAddress{});
    }
    conn->send(Msg::Status, w.data());
}

void Cluster::handle_coordinator(const ConnectionPtr& conn, const Message& first)
{
    Reader r(first.payload);
    ServerAddress leader = read_server(r);
    conn->send(Msg::Ok);

    if (leader.id < self().id) {
        LOG_INFO(describe(leader) << " claims to be the primary, but this server outranks it");
        start_election();
        return;
    }

    std::vector<LinkPtr> stale_links;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (role_ == Role::Primary) {
            for (const auto& entry : links_) {
                stale_links.push_back(entry.second);
            }
            links_.clear();
        }
        role_ = Role::Backup;
        primary_ = leader;
        primary_known_ = true;
        coordinator_seen_ = true;
        last_heard_ = Clock::now() + ATTACH_GRACE; // its replica link comes next
        upsert(members_, leader);
    }
    coordinator_cv_.notify_all();
    for (const LinkPtr& link : stale_links) {
        link->conn->shutdown();
    }
    LOG_INFO("the new primary is " << describe(leader));
}
