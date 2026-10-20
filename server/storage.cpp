#include "storage.hpp"

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

Storage::Storage(std::string root)
    : root_(std::move(root)), users_dir_(join_path(root_, "users")), tmp_dir_(join_path(root_, "tmp"))
{
}

bool Storage::init()
{
    if (!make_dirs(users_dir_) || !make_dirs(tmp_dir_)) {
        return false;
    }
    // Leftovers from transfers interrupted by a crash.
    if (DIR* d = opendir(tmp_dir_.c_str())) {
        while (dirent* entry = readdir(d)) {
            std::string name = entry->d_name;
            if (name != "." && name != "..") {
                remove_file(join_path(tmp_dir_, name));
            }
        }
        closedir(d);
    }
    return true;
}

std::string Storage::new_temp_path() const
{
    return make_temp_path(tmp_dir_);
}

std::string Storage::user_dir(const std::string& user) const
{
    return join_path(users_dir_, user);
}

std::string Storage::file_path(const std::string& user, const std::string& name) const
{
    return join_path(user_dir(user), name);
}

bool Storage::ensure_user(const std::string& user)
{
    return make_dirs(user_dir(user));
}

std::vector<std::string> Storage::users() const
{
    std::vector<std::string> users;
    if (DIR* d = opendir(users_dir_.c_str())) {
        while (dirent* entry = readdir(d)) {
            std::string name = entry->d_name;
            struct stat st{};
            if (is_valid_username(name) && stat(user_dir(name).c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                users.push_back(name);
            }
        }
        closedir(d);
    }
    std::sort(users.begin(), users.end());
    return users;
}

std::vector<FileInfo> Storage::list(const std::string& user, bool with_hash) const
{
    return list_dir(user_dir(user), with_hash);
}

bool Storage::commit(const std::string& user, const std::string& name, const std::string& temp,
                     int64_t mtime_ns)
{
    if (!ensure_user(user)) {
        return false;
    }
    set_mtime(temp, mtime_ns);
    return replace_file(temp, file_path(user, name));
}

bool Storage::remove(const std::string& user, const std::string& name)
{
    return remove_file(file_path(user, name));
}
