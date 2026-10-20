#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Wire format: every message is an 8-byte header followed by `length` bytes
// of payload, all integers in network byte order (big-endian):
//
//   uint16 type | uint16 reserved (0) | uint32 length | payload...
//
// Payload fields are written with Writer and read back with Reader. Strings
// are a uint16 length followed by the bytes (no terminator).

constexpr std::size_t HEADER_SIZE = 8;
constexpr uint32_t MAX_PAYLOAD = 1024 * 1024;
constexpr std::size_t FILE_CHUNK_SIZE = 64 * 1024;

enum class Msg : uint16_t {
    // Generic replies.
    Ok = 1,
    Error = 2, // str reason

    // Client -> server, command connection.
    Login = 10,     // str username
    Logout = 11,
    Ping = 12,
    ListFiles = 13,
    Upload = 14,    // u8 origin, file header, then a file body
    Download = 15,  // str name
    Delete = 16,    // u8 origin, str name

    // Server -> client, command connection.
    LoginOk = 20,   // u64 session token, server list
    Redirect = 21,  // str host, u16 port (empty host: primary unknown, retry later)
    Pong = 22,
    FileList = 23,  // u32 count, file infos
    File = 24,      // file header, then a file body (reply to Download)

    // Sync connection: the client attaches it to its session, then the
    // server pushes changes made by the user's other sessions.
    SyncAttach = 30, // u64 session token
    PushFile = 31,   // file header, then a file body
    PushDelete = 32, // str name
    ServerList = 33, // server list

    // File bodies, sent after a file header announcing the size.
    FileData = 40,  // raw bytes
    FileEnd = 41,
    FileAbort = 42, // the sender could not read the whole file; discard it

    // Server <-> server.
    Join = 50,            // str host, u16 port
    JoinOk = 51,          // u32 assigned id, u32 primary id
    Election = 52,        // server address of the candidate
    Coordinator = 53,     // server address of the new primary
    ReplicaAttach = 54,   // primary address, server list
    ReplicaManifest = 55, // u32 count, (str user, file info)...
    ReplPut = 56,         // str user, file header, then a file body
    ReplDelete = 57,      // str user, str name
    Membership = 58,      // server list
    Heartbeat = 59,
};

// Where a client change came from: an explicit command (pushed to every
// session of the user, including the sender's own sync_dir) or the sync_dir
// watcher (pushed to the user's other sessions only).
enum class Origin : uint8_t { Command = 0, Sync = 1 };

const char* msg_name(Msg type);

struct Message {
    Msg type = Msg::Error;
    std::string payload;
};

struct ProtocolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Writer {
public:
    Writer& u8(uint8_t value);
    Writer& u16(uint16_t value);
    Writer& u32(uint32_t value);
    Writer& u64(uint64_t value);
    Writer& i64(int64_t value);
    Writer& str(const std::string& value);
    const std::string& data() const { return buf_; }

private:
    std::string buf_;
};

// Reads fields from a payload. Throws ProtocolError when the payload is too
// short, so handlers can treat a malformed message as a broken connection.
class Reader {
public:
    explicit Reader(const std::string& data) : data_(data) {}
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int64_t i64();
    std::string str();

private:
    const unsigned char* take(std::size_t n);
    const std::string& data_;
    std::size_t pos_ = 0;
};

struct ServerAddress {
    uint32_t id = 0;
    std::string host;
    uint16_t port = 0;
};

void write_server(Writer& w, const ServerAddress& server);
ServerAddress read_server(Reader& r);
void write_servers(Writer& w, const std::vector<ServerAddress>& servers);
std::vector<ServerAddress> read_servers(Reader& r);
std::string describe(const ServerAddress& server);
