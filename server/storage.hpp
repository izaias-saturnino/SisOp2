#pragma once

#include "../common/files.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

// A server's copy of every user's files, kept in <root>/users/<user>/<path>
// (paths may contain subdirectories). Incoming transfers are written to
// <root>/tmp first and moved into place only once complete, so a
// half-received file never shows up in a listing.
//
// The server serializes changes (and their replication) with its write mutex;
// readers only ever see whole files thanks to the atomic rename.
class Storage {
public:
    explicit Storage(std::string root);

    bool init();
    const std::string& root() const { return root_; }
    std::string new_temp_path() const;
    std::string file_path(const std::string& user, const std::string& path) const;
    std::string accounts_dir() const;

    bool ensure_user(const std::string& user);
    std::vector<std::string> users() const;
    // Every file of the user, with content hashes. Hashes are cached and only
    // recomputed when a file's size or modification time changes.
    std::vector<FileInfo> list(const std::string& user) const;

    // Moves a fully received temp file into place with the given mtime,
    // creating parent directories as needed.
    bool commit(const std::string& user, const std::string& path, const std::string& temp,
                int64_t mtime_ns);
    // True if the file existed. Directories left empty are removed.
    bool remove(const std::string& user, const std::string& path);

private:
    std::string user_dir(const std::string& user) const;

    std::string root_;
    std::string users_dir_;
    std::string tmp_dir_;

    mutable std::mutex cache_mutex_;
    mutable std::map<std::string, FileInfo> hash_cache_; // by full path
};
