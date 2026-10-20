#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

// Watches a directory tree with inotify and reports the paths (relative to
// the root) of files that were written, moved in or out, or deleted. It only
// says that something happened to a path; the client then looks at the file
// to decide what to do, so duplicate or stale events are harmless.
//
// inotify isn't recursive, so there is one watch per directory. When a whole
// directory is created, moved or deleted, inotify reports just the directory,
// not the files in it; the watcher then reports an empty path, which means
// "rescan everything". The same happens if the kernel's event queue
// overflowed.
class Watcher {
public:
    using Callback = std::function<void(const std::string& path)>;

    Watcher(std::string root, Callback on_change);
    ~Watcher();

    bool start();
    void stop();

private:
    void loop();
    void watch_tree(const std::string& relative_dir);
    void unwatch_tree(const std::string& relative_dir);

    std::string root_;
    Callback on_change_;
    int fd_ = -1;
    int root_wd_ = -1;
    std::map<int, std::string> dirs_; // watch descriptor -> directory relative to root
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
