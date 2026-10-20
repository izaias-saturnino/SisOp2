#pragma once

#include "../common/files.hpp"

#include <map>
#include <string>
#include <vector>

// The client's record of each file as it was when last in sync with the
// server (size, mtime, content hash). It is the "base" of a three-way
// comparison: a file that differs from its base changed on that side since
// the last sync. That's how the client tells a file deleted here while
// offline (upload the deletion) from one created elsewhere (download it),
// and how it ignores filesystem events caused by its own downloads.
//
// Stored as a small text file inside sync_dir under the reserved prefix, so
// it is never synced itself. Not thread-safe: the client guards it with its
// sync mutex.
class SyncState {
public:
    explicit SyncState(std::string path);

    void load();
    bool save() const;

    bool get(const std::string& name, FileInfo& info) const;
    void set(const FileInfo& info);
    void erase(const std::string& name);
    std::vector<std::string> names() const;

private:
    std::string path_;
    std::map<std::string, FileInfo> entries_;
};
