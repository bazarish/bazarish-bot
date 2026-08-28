// Bazarish project (c) 2026
#pragma once

#include "Runtime.hpp"

#include <bazarish/HttpServer.hpp>
#include <bazarish/PortalSession.hpp>
#include <bazarish/WebAssets.hpp>

#include <memory>
#include <string>

namespace bazarish::daemon {

// The one port this daemon answers on, carrying both faces:
//
//   /v1/...   the REST API the operator's automation calls, opened by a token
//             in the Authorization header
//   /ui/...   what the interface itself calls, opened by the operator's password
//   /         the interface
//
// One port because both are the same account seen from two sides, and because an
// operator who has to open a second one will open it to more than they meant to.
class HttpFace {
public:
    HttpFace(Runtime& runtime, std::filesystem::path assets);
    ~HttpFace();

    // Starts serving and returns the bound port.
    int start();
    void stop();

private:
    void routes();
    // The token's name when the request carries one that opens the API, empty
    // otherwise. Bearer only: a token in a query string is a token in a log.
    std::string tokenFor(const http::Request& request) const;
    // Whether the request carries a signed-in operator's cookie.
    bool signedIn(const http::Request& request) const;

    Runtime& runtime_;
    WebAssets assets_;
    service::PortalSession sessions_;
    std::unique_ptr<http::Server> server_;
};

}  // namespace bazarish::daemon
