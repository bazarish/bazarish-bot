// Bazarish project (c) 2026
#pragma once

#include <bazarish/I2p.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace bazarish::daemon {

// What the daemon runs on, read out of its one config document. Nothing here is
// settable on the command line: two places to say the same thing is two answers
// to the same question.
struct Config {
    // Where the account, the mailbox and the I2P state live.
    std::filesystem::path stateDir;

    // The REST API the operator's automation calls.
    std::string apiHost;
    int apiPort = 0;
    // What a caller must present on that API. An empty token is refused at
    // startup rather than served: an open API is an open account.
    std::string apiToken;

    // The I2P transport. With a SAM address the daemon speaks to a router that
    // is already running; without one it runs its own.
    bool useSam = false;
    std::string samHost;
    int samPort = 0;

    // How long the destinations this daemon serves are, in hops.
    i2p::Privacy privacy = i2p::Privacy::eMiddle;

    std::string logLevel;
};

// Reads the document at `path`. Throws when it cannot be read, when it is not
// the shape a config is, or when a value it needs is missing.
Config readConfig(const std::filesystem::path& path);

}  // namespace bazarish::daemon
