#include "sessions.hpp"

uint64_t SessionManager::open(const std::string& user, ConnectionPtr command)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& entry : sessions_) {
        if (entry.second.user == user) {
            ++count;
        }
    }
    if (count >= MAX_SESSIONS_PER_USER) {
        return 0;
    }
    uint64_t token = 0;
    while (token == 0 || sessions_.count(token) > 0) {
        token = rng_();
    }
    sessions_[token] = Session{user, std::move(command), nullptr};
    return token;
}

void SessionManager::close(uint64_t token)
{
    Session session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(token);
        if (it == sessions_.end()) {
            return;
        }
        session = it->second;
        sessions_.erase(it);
    }
    session.command->shutdown();
    if (session.sync) {
        session.sync->shutdown();
    }
}

bool SessionManager::attach_sync(uint64_t token, ConnectionPtr sync)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end() || it->second.sync) {
        return false;
    }
    it->second.sync = std::move(sync);
    return true;
}

void SessionManager::detach_sync(uint64_t token, const ConnectionPtr& sync)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it != sessions_.end() && it->second.sync == sync) {
        it->second.sync.reset();
    }
}

std::vector<ConnectionPtr> SessionManager::sync_connections(const std::string& user, uint64_t except)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConnectionPtr> result;
    for (const auto& entry : sessions_) {
        if (entry.first != except && entry.second.user == user && entry.second.sync) {
            result.push_back(entry.second.sync);
        }
    }
    return result;
}

std::vector<ConnectionPtr> SessionManager::all_sync_connections()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ConnectionPtr> result;
    for (const auto& entry : sessions_) {
        if (entry.second.sync) {
            result.push_back(entry.second.sync);
        }
    }
    return result;
}
