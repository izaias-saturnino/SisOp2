#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// Watches a directory with inotify and reports the names of files that were
// written, moved in or out, or deleted. It reports only that something
// happened to a name; the client then looks at the file to decide what to do,
// so duplicate or stale events are harmless.
//
// An empty name means events were lost (the kernel queue overflowed) and
// the whole directory should be rescanned.
class Watcher {
public:
    using Callback = std::function<void(const std::string& name)>;

    Watcher(std::string dir, Callback on_change);
    ~Watcher();

    bool start();
    void stop();

private:
    void loop();

    std::string dir_;
    Callback on_change_;
    int fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};
