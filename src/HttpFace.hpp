// Bazarish project (c) 2026
#pragma once

#include "Runtime.hpp"

#include <bazarish/HttpServer.hpp>
#include <bazarish/PortalSession.hpp>
#include <bazarish/WebAssets.hpp>

#include <memory>
#include <string>

namespace bazarish::bot {

class HttpFace {
public:
    HttpFace(Runtime& runtime, std::filesystem::path assets);
    ~HttpFace();

    int start();
    void stop();

private:
    void routes();
    std::string tokenFor(const http::Request& request) const;
    bool signedIn(const http::Request& request) const;

    Runtime& runtime_;
    WebAssets assets_;
    service::PortalSession sessions_;
    std::unique_ptr<http::Server> server_;
};

}  // namespace bazarish::bot
