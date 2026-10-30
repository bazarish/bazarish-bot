// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <ApiClient.hpp>

#include <bazarish/Crypto.hpp>
#include <bazarish/I2p.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

namespace fs = std::filesystem;
using namespace bazarish;

int main()
{
    const fs::path state = fs::temp_directory_path() / "bazarish-bot-test-server-side";
    fs::remove_all(state);
    setSelfHostedFacadeOnLoopback(true);
    {
        bot::ServerSide server(state, i2p::Privacy::eMiddle, nullptr);
        server.start();

        const Identity owner = Identity::generate();
        client::ServerEndpoint endpoint;
        endpoint.serverFingerprint = server.fingerprint();
        endpoint.facades = {client::Facade{server.host(), server.port()}};
        client::ApiClient api(owner, "bot01", endpoint);

        CHECK(api.postJson("/v1/account/registration", nlohmann::json::object()).status == 200);
        CHECK(api.get("/v1/account/i2p-status").json().at("approval") == "none");
        CHECK(api.get("/v1/messaging/storage-usage").status == 200);

        client::ServerEndpoint impostor = endpoint;
        impostor.serverFingerprint = Identity::generate().fingerprint();
        client::ApiClient misled(owner, "bot01", impostor);
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
