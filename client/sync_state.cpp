#include "sync_state.hpp"

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

const char* const HEADER = "sisop-sync-state 1";

} // namespace

SyncState::SyncState(std::string path) : path_(std::move(path))
{
}

// Format: a header line, then one line per file:
//   <hash, 16 hex digits> <size> <mtime_ns> <name>
// The name goes last so it may contain spaces (valid names have no newlines).
void SyncState::load()
{
    entries_.clear();
    std::ifstream in(path_);
    std::string line;
    if (!std::getline(in, line) || line != HEADER) {
        return; // missing or unknown: start with no base, nothing gets deleted
    }
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string hash_hex;
        FileInfo info;
        if (!(fields >> hash_hex >> info.size >> info.mtime_ns)) {
            continue;
        }
        fields.get(); // the space before the name
        std::getline(fields, info.name);
        if (!is_valid_file_name(info.name)) {
            continue;
        }
        info.hash = std::stoull(hash_hex, nullptr, 16);
        entries_[info.name] = info;
    }
}

bool SyncState::save() const
{
    std::string temp = path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        out << HEADER << "\n";
        for (const auto& entry : entries_) {
            const FileInfo& info = entry.second;
            char hash_hex[17];
            std::snprintf(hash_hex, sizeof(hash_hex), "%016" PRIx64, info.hash);
            out << hash_hex << ' ' << info.size << ' ' << info.mtime_ns << ' ' << info.name << "\n";
        }
        if (!out.flush()) {
            return false;
        }
    }
    return replace_file(temp, path_);
}

bool SyncState::get(const std::string& name, FileInfo& info) const
{
    auto it = entries_.find(name);
    if (it == entries_.end()) {
        return false;
    }
    info = it->second;
    return true;
}

void SyncState::set(const FileInfo& info)
{
    entries_[info.name] = info;
}

void SyncState::erase(const std::string& name)
{
    entries_.erase(name);
}

std::vector<std::string> SyncState::names() const
{
    std::vector<std::string> names;
    for (const auto& entry : entries_) {
        names.push_back(entry.first);
    }
    return names;
}
