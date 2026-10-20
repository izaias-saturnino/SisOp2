#include "../common/log.hpp"
#include "../common/net.hpp"
#include "client.hpp"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: " << argv[0] << " <username> <server_host> <server_port> [sync_dir]\n"
                  << "\n"
                  << "Keeps sync_dir (default ./sync_dir) in sync with your files on the server.\n"
                  << "Set SISOP_LOG=debug|info|warn|error to change how much is logged.\n";
        return 2;
    }

    ClientOptions options;
    options.user = argv[1];
    options.host = argv[2];
    if (!parse_port(argv[3], options.port)) {
        std::cerr << "invalid port: " << argv[3] << "\n";
        return 2;
    }
    if (argc == 5) {
        options.sync_dir = argv[4];
    }

    log_init("client", LogLevel::Warn);
    Client client(options);
    return client.run();
}
