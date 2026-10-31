// Bazarish project (c) 2026
#pragma once

#include <Session.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bazarish::bot {

struct Transfer {
    std::string peer;
    std::string messageId;
    bool outgoing = true;
    std::string path;
    std::string state;
    std::string stage;
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    std::string error;
};

class Transfers {
public:
    void expect(const std::string& peer, const std::string& messageId, const std::string& path);
    std::optional<Transfer> note(const client::TransferEvent& event);
    bool active(const std::string& peer, const std::string& messageId) const;
    std::optional<Transfer> find(const std::string& peer, const std::string& messageId) const;
    std::vector<Transfer> all() const;

private:
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Transfer> transfers_;
};

const char* transferStateName(client::TransferState state);

}  // namespace bazarish::bot
