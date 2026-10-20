#include "health.hpp"

#include "../common/log.hpp"
#include "../common/net.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <netdb.h>

namespace {

constexpr auto CHECK_INTERVAL = std::chrono::seconds(2);
constexpr int REMINDER_EVERY = 15; // checks, i.e. 30 s
constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int REPLY_TIMEOUT_MS = 2000;

std::string key(const Endpoint& server)
{
    return server.host + ":" + std::to_string(server.port);
}

} // namespace

HealthMonitor::HealthMonitor(Cluster& cluster, std::vector<Endpoint> expected, Notify notify)
    : cluster_(cluster), expected_(std::move(expected)), notify_(std::move(notify))
{
}

HealthMonitor::~HealthMonitor()
{
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HealthMonitor::start()
{
    if (!expected_.empty()) {
        thread_ = std::thread(&HealthMonitor::loop, this);
    }
}

std::string HealthMonitor::problems() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& entry : messages_) {
        text += (text.empty() ? "" : "\n") + entry.second;
    }
    return text;
}

void HealthMonitor::loop()
{
    for (int round = 1; !stopping_; ++round) {
        check();
        if (round % REMINDER_EVERY == 0) {
            std::string current = problems();
            if (!current.empty()) {
                LOG_WARN("still missing expected servers:\n" << current);
            }
        }
        for (auto waited = std::chrono::milliseconds(0); waited < CHECK_INTERVAL && !stopping_;
             waited += std::chrono::milliseconds(100)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::string HealthMonitor::resolve(const std::string& host)
{
    auto cached = resolved_.find(host);
    if (cached != resolved_.end()) {
        return cached->second;
    }
    std::string ip = host;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) == 0 && results != nullptr) {
        char text[INET_ADDRSTRLEN];
        const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
        if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) != nullptr) {
            ip = text;
        }
        freeaddrinfo(results);
    }
    resolved_[host] = ip;
    return ip;
}

// "localhost:4000" and "127.0.0.1:4000" are the same server.
bool HealthMonitor::same(const std::string& host_a, uint16_t port_a, const std::string& host_b,
                         uint16_t port_b)
{
    return port_a == port_b && (host_a == host_b || resolve(host_a) == resolve(host_b));
}

HealthMonitor::State HealthMonitor::probe(const Endpoint& server, std::string& detail)
{
    ConnectionPtr conn = connect_to(server.host, server.port, CONNECT_TIMEOUT_MS);
    Message reply;
    if (!conn || call(*conn, Msg::StatusRequest, "", Msg::Status, reply, REPLY_TIMEOUT_MS) != CallResult::Ok) {
        detail = "expected server " + key(server) + " is not running (or can't be reached)";
        return State::Down;
    }
    Reader r(reply.payload);
    uint8_t role = r.u8();
    bool primary_known = r.u8() != 0;
    ServerAddress their_primary = read_server(r);

    ServerAddress our_primary;
    bool ours_known = cluster_.primary_address(our_primary);
    if (role != 2 && primary_known && ours_known &&
        same(their_primary.host, their_primary.port, our_primary.host, our_primary.port)) {
        return State::Up; // in our cluster, just not listed yet (still joining)
    }
    if (role == 2) {
        detail = "expected server " + key(server) +
                 " is running as a separate primary (a network split, or it was started without --join)";
        return State::Separate;
    }
    if (!primary_known) {
        return State::Up; // in the middle of an election; look again next round
    }
    detail = "expected server " + key(server) + " follows a different primary (" + their_primary.host +
             ":" + std::to_string(their_primary.port) + "): the cluster is split";
    return State::Split;
}

void HealthMonitor::check()
{
    ServerAddress me = cluster_.self();
    std::vector<ServerAddress> members = cluster_.members();
    std::vector<std::string> changes;
    bool all_up = true;
    std::size_t total = expected_.size() + 1; // this server, unless it's listed too

    for (const Endpoint& server : expected_) {
        if (same(server.host, server.port, me.host, me.port)) {
            total = expected_.size();
            continue;
        }
        State state = State::Up;
        std::string detail;
        bool member = false;
        for (const ServerAddress& m : members) {
            member = member || same(server.host, server.port, m.host, m.port);
        }
        if (!member) {
            try {
                state = probe(server, detail);
            } catch (const ProtocolError&) {
                state = State::Down;
                detail = "expected server " + key(server) + " sent a malformed status reply";
            }
        }
        all_up = all_up && state == State::Up;

        std::lock_guard<std::mutex> lock(mutex_);
        State& previous = states_[key(server)];
        if (state == previous) {
            continue;
        }
        bool was_known = previous != State::Unknown;
        previous = state;
        if (state == State::Up) {
            messages_.erase(key(server));
            if (was_known) {
                changes.push_back("expected server " + key(server) + " is back in the cluster");
            }
        } else {
            messages_[key(server)] = "warning: " + detail;
            changes.push_back("warning: " + detail);
        }
    }

    for (const std::string& change : changes) {
        if (change.compare(0, 8, "warning:") == 0) {
            LOG_WARN(change.substr(9));
        } else {
            LOG_INFO(change);
        }
        notify_(change);
    }
    if (!changes.empty() && all_up) {
        std::string ok = "all " + std::to_string(total) + " expected servers are up";
        LOG_INFO(ok);
        notify_(ok);
    }
}
