// Bazarish project (c) 2026
#pragma once

#include <filesystem>
#include <string>

namespace bazarish::daemon {

// The three things a daemon cannot be told by the interface it has not started
// yet: where to keep everything, where to answer, and how loudly to say what it
// is doing. Everything else - the transport, the account, the tokens that open
// the API - is set in that interface and kept in the state directory, because an
// operator who has to edit a file and restart to change a token is an operator
// with one fewer reason to change it.
struct Config {
    std::filesystem::path stateDir;
    // Where the web interface and the REST API answer. Loopback by default: what
    // is served there opens the account.
    std::string listenHost;
    int listenPort = 0;
    std::string logLevel;
};

// Reads the document at `path`. Throws when it cannot be read, when it is not
// the shape a config is, or when it does not say where the state lives.
Config readConfig(const std::filesystem::path& path);

}  // namespace bazarish::daemon
