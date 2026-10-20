#include "protocol.hpp"

const char* msg_name(Msg type)
{
    switch (type) {
    case Msg::Ok: return "Ok";
    case Msg::Error: return "Error";
    case Msg::Login: return "Login";
    case Msg::Logout: return "Logout";
    case Msg::Ping: return "Ping";
    case Msg::ListFiles: return "ListFiles";
    case Msg::Upload: return "Upload";
    case Msg::Download: return "Download";
    case Msg::Delete: return "Delete";
    case Msg::LoginOk: return "LoginOk";
    case Msg::Redirect: return "Redirect";
    case Msg::Pong: return "Pong";
    case Msg::FileList: return "FileList";
    case Msg::File: return "File";
    case Msg::SyncAttach: return "SyncAttach";
    case Msg::PushFile: return "PushFile";
    case Msg::PushDelete: return "PushDelete";
    case Msg::ServerList: return "ServerList";
    case Msg::FileData: return "FileData";
    case Msg::FileEnd: return "FileEnd";
    case Msg::FileAbort: return "FileAbort";
    case Msg::Join: return "Join";
    case Msg::JoinOk: return "JoinOk";
    case Msg::Election: return "Election";
    case Msg::Coordinator: return "Coordinator";
    case Msg::ReplicaAttach: return "ReplicaAttach";
    case Msg::ReplicaManifest: return "ReplicaManifest";
    case Msg::ReplPut: return "ReplPut";
    case Msg::ReplDelete: return "ReplDelete";
    case Msg::Membership: return "Membership";
    case Msg::Heartbeat: return "Heartbeat";
    case Msg::ReplAccount: return "ReplAccount";
    }
    return "Unknown";
}

Writer& Writer::u8(uint8_t value)
{
    buf_.push_back(static_cast<char>(value));
    return *this;
}

Writer& Writer::u16(uint16_t value)
{
    u8(static_cast<uint8_t>(value >> 8));
    return u8(static_cast<uint8_t>(value));
}

Writer& Writer::u32(uint32_t value)
{
    u16(static_cast<uint16_t>(value >> 16));
    return u16(static_cast<uint16_t>(value));
}

Writer& Writer::u64(uint64_t value)
{
    u32(static_cast<uint32_t>(value >> 32));
    return u32(static_cast<uint32_t>(value));
}

Writer& Writer::i64(int64_t value)
{
    return u64(static_cast<uint64_t>(value));
}

Writer& Writer::str(const std::string& value)
{
    if (value.size() > 0xFFFF) {
        throw ProtocolError("string too long for the protocol");
    }
    u16(static_cast<uint16_t>(value.size()));
    buf_.append(value);
    return *this;
}

const unsigned char* Reader::take(std::size_t n)
{
    if (data_.size() - pos_ < n) {
        throw ProtocolError("message payload too short");
    }
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data_.data()) + pos_;
    pos_ += n;
    return p;
}

uint8_t Reader::u8()
{
    return *take(1);
}

uint16_t Reader::u16()
{
    const unsigned char* p = take(2);
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Reader::u32()
{
    uint32_t high = u16();
    return (high << 16) | u16();
}

uint64_t Reader::u64()
{
    uint64_t high = u32();
    return (high << 32) | u32();
}

int64_t Reader::i64()
{
    return static_cast<int64_t>(u64());
}

std::string Reader::str()
{
    uint16_t size = u16();
    const unsigned char* p = take(size);
    return std::string(reinterpret_cast<const char*>(p), size);
}

void write_server(Writer& w, const ServerAddress& server)
{
    w.u32(server.id).str(server.host).u16(server.port);
}

ServerAddress read_server(Reader& r)
{
    ServerAddress server;
    server.id = r.u32();
    server.host = r.str();
    server.port = r.u16();
    return server;
}

void write_servers(Writer& w, const std::vector<ServerAddress>& servers)
{
    w.u16(static_cast<uint16_t>(servers.size()));
    for (const ServerAddress& server : servers) {
        write_server(w, server);
    }
}

std::vector<ServerAddress> read_servers(Reader& r)
{
    std::vector<ServerAddress> servers(r.u16());
    for (ServerAddress& server : servers) {
        server = read_server(r);
    }
    return servers;
}

std::string describe(const ServerAddress& server)
{
    return server.host + ":" + std::to_string(server.port) + " (id " + std::to_string(server.id) + ")";
}
