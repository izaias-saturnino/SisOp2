#pragma once

#include "../common/connection.hpp"
#include "../common/protocol.hpp"
#include "accounts.hpp"
#include "storage.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Primary-backup replication with bully leader election.
//
// One server is the primary. It serves every client, and for each change it
// applies the change locally and copies it to every backup over a persistent
// "replica link" before acknowledging the client. Backups only apply what the
// primary sends them, and redirect clients to the primary.
//
// Joining: a new server asks any member (which redirects it to the primary)
// for an id. The primary then opens a replica link to it, compares file
// lists, sends whatever is missing or different, and announces the new
// member to everyone (servers and clients).
//
// Failure detection: the primary sends a heartbeat on each link every
// 500 ms. A backup that hears nothing from the primary for 3 s starts an
// election; a backup whose link fails is dropped by the primary.
//
// Election (bully): the candidate contacts every member with a higher id. If
// none answers, it becomes the primary, announces itself (Coordinator) and
// opens replica links to everyone, which also brings every backup up to date
// with its files. If a higher one answers, that one takes over the election.
//
// Ids: the first server gets FIRST_ID and each new member gets a lower id
// than everyone so far. That keeps the bully invariant "the primary has the
// highest id": a backup that wrongly suspects a live primary always asks it
// during the election, so no second primary gets elected. When the primary
// does die, the backup that joined earliest takes over.
//
// Locking order: the server's write mutex, then a link's transaction mutex,
// then mutex_. Nothing does network I/O while holding mutex_.
class Cluster {
public:
    using MembershipListener = std::function<void(const std::vector<ServerAddress>&)>;

    Cluster(std::string host, uint16_t port, Storage& storage, Accounts& accounts,
            std::mutex& write_mutex);
    ~Cluster();

    void set_membership_listener(MembershipListener listener);
    void start_as_primary();
    // Blocks until the cluster at host:port accepts this server as a backup.
    bool join(const std::string& host, uint16_t port);
    void start_monitor();

    bool is_primary() const;
    bool primary_address(ServerAddress& out) const;
    std::vector<ServerAddress> members() const;
    ServerAddress self() const;

    // Connections whose first message is a cluster message end up here.
    void handle_join(const ConnectionPtr& conn, const Message& first);
    void handle_election(const ConnectionPtr& conn, const Message& first);
    void handle_coordinator(const ConnectionPtr& conn, const Message& first);
    void handle_replica_attach(const ConnectionPtr& conn, const Message& first);
    void handle_status(const ConnectionPtr& conn);

    // Primary only. The caller must hold the write mutex, so backups apply
    // changes in the same order as the primary.
    void replicate_put(const std::string& user, const std::string& name);
    void replicate_delete(const std::string& user, const std::string& name);
    void replicate_account(const std::string& user, const std::string& record);

private:
    enum class Role { Joining, Backup, Primary };

    struct Link {
        ServerAddress member;
        ConnectionPtr conn;
        std::mutex txn; // one request/reply exchange at a time
    };
    using LinkPtr = std::shared_ptr<Link>;
    using Clock = std::chrono::steady_clock;

    // Primary side.
    bool attach_replica(const ServerAddress& member);
    bool put_on_link(Link& link, const std::string& user, const std::string& name);
    bool delete_on_link(Link& link, const std::string& user, const std::string& name);
    bool account_on_link(Link& link, const std::string& user, const std::string& record);
    void drop_link(uint32_t id, const std::string& reason);
    void broadcast_membership();
    std::vector<LinkPtr> links_snapshot();
    void heartbeat_links();

    // Backup side.
    bool apply_replicated(Connection& conn, const Message& msg);
    std::string build_manifest();

    // Election.
    void start_election();
    void run_election();
    void become_primary();
    void announce_to(const ServerAddress& member);

    void monitor_loop();
    void notify_membership();
    void add_member(const ServerAddress& member);
    void remove_member(uint32_t id);

    Storage& storage_;
    Accounts& accounts_;
    std::mutex& write_mutex_;

    mutable std::mutex mutex_;
    std::condition_variable coordinator_cv_;
    ServerAddress self_;
    Role role_ = Role::Joining;
    ServerAddress primary_;
    bool primary_known_ = false;
    std::vector<ServerAddress> members_; // includes this server
    uint32_t lowest_id_ = 0;             // primary: ids are handed out downwards from here
    std::map<uint32_t, LinkPtr> links_;  // primary: one per backup
    ConnectionPtr primary_link_;         // backup: the link from the primary
    Clock::time_point last_heard_;       // backup: last message from the primary
    bool building_manifest_ = false;     // backup: don't suspect the primary meanwhile
    bool coordinator_seen_ = false;
    MembershipListener listener_;

    std::atomic<bool> electing_{false};
    std::atomic<bool> stopping_{false};
    std::thread monitor_;
};
