#pragma once

#include "../common/connection.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

// The logged-in client sessions on this server. A session is created by a
// login on the command connection and identified by a random token, which
// the client then uses to attach its sync connection (the one the server
// pushes changes on).
class SessionManager {
public:
    static constexpr int MAX_SESSIONS_PER_USER = 2;

    // Returns the new session's token, or 0 if the user already has the
    // maximum number of sessions.
    uint64_t open(const std::string& user, ConnectionPtr command);
    // Ends a session and shuts down both of its connections.
    void close(uint64_t token);

    bool attach_sync(uint64_t token, ConnectionPtr sync);
    void detach_sync(uint64_t token, const ConnectionPtr& sync);

    // Sync connections of the user's sessions, except the one with token
    // `except` (0 to include all of them).
    std::vector<ConnectionPtr> sync_connections(const std::string& user, uint64_t except);
    std::vector<ConnectionPtr> all_sync_connections();

private:
    struct Session {
        std::string user;
        ConnectionPtr command;
        ConnectionPtr sync;
    };

    std::mutex mutex_;
    std::map<uint64_t, Session> sessions_;
    std::mt19937_64 rng_{std::random_device{}()};
};
