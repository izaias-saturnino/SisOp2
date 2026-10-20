#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

// User accounts: one file per user in <data>/accounts/<user>, holding a
// salted password hash:
//
//   pbkdf2-sha256$<iterations>$<salt, hex>$<hash, hex>
//
// An account is created the first time someone logs in with a new username;
// that first password becomes the user's password. Records are replicated to
// the backups like files are, so a failover doesn't lose any account.
class Accounts {
public:
    static constexpr std::size_t MIN_PASSWORD_LENGTH = 6;
    static constexpr std::size_t MAX_PASSWORD_LENGTH = 256;

    explicit Accounts(std::string dir);
    bool init();

    bool get(const std::string& user, std::string& record) const;
    // Stores the record unless the account already exists. Returns false if
    // it existed (or couldn't be written).
    bool create(const std::string& user, const std::string& record);
    // Stores the record unconditionally (used when replicating).
    bool put(const std::string& user, const std::string& record);
    std::vector<std::pair<std::string, std::string>> all() const;

    // Hashing is deliberately slow; call these without holding locks.
    static std::string make_record(const std::string& password);
    static bool verify(const std::string& record, const std::string& password);
    static bool is_valid_record(const std::string& record);

private:
    bool write(const std::string& user, const std::string& record);

    std::string dir_;
    mutable std::mutex mutex_;
};
