#include "files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr int BODY_TIMEOUT_MS = 15000;

class Fnv1a {
public:
    void update(const char* data, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i) {
            hash_ ^= static_cast<unsigned char>(data[i]);
            hash_ *= 1099511628211ULL;
        }
    }
    uint64_t value() const { return hash_; }

private:
    uint64_t hash_ = 1469598103934665603ULL;
};

bool read_fully(int fd, char* buffer, std::size_t size)
{
    std::size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buffer + got, size - got);
        if (n > 0) {
            got += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool write_fully(int fd, const char* data, std::size_t size)
{
    std::size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, data + done, size - done);
        if (n > 0) {
            done += static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int64_t mtime_of(const struct stat& st)
{
    return static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
}

bool hash_fd(int fd, uint64_t size, uint64_t& hash)
{
    Fnv1a fnv;
    std::string buffer(FILE_CHUNK_SIZE, '\0');
    uint64_t remaining = size;
    while (remaining > 0) {
        std::size_t chunk = static_cast<std::size_t>(std::min<uint64_t>(remaining, buffer.size()));
        if (!read_fully(fd, &buffer[0], chunk)) {
            return false;
        }
        fnv.update(buffer.data(), chunk);
        remaining -= chunk;
    }
    hash = fnv.value();
    return true;
}

} // namespace

bool is_valid_file_name(const std::string& name)
{
    if (name.empty() || name.size() > MAX_NAME_LENGTH || name == "." || name == "..") {
        return false;
    }
    if (name.compare(0, std::string(RESERVED_PREFIX).size(), RESERVED_PREFIX) == 0) {
        return false;
    }
    for (unsigned char c : name) {
        if (c == '/' || c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool is_valid_path(const std::string& path)
{
    if (path.empty() || path.size() > MAX_PATH_LENGTH) {
        return false;
    }
    std::size_t depth = 0;
    std::size_t start = 0;
    while (true) {
        std::size_t slash = path.find('/', start);
        std::string component = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!is_valid_file_name(component) || ++depth > MAX_PATH_DEPTH) {
            return false;
        }
        if (slash == std::string::npos) {
            return true;
        }
        start = slash + 1;
    }
}

bool is_valid_username(const std::string& name)
{
    if (name.empty() || name.size() > MAX_USERNAME_LENGTH) {
        return false;
    }
    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string join_path(const std::string& dir, const std::string& name)
{
    if (dir.empty() || dir.back() == '/') {
        return dir + name;
    }
    return dir + "/" + name;
}

std::string parent_path(const std::string& path)
{
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

std::string base_name(const std::string& path)
{
    std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool make_parent_dirs(const std::string& root, const std::string& relative_path)
{
    std::string parent = parent_path(relative_path);
    return parent.empty() || make_dirs(join_path(root, parent));
}

void remove_empty_parents(const std::string& root, const std::string& relative_path)
{
    // rmdir only removes empty directories, so stop at the first that isn't.
    for (std::string dir = parent_path(relative_path); !dir.empty(); dir = parent_path(dir)) {
        if (rmdir(join_path(root, dir).c_str()) != 0) {
            return;
        }
    }
}

bool make_dirs(const std::string& path)
{
    std::string partial;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            slash = path.size();
        }
        partial = path.substr(0, slash);
        if (!partial.empty() && mkdir(partial.c_str(), 0755) < 0 && errno != EEXIST) {
            return false;
        }
        start = slash + 1;
    }
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool stat_file(const std::string& path, FileInfo& info, bool with_hash)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok) {
        info.size = static_cast<uint64_t>(st.st_size);
        info.mtime_ns = mtime_of(st);
        info.hash = 0;
        if (with_hash) {
            ok = hash_fd(fd, info.size, info.hash);
        }
    }
    close(fd);
    return ok;
}

bool hash_file(const std::string& path, uint64_t& hash)
{
    FileInfo info;
    if (!stat_file(path, info, true)) {
        return false;
    }
    hash = info.hash;
    return true;
}

namespace {

void walk(const std::string& root, const std::string& relative_dir, std::size_t depth,
          std::vector<FileInfo>& files)
{
    DIR* d = opendir(join_path(root, relative_dir).c_str());
    if (d == nullptr) {
        return;
    }
    while (dirent* entry = readdir(d)) {
        std::string name = entry->d_name;
        if (!is_valid_file_name(name)) {
            continue; // ".", "..", reserved names
        }
        std::string relative = relative_dir.empty() ? name : relative_dir + "/" + name;
        struct stat st{};
        if (lstat(join_path(root, relative).c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode) && depth + 1 < MAX_PATH_DEPTH) {
            walk(root, relative, depth + 1, files);
        } else if (S_ISREG(st.st_mode) && is_valid_path(relative)) {
            FileInfo info;
            info.name = relative;
            info.size = static_cast<uint64_t>(st.st_size);
            info.mtime_ns = mtime_of(st);
            files.push_back(info);
        }
        // Symlinks and special files are skipped.
    }
    closedir(d);
}

} // namespace

std::vector<FileInfo> list_tree(const std::string& root)
{
    std::vector<FileInfo> files;
    walk(root, "", 0, files);
    std::sort(files.begin(), files.end(),
              [](const FileInfo& a, const FileInfo& b) { return a.name < b.name; });
    return files;
}

bool set_mtime(const std::string& path, int64_t mtime_ns)
{
    timespec times[2];
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT; // leave the access time alone
    times[1].tv_sec = static_cast<time_t>(mtime_ns / 1000000000LL);
    times[1].tv_nsec = static_cast<long>(mtime_ns % 1000000000LL);
    return utimensat(AT_FDCWD, path.c_str(), times, 0) == 0;
}

bool remove_file(const std::string& path)
{
    return unlink(path.c_str()) == 0;
}

bool replace_file(const std::string& from, const std::string& to)
{
    return rename(from.c_str(), to.c_str()) == 0;
}

std::string make_temp_path(const std::string& dir)
{
    static thread_local std::mt19937_64 rng(std::random_device{}());
    char suffix[20];
    std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(rng()));
    return join_path(dir, std::string(RESERVED_PREFIX) + "_tmp_" + suffix);
}

std::string format_size(uint64_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 3) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return text;
}

std::string format_time(int64_t mtime_ns)
{
    std::time_t seconds = static_cast<std::time_t>(mtime_ns / 1000000000LL);
    std::tm local{};
    localtime_r(&seconds, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

void write_file_header(Writer& w, const FileInfo& info)
{
    w.str(info.name).u64(info.size).i64(info.mtime_ns);
}

FileInfo read_file_header(Reader& r)
{
    FileInfo info;
    info.name = r.str();
    info.size = r.u64();
    info.mtime_ns = r.i64();
    return info;
}

void write_file_info(Writer& w, const FileInfo& info)
{
    write_file_header(w, info);
    w.u64(info.hash);
}

FileInfo read_file_info(Reader& r)
{
    FileInfo info = read_file_header(r);
    info.hash = r.u64();
    return info;
}

TransferResult send_file(Connection& conn, Msg type, const std::string& prefix,
                         const std::string& name, const std::string& path, FileInfo* sent)
{
    auto lock = conn.lock_send();

    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return TransferResult::NotSent;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return TransferResult::NotSent;
    }

    FileInfo info;
    info.name = name;
    info.size = static_cast<uint64_t>(st.st_size);
    info.mtime_ns = mtime_of(st);
    if (sent != nullptr) {
        *sent = info;
    }
    Writer header;
    write_file_header(header, info);

    if (!conn.send_locked(type, prefix + header.data())) {
        close(fd);
        return TransferResult::ConnectionLost;
    }

    // Send exactly the announced size, even if the file grows meanwhile. If
    // it shrinks, abort so the receiver discards the partial copy.
    std::string buffer(FILE_CHUNK_SIZE, '\0');
    uint64_t remaining = info.size;
    TransferResult result = TransferResult::Ok;
    while (remaining > 0) {
        std::size_t chunk = static_cast<std::size_t>(std::min<uint64_t>(remaining, buffer.size()));
        if (!read_fully(fd, &buffer[0], chunk)) {
            result = TransferResult::FileError;
            break;
        }
        if (!conn.send_locked(Msg::FileData, buffer.substr(0, chunk))) {
            close(fd);
            return TransferResult::ConnectionLost;
        }
        remaining -= chunk;
    }
    close(fd);

    Msg end = result == TransferResult::Ok ? Msg::FileEnd : Msg::FileAbort;
    if (!conn.send_locked(end)) {
        return TransferResult::ConnectionLost;
    }
    return result;
}

TransferResult recv_file_body(Connection& conn, const std::string& dest, uint64_t size,
                              uint64_t* hash)
{
    int fd = open(dest.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool write_ok = fd >= 0;
    bool aborted = false;
    bool broken = false; // the stream can't be trusted any more
    uint64_t received = 0;
    Fnv1a fnv;

    // Keep reading even if the file can't be written, so the stream stays in sync.
    while (true) {
        Message msg;
        if (conn.recv(msg, BODY_TIMEOUT_MS) != RecvStatus::Ok) {
            broken = true;
            break;
        }
        if (msg.type == Msg::FileData) {
            if (size - received < msg.payload.size()) {
                broken = true; // more data than announced
                break;
            }
            if (write_ok) {
                write_ok = write_fully(fd, msg.payload.data(), msg.payload.size());
            }
            fnv.update(msg.payload.data(), msg.payload.size());
            received += msg.payload.size();
        } else if (msg.type == Msg::FileEnd) {
            broken = received != size;
            break;
        } else if (msg.type == Msg::FileAbort) {
            aborted = true;
            break;
        } else {
            broken = true;
            break;
        }
    }

    if (fd >= 0) {
        close(fd);
    }
    if (broken) {
        unlink(dest.c_str());
        return TransferResult::ConnectionLost;
    }
    if (!write_ok || aborted) {
        unlink(dest.c_str());
        return TransferResult::FileError;
    }
    if (hash != nullptr) {
        *hash = fnv.value();
    }
    return TransferResult::Ok;
}
