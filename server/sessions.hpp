#pragma once

#include "../common/connection.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

// The logged-in client sessions on this server. A session is created by a
// login on the command connection and identified by a random token (128
// bits from OpenSSL's CSPRNG), which the client then uses to attach its sync
// connection (the one the server pushes changes on).
class SessionManager {
public:
    static constexpr int MAX_SESSIONS_PER_USER = 2;

    // Returns the new session's token, or "" if the user already has the
    // maximum number of sessions.
    std::string open(const std::string& user, ConnectionPtr command);
    // Ends a session and shuts down both of its connections.
    void close(const std::string& token);

    bool attach_sync(const std::string& token, ConnectionPtr sync);
    void detach_sync(const std::string& token, const ConnectionPtr& sync);

    // Sync connections of the user's sessions, except the one with token
    // `except` ("" to include all of them).
    std::vector<ConnectionPtr> sync_connections(const std::string& user, const std::string& except);
    std::vector<ConnectionPtr> all_sync_connections();

private:
    struct Session {
        std::string user;
        ConnectionPtr command;
        ConnectionPtr sync;
    };

    std::mutex mutex_;
    std::map<std::string, Session> sessions_;
};
