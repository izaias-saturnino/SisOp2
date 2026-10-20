#include "../common/log.hpp"
#include "../common/net.hpp"
#include "../common/tls.hpp"
#include "client.hpp"

#include <cstdlib>
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace {

void usage(const char* program)
{
    std::cerr << "usage: " << program << " <username> <server_host> <server_port> [sync_dir] [--ca FILE]\n"
              << "\n"
              << "Keeps sync_dir (default ./sync_dir) in sync with your files on the server.\n"
              << "  --ca FILE   CA certificate used to verify the server (default certs/ca.pem)\n"
              << "\n"
              << "The password is read from the SISOP_PASSWORD environment variable if set;\n"
              << "otherwise it is asked for (or read from the first line of input when it isn't\n"
              << "a terminal). The first login with a new username creates the account.\n"
              << "Set SISOP_LOG=debug|info|warn|error to change how much is logged.\n";
}

std::string read_password(const std::string& user)
{
    if (const char* env = std::getenv("SISOP_PASSWORD")) {
        return env;
    }
    std::string password;
    if (!isatty(STDIN_FILENO)) {
        std::getline(std::cin, password);
        return password;
    }
    std::cout << "Password for " << user << ": " << std::flush;
    termios saved{};
    tcgetattr(STDIN_FILENO, &saved);
    termios silent = saved;
    silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &silent);
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    std::cout << std::endl;
    return password;
}

} // namespace

int main(int argc, char* argv[])
{
    ClientOptions options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ca" && i + 1 < argc) {
            options.ca_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() < 3 || positional.size() > 4) {
        usage(argv[0]);
        return 2;
    }
    options.user = positional[0];
    options.host = positional[1];
    if (!parse_port(positional[2], options.port)) {
        std::cerr << "invalid port: " << positional[2] << "\n";
        return 2;
    }
    if (positional.size() == 4) {
        options.sync_dir = positional[3];
    }

    log_init("client", LogLevel::Warn);
    TlsConfig tls;
    tls.ca_file = options.ca_file;
    std::string error;
    if (!tls_init(tls, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    options.password = read_password(options.user);
    Client client(options);
    return client.run();
}
