#pragma once

#include "cluster.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

// Watches for expected servers that aren't part of the cluster (--expect).
//
// The cluster tolerates crashes but not network splits; for a cluster run
// locally it's more useful to notice a problem right away than to handle it
// automatically. Every two seconds, each expected server that isn't a
// member is asked which cluster it's in, which tells apart:
//
//  - not running (or unreachable);
//  - running as a separate primary: a network split, or started without --join;
//  - following a different primary: the cluster is split.
//
// Changes are logged at once, problems are logged again every 30 s while they
// last, and every change is passed to `notify` (the primary forwards them to
// its clients).
class HealthMonitor {
public:
    using Notify = std::function<void(const std::string& text)>;

    HealthMonitor(Cluster& cluster, std::vector<Endpoint> expected, Notify notify);
    ~HealthMonitor();

    void start();
    // The current problems, one per line; empty when everything is fine.
    std::string problems() const;

private:
    enum class State { Unknown, Up, Down, Separate, Split };

    void loop();
    void check();
    State probe(const Endpoint& server, std::string& detail);
    bool same(const std::string& host_a, uint16_t port_a, const std::string& host_b, uint16_t port_b);
    std::string resolve(const std::string& host);

    Cluster& cluster_;
    std::vector<Endpoint> expected_;
    Notify notify_;

    mutable std::mutex mutex_;
    std::map<std::string, State> states_;         // by "host:port"
    std::map<std::string, std::string> messages_; // current problem per server
    std::map<std::string, std::string> resolved_; // host -> IPv4 (health thread only)

    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
