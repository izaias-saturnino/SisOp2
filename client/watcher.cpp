#include "watcher.hpp"

#include "../common/files.hpp"
#include "../common/log.hpp"

#include <cerrno>
#include <climits>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

Watcher::Watcher(std::string dir, Callback on_change)
    : dir_(std::move(dir)), on_change_(std::move(on_change))
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
    // IN_CLOSE_WRITE rather than IN_CREATE/IN_MODIFY: report a file once it
    // has been written and closed, not while it's still half-written.
    uint32_t mask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE | IN_DELETE_SELF;
    if (inotify_add_watch(fd_, dir_.c_str(), mask) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }
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
            } else if (event->mask & (IN_DELETE_SELF | IN_IGNORED)) {
                LOG_ERROR("the sync directory " << dir_ << " was removed; changes are no longer watched");
                return;
            } else if (event->len > 0 && !(event->mask & IN_ISDIR)) {
                std::string name = event->name;
                if (is_valid_file_name(name)) {
                    on_change_(name);
                }
            }
        }
    }
}
