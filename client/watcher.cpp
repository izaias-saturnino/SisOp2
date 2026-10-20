#include "watcher.hpp"

#include "../common/files.hpp"
#include "../common/log.hpp"

#include <cerrno>
#include <climits>
#include <dirent.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// IN_CLOSE_WRITE rather than IN_MODIFY: report a file once it has been
// written and closed, not while it's still half-written. IN_CREATE is only
// used for directories.
constexpr uint32_t WATCH_MASK =
    IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_CREATE | IN_DELETE_SELF | IN_MOVE_SELF;

bool is_inside(const std::string& path, const std::string& dir)
{
    return path == dir || path.compare(0, dir.size() + 1, dir + "/") == 0;
}

} // namespace

Watcher::Watcher(std::string root, Callback on_change)
    : root_(std::move(root)), on_change_(std::move(on_change))
{
}

Watcher::~Watcher()
{
    stop();
}

bool Watcher::start()
{
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }
    root_wd_ = inotify_add_watch(fd_, root_.c_str(), WATCH_MASK);
    if (root_wd_ < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
    dirs_[root_wd_] = "";
    watch_tree("");
    thread_ = std::thread(&Watcher::loop, this);
    return true;
}

void Watcher::stop()
{
    stopping_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

// Adds a watch for every directory below `relative_dir` (and for it, unless
// it's the root). Watching the same directory again returns its existing
// watch descriptor, whose path is then updated: that's how renamed
// directories keep reporting the right paths.
void Watcher::watch_tree(const std::string& relative_dir)
{
    if (!relative_dir.empty()) {
        int wd = inotify_add_watch(fd_, join_path(root_, relative_dir).c_str(), WATCH_MASK | IN_ONLYDIR);
        if (wd < 0) {
            return;
        }
        dirs_[wd] = relative_dir;
    }
    DIR* d = opendir(join_path(root_, relative_dir).c_str());
    if (d == nullptr) {
        return;
    }
    while (dirent* entry = readdir(d)) {
        std::string name = entry->d_name;
        if (!is_valid_file_name(name)) {
            continue;
        }
        std::string child = relative_dir.empty() ? name : relative_dir + "/" + name;
        struct stat st{};
        if (lstat(join_path(root_, child).c_str(), &st) == 0 && S_ISDIR(st.st_mode) && is_valid_path(child)) {
            watch_tree(child);
        }
    }
    closedir(d);
}

// Stops watching a directory that left the tree (moved out or deleted).
void Watcher::unwatch_tree(const std::string& relative_dir)
{
    for (auto it = dirs_.begin(); it != dirs_.end();) {
        if (it->first != root_wd_ && is_inside(it->second, relative_dir)) {
            inotify_rm_watch(fd_, it->first);
            it = dirs_.erase(it);
        } else {
            ++it;
        }
    }
}

void Watcher::loop()
{
    alignas(inotify_event) char buffer[64 * (sizeof(inotify_event) + NAME_MAX + 1)];
    while (!stopping_) {
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) {
            continue;
        }
        ssize_t length = read(fd_, buffer, sizeof(buffer));
        if (length <= 0) {
            continue;
        }
        for (ssize_t offset = 0; offset < length;) {
            const inotify_event* event = reinterpret_cast<const inotify_event*>(buffer + offset);
            offset += sizeof(inotify_event) + event->len;

            if (event->mask & IN_Q_OVERFLOW) {
                on_change_("");
                continue;
            }
            if (event->wd == root_wd_ && (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))) {
                LOG_ERROR("the sync directory " << root_ << " was moved or removed; changes are no longer watched");
                return;
            }
            if (event->mask & IN_IGNORED) {
                dirs_.erase(event->wd); // a watched directory went away
                continue;
            }
            auto dir = dirs_.find(event->wd);
            if (dir == dirs_.end() || event->len == 0) {
                continue;
            }
            std::string name = event->name;
            std::string path = dir->second.empty() ? name : dir->second + "/" + name;
            if (!is_valid_path(path)) {
                continue;
            }

            if (event->mask & IN_ISDIR) {
                if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
                    watch_tree(path);
                } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                    unwatch_tree(path);
                }
                // Files in it may have appeared or vanished without events of their own.
                on_change_("");
            } else if (!(event->mask & IN_CREATE)) {
                on_change_(path);
            }
        }
    }
}
