#include "accounts.hpp"

#include "../common/files.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace {

const char* const SCHEME = "pbkdf2-sha256";
constexpr int ITERATIONS = 100000;
constexpr std::size_t SALT_SIZE = 16;
constexpr std::size_t HASH_SIZE = 32;

std::string to_hex(const unsigned char* data, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (std::size_t i = 0; i < size; ++i) {
        text += digits[data[i] >> 4];
        text += digits[data[i] & 0xF];
    }
    return text;
}

bool from_hex(const std::string& text, std::string& out)
{
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < text.size(); i += 2) {
        int value = 0;
        for (std::size_t j = i; j < i + 2; ++j) {
            char c = text[j];
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) {
                return false;
            }
            value = value * 16 + digit;
        }
        out.push_back(static_cast<char>(value));
    }
    return true;
}

struct ParsedRecord {
    int iterations = 0;
    std::string salt;
    std::string hash;
};

bool parse(const std::string& record, ParsedRecord& parsed)
{
    std::vector<std::string> fields;
    std::stringstream stream(record);
    std::string field;
    while (std::getline(stream, field, '$')) {
        fields.push_back(field);
    }
    if (fields.size() != 4 || fields[0] != SCHEME || fields[1].empty() ||
        fields[1].find_first_not_of("0123456789") != std::string::npos || fields[1].size() > 9) {
        return false;
    }
    parsed.iterations = std::stoi(fields[1]);
    return parsed.iterations > 0 && from_hex(fields[2], parsed.salt) && !parsed.salt.empty() &&
           from_hex(fields[3], parsed.hash) && parsed.hash.size() == HASH_SIZE;
}

bool derive(const std::string& password, const std::string& salt, int iterations, unsigned char* out)
{
    return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                             reinterpret_cast<const unsigned char*>(salt.data()),
                             static_cast<int>(salt.size()), iterations, EVP_sha256(),
                             static_cast<int>(HASH_SIZE), out) == 1;
}

} // namespace

Accounts::Accounts(std::string dir) : dir_(std::move(dir))
{
}

bool Accounts::init()
{
    return make_dirs(dir_);
}

bool Accounts::get(const std::string& user, std::string& record) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream in(join_path(dir_, user));
    return std::getline(in, record) && is_valid_record(record);
}

bool Accounts::create(const std::string& user, const std::string& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream existing(join_path(dir_, user));
    if (existing) {
        return false;
    }
    return write(user, record);
}

bool Accounts::put(const std::string& user, const std::string& record)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return write(user, record);
}

// Caller holds mutex_.
bool Accounts::write(const std::string& user, const std::string& record)
{
    std::string temp = make_temp_path(dir_);
    {
        std::ofstream out(temp, std::ios::trunc);
        out << record << "\n";
        if (!out.flush()) {
            remove_file(temp);
            return false;
        }
    }
    return replace_file(temp, join_path(dir_, user));
}

std::vector<std::pair<std::string, std::string>> Accounts::all() const
{
    std::vector<std::pair<std::string, std::string>> accounts;
    std::vector<std::string> users;
    if (DIR* d = opendir(dir_.c_str())) {
        while (dirent* entry = readdir(d)) {
            if (is_valid_username(entry->d_name)) {
                users.push_back(entry->d_name);
            }
        }
        closedir(d);
    }
    std::sort(users.begin(), users.end());
    for (const std::string& user : users) {
        std::string record;
        if (get(user, record)) {
            accounts.emplace_back(user, record);
        }
    }
    return accounts;
}

std::string Accounts::make_record(const std::string& password)
{
    unsigned char salt[SALT_SIZE];
    unsigned char hash[HASH_SIZE];
    if (RAND_bytes(salt, sizeof(salt)) != 1 ||
        !derive(password, std::string(reinterpret_cast<char*>(salt), sizeof(salt)), ITERATIONS, hash)) {
        return "";
    }
    return std::string(SCHEME) + "$" + std::to_string(ITERATIONS) + "$" + to_hex(salt, sizeof(salt)) +
           "$" + to_hex(hash, sizeof(hash));
}

bool Accounts::verify(const std::string& record, const std::string& password)
{
    ParsedRecord parsed;
    unsigned char hash[HASH_SIZE];
    return parse(record, parsed) && derive(password, parsed.salt, parsed.iterations, hash) &&
           CRYPTO_memcmp(hash, parsed.hash.data(), HASH_SIZE) == 0;
}

bool Accounts::is_valid_record(const std::string& record)
{
    ParsedRecord parsed;
    return parse(record, parsed);
}
