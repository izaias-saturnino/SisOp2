#pragma once

#include "../common/files.hpp"

#include <string>
#include <vector>

// A server's copy of every user's files, kept in <root>/users/<user>/<file>.
// Incoming transfers are written to <root>/tmp first and moved into place only
// once complete, so a half-received file never shows up in a listing.
//
// Storage itself isn't locked: the server serializes changes (and their
// replication) with its write mutex, and readers only ever see whole files
// thanks to the atomic rename.
class Storage {
public:
    explicit Storage(std::string root);

    bool init();
    const std::string& root() const { return root_; }
    std::string new_temp_path() const;
    std::string file_path(const std::string& user, const std::string& name) const;

    bool ensure_user(const std::string& user);
    std::vector<std::string> users() const;
    std::vector<FileInfo> list(const std::string& user, bool with_hash) const;

    // Moves a fully received temp file into place with the given mtime.
    bool commit(const std::string& user, const std::string& name, const std::string& temp,
                int64_t mtime_ns);
    // True if the file existed.
    bool remove(const std::string& user, const std::string& name);

private:
    std::string user_dir(const std::string& user) const;

    std::string root_;
    std::string users_dir_;
    std::string tmp_dir_;
};
