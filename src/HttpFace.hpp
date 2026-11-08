// Bazarish project (c) 2026
#pragma once

#include "OperatorAuth.hpp"
#include "Runtime.hpp"

#include <bazarish/HttpServer.hpp>
#include <bazarish/PortalSession.hpp>
#include <bazarish/WebAssets.hpp>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace bazarish::bot {

class HttpFace {
public:
    HttpFace(Runtime& runtime, std::filesystem::path assets);
    ~HttpFace();

    int start();
    void stop();
    void onRestoreReady(std::function<void()> restore) { restoreReady_ = std::move(restore); }
    void onRestartAsked(std::function<void()> restart) { restartAsked_ = std::move(restart); }

private:
    void apiRoutes();
    void operatorRoutes();
    void accountRoutes();
    std::shared_ptr<Account> accountForToken(const http::Request& request) const;
    bool signedIn(const http::Request& request) const;
    std::optional<http::Response> answerEvents(const std::shared_ptr<Account>& account,
        const http::Request& request, http::Responder respond);
    void reapPolls(bool all);

    struct Poll {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    Runtime& runtime_;
    OperatorAuth operator_;
    WebAssets assets_;
    service::PortalSession sessions_;
    std::unique_ptr<http::Server> server_;
    std::function<void()> restoreReady_;
    std::function<void()> restartAsked_;
    std::mutex pollsMutex_;
    std::list<Poll> polls_;
};

}  // namespace bazarish::bot
