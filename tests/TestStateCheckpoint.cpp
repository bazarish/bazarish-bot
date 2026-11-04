// Bazarish project (c) 2026
#include "StateCheckpoint.hpp"

#include <AccountDb.hpp>
#include <TranscriptDb.hpp>

#include <bazarish/PrivateFile.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
    const fs::path root = fs::temp_directory_path() / "bz-bot-checkpoint";
    fs::remove_all(root);
    const fs::path live = root / "live";
    const fs::path account = live / "accounts" / "a1.db";
    fs::create_directories(live / "server");
    fs::create_directories(live / "i2p");
    writeFileAtomic(live / "settings.json", std::string_view("{}"));
    writeFileAtomic(live / "server" / "identity.pem", std::string_view("pem"));
    writeFileAtomic(live / "i2p" / "router.info", std::string_view("router"));

    client::AccountDb db(account, std::string());
    db.putText("probe", "before");
    client::TranscriptStore store;
    CHECK(store.open(account));
    client::StoredMessage row;
    row.peer = "peer";
    row.type = "text";
    row.e2eId = "m1";
    row.text = "held in the write-ahead log";
    CHECK(store.append(row) != 0);

    const Bytes taken = bot::takeCheckpoint(live);
    db.putText("probe", "after");

    const fs::path restored = root / "restored";
    bot::restoreCheckpoint(restored, taken);
    CHECK(readFileText(restored / "settings.json") == "{}");
    CHECK(readFileText(restored / "server" / "identity.pem") == "pem");
    CHECK(!fs::exists(restored / "i2p"));
    for (const auto& entry : fs::recursive_directory_iterator(restored)) {
        CHECK(entry.path().extension() != ".checkpoint");
    }
    {
        const client::AccountDb copy(restored / "accounts" / "a1.db", std::string());
        CHECK(copy.text("probe") == "before");
        client::TranscriptStore copied;
        CHECK(copied.open(restored / "accounts" / "a1.db"));
        CHECK(copied.latestMessages("peer", 10).size() == 1);
    }

    bot::restoreCheckpoint(restored, bot::takeCheckpoint(live));
    CHECK(client::AccountDb(restored / "accounts" / "a1.db", std::string()).text("probe") == "after");

    store.close();
    fs::remove_all(root);
    std::fprintf(stderr, "TestStateCheckpoint passed\n");
    return 0;
}
