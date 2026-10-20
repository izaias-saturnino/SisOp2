#include "sessions.hpp"

#include <openssl/rand.h>

namespace {

std::string random_token()
{
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
        return "";
    }
    static const char digits[] = "0123456789abcdef";
    std::string token;
    for (unsigned char b : bytes) {
        token += digits[b >> 4];
        token += digits[b & 0xF];
    }
    return token;
}

} // namespace

std::string SessionManager::open(const std::string& user, ConnectionPtr command)
{
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& entry : sessions_) {
        if (entry.second.user == user) {
            ++count;
        }
    }
    if (count >= MAX_SESSIONS_PER_USER) {
        return "";
    }
    std::string token = random_token();
    if (token.empty() || sessions_.count(token) > 0) {
        return "";
    }
    sessions_[token] = Session{user, std::move(command), nullptr};
    return token;
}

void SessionManager::close(const std::string& token)
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

bool SessionManager::attach_sync(const std::string& token, ConnectionPtr sync)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end() || it->second.sync) {
        return false;
    }
    it->second.sync = std::move(sync);
    return true;
}

void SessionManager::detach_sync(const std::string& token, const ConnectionPtr& sync)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it != sessions_.end() && it->second.sync == sync) {
        it->second.sync.reset();
    }
}

std::vector<ConnectionPtr> SessionManager::sync_connections(const std::string& user,
                                                            const std::string& except)
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
