// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace bazarish::bot {

class Database {
public:
    Database(const std::filesystem::path& file, const std::string& passphrase, std::string label);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* handle() const { return db_; }
    const std::string& label() const { return label_; }
    void run(const std::string& sql) const;
    std::int64_t lastInsertId() const;
    int changes() const;

private:
    std::string label_;
    sqlite3* db_ = nullptr;
};

class Statement {
public:
    Statement(const Database& database, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int index, const std::string& text);
    void bind(int index, std::int64_t value);
    void bind(int index, const Bytes& blob);
    bool step();
    std::string text(int index) const;
    std::int64_t number(int index) const;
    Bytes blob(int index) const;

private:
    const std::string& label_;
    sqlite3_stmt* statement_ = nullptr;
};

}  // namespace bazarish::bot
