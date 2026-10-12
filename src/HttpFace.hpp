// Bazarish project (c) 2026
#pragma once

#include "OperatorAuth.hpp"
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
    void apiRoutes();
    void operatorRoutes();
    void accountRoutes();
    std::shared_ptr<Account> accountForToken(const http::Request& request) const;
    bool signedIn(const http::Request& request) const;

    Runtime& runtime_;
    OperatorAuth operator_;
    WebAssets assets_;
    service::PortalSession sessions_;
    std::unique_ptr<http::Server> server_;
};

}  // namespace bazarish::bot
