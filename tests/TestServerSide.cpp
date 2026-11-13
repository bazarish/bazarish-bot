// Bazarish project (c) 2026
#include "ServerSide.hpp"
#include "DeviceTestKit.hpp"
#include "TestUtil.hpp"

#include <ApiClient.hpp>

#include <bazarish/Crypto.hpp>
#include <bazarish/Errors.hpp>
#include <bazarish/I2p.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using namespace bazarish;

int main()
{
    const fs::path state = testutil::freshDir("bot-test-server-side");
    setSelfHostedFacadeOnLoopback(true);
    {
        bot::ServerSide server(state, i2p::Privacy::eMiddle, std::nullopt);
        server.start();

        const devicetest::Account owner;
        client::ServerEndpoint endpoint;
        endpoint.serverFingerprint = server.fingerprint();
        endpoint.facades = {client::Facade{server.host(), server.port()}};
        client::ApiClient api(owner.device, endpoint);

        const auto refusedFor = [&endpoint](const DeviceCredential& who, const std::string& path)
            -> std::optional<ErrorCode> {
            client::ApiClient caller(who, endpoint);
            try {
                (void)caller.postJson(path, nlohmann::json::object());
            } catch (const client::ApiError& error) {
                return error.code;
            }
            return std::nullopt;
        };
        const devicetest::Account stranger;
        CHECK(refusedFor(stranger.device, "/v1/account/registration") == ErrorCode::eRequestFailed);

        server.admit(owner.fingerprint());
        CHECK(api.postJson("/v1/account/registration", nlohmann::json::object()).status == 200);
        CHECK(api.get("/v1/account/i2p-status").json().at("approval") == "none");
        bool unknownToMailbox = false;
        try {
            (void)api.get("/v1/messaging/storage-usage");
        } catch (const client::ApiError& error) {
            unknownToMailbox = error.code == ErrorCode::eAccountUnknown;
        }
        CHECK(unknownToMailbox);

        client::ServerEndpoint impostor = endpoint;
        impostor.serverFingerprint = Identity::generate().fingerprint();
        client::ApiClient misled(owner.device, impostor);
        bool refused = false;
        try {
            (void)misled.get("/v1/account/i2p-status");
        } catch (const client::ApiError&) {
            refused = true;
        }
        CHECK(refused);
        server.stop();
    }
    fs::remove_all(state);
    std::printf("TestServerSide OK\n");
    return 0;
}
