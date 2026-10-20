#include "../common/log.hpp"
#include "../common/net.hpp"
#include "server.hpp"

#include <iostream>
#include <string>

namespace {

void usage(const char* program)
{
    std::cerr << "usage: " << program
              << " [--port PORT] [--host HOST] [--data DIR] [--certs DIR] [--join HOST:PORT]\n"
              << "       [--expect HOST:PORT,...]\n"
              << "\n"
              << "  --port PORT       port to listen on (default 4000)\n"
              << "  --host HOST       address other servers and clients use to reach this one\n"
              << "                    (default 127.0.0.1; set it when running on several machines,\n"
              << "                    and include it in the certificate: tools/make-certs.sh)\n"
              << "  --data DIR        where to keep the files (default server_data_<port>)\n"
              << "  --certs DIR       ca.pem, server.pem and server.key (default certs)\n"
              << "  --expect HOST:PORT[,HOST:PORT...]\n"
              << "                    servers that should be in the cluster; a warning is logged\n"
              << "                    (and shown to clients) as soon as one is missing, or running\n"
              << "                    on its own because of a split or a forgotten --join\n"
              << "  --join HOST:PORT  join the cluster that server belongs to, as a backup;\n"
              << "                    without it, this server starts a new cluster as the primary\n"
              << "\n"
              << "Set SISOP_LOG=debug|info|warn|error to change how much is logged.\n";
}

} // namespace

int main(int argc, char* argv[])
{
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;
        if (arg == "--port" && has_value) {
            if (!parse_port(argv[++i], options.port)) {
                std::cerr << "invalid port: " << argv[i] << "\n";
                return 2;
            }
        } else if (arg == "--host" && has_value) {
            options.host = argv[++i];
        } else if (arg == "--data" && has_value) {
            options.data_dir = argv[++i];
        } else if (arg == "--certs" && has_value) {
            options.certs_dir = argv[++i];
        } else if (arg == "--expect" && has_value) {
            std::string list = argv[++i];
            for (std::size_t start = 0; start <= list.size();) {
                std::size_t comma = list.find(',', start);
                if (comma == std::string::npos) comma = list.size();
                Endpoint server;
                if (!parse_endpoint(list.substr(start, comma - start), server.host, server.port)) {
                    std::cerr << "invalid address in --expect (expected HOST:PORT): "
                              << list.substr(start, comma - start) << "\n";
                    return 2;
                }
                options.expected.push_back(server);
                start = comma + 1;
            }
        } else if (arg == "--join" && has_value) {
            if (!parse_endpoint(argv[++i], options.join_host, options.join_port)) {
                std::cerr << "invalid address (expected HOST:PORT): " << argv[i] << "\n";
                return 2;
            }
        } else {
            usage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
    }

    log_init("server:" + std::to_string(options.port), LogLevel::Info);
    Server server(options);
    return server.run();
}
