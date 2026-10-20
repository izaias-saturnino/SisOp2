#pragma once

#include "connection.hpp"

#include <cstdint>
#include <string>
#include <vector>

// File names starting with this prefix are reserved for the program's own
// files (temporary downloads, the client's sync state) and are never synced.
constexpr const char* RESERVED_PREFIX = ".sisop";
constexpr std::size_t MAX_NAME_LENGTH = 255;
constexpr std::size_t MAX_USERNAME_LENGTH = 32;

struct FileInfo {
    std::string name;
    uint64_t size = 0;
    int64_t mtime_ns = 0; // modification time, nanoseconds since the epoch
    uint64_t hash = 0;    // FNV-1a of the contents (0 when not computed)
};

// A file name is a single path component: not empty, not "." or "..", no
// '/', no control characters, not reserved. This is what keeps clients from
// reaching outside their own directory.
bool is_valid_file_name(const std::string& name);
// Usernames become directory names: letters, digits, '_' and '-' only.
bool is_valid_username(const std::string& name);

std::string join_path(const std::string& dir, const std::string& name);
bool make_dirs(const std::string& path);
// Fills `info` for a regular file (symlinks and directories are rejected).
bool stat_file(const std::string& path, FileInfo& info, bool with_hash);
// The regular files in `dir` whose names are valid, sorted by name.
std::vector<FileInfo> list_dir(const std::string& dir, bool with_hash);
bool set_mtime(const std::string& path, int64_t mtime_ns);
// True if the file existed and was removed.
bool remove_file(const std::string& path);
// Atomically moves `from` over `to` (both must be on the same filesystem).
bool replace_file(const std::string& from, const std::string& to);
// A fresh path for a temporary file inside `dir`, using the reserved prefix.
std::string make_temp_path(const std::string& dir);

std::string format_size(uint64_t bytes);
std::string format_time(int64_t mtime_ns);

// File header: str name, u64 size, i64 mtime. File info adds u64 hash.
void write_file_header(Writer& w, const FileInfo& info);
FileInfo read_file_header(Reader& r);
void write_file_info(Writer& w, const FileInfo& info);
FileInfo read_file_info(Reader& r);

enum class TransferResult {
    Ok,
    NotSent,        // the file couldn't be opened; nothing was sent
    FileError,      // the file couldn't be read or written; the connection is still in sync
    ConnectionLost, // the connection failed; stop using it
};

// Sends a `type` message whose payload is `prefix` followed by the file
// header, then the body as FileData messages and FileEnd. Holds the
// connection's send lock for the whole transfer, and opens the file only once
// it has the lock: when several threads push the same file, the last one to
// send also sends the newest contents. If the file can't be read to the end,
// FileAbort ends the body (FileError). `sent` (optional) receives the
// size/mtime that were announced.
TransferResult send_file(Connection& conn, Msg type, const std::string& prefix,
                         const std::string& name, const std::string& path, FileInfo* sent = nullptr);

// Receives a body of exactly `size` bytes into `dest`. On FileError the
// sender aborted or `dest` couldn't be written: `dest` is removed but the
// connection is still in sync. `hash` (optional) receives the content hash.
TransferResult recv_file_body(Connection& conn, const std::string& dest, uint64_t size,
                              uint64_t* hash = nullptr);
