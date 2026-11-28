#pragma once

// RAII building blocks for the SQLite store: an owning connection handle, a
// movable prepared statement, a step helper that never hides an error as "no
// rows", and a transaction guard. Header-only so tests can exercise them.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <sqlite3.h>

namespace thewatcher::server::sqlite
{

// How long a statement waits for another connection's lock before SQLITE_BUSY
// is reported (backup, WAL checkpoint, another process).
inline constexpr int kBusyTimeoutMs = 5000;

// Closes a sqlite3 handle. sqlite3_close_v2 defers the close until any
// statement still alive is finalized, so destruction order cannot leak.
struct ConnectionCloser
{
    void operator()(sqlite3* db) const noexcept
    {
        sqlite3_close_v2(db);
    }
};

// Owns a sqlite3 handle. Converts implicitly to sqlite3* so it can be passed
// straight to the C API.
class Connection
{
public:
    // Opens `path` and installs the busy timeout. The handle is owned before
    // any check runs, so a throwing constructor cannot leak it (sqlite3_open
    // returns a handle even when it fails).
    explicit Connection(const std::string& path)
    {
        sqlite3* raw = nullptr;
        const int rc = sqlite3_open(path.c_str(), &raw);
        db_.reset(raw);
        if (rc != SQLITE_OK)
            throw std::runtime_error(std::string("sqlite3_open: ") + (raw ? sqlite3_errmsg(raw) : "out of memory"));
        sqlite3_busy_timeout(raw, kBusyTimeoutMs);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    ~Connection() = default;

    sqlite3* get() const noexcept
    {
        return db_.get();
    }
    operator sqlite3*() const noexcept
    {
        return db_.get();
    }

private:
    std::unique_ptr<sqlite3, ConnectionCloser> db_;
};

// Owns a prepared statement. `s` stays public because sqlite3_prepare_v2
// writes it through an out-parameter.
struct Stmt
{
    sqlite3_stmt* s = nullptr;

    Stmt() = default;
    ~Stmt()
    {
        reset();
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept
        : s(std::exchange(other.s, nullptr))
    {
    }
    Stmt& operator=(Stmt&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            s = std::exchange(other.s, nullptr);
        }
        return *this;
    }

    sqlite3_stmt* operator->() const noexcept
    {
        return s;
    }

private:
    void reset() noexcept
    {
        if (s)
            sqlite3_finalize(s);
        s = nullptr;
    }
};

// Throws when `rc` is not a success code.
inline void check(int rc, sqlite3* db, const char* ctx)
{
    if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
        throw std::runtime_error(std::string(ctx) + ": " + sqlite3_errmsg(db));
}

// Steps a read statement. Returns true on SQLITE_ROW and false on SQLITE_DONE;
// any other result (SQLITE_BUSY, SQLITE_ERROR, ...) throws, so a locked or
// failing database is never reported as "no rows".
inline bool step_row(sqlite3_stmt* stmt, sqlite3* db, const char* ctx)
{
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    throw std::runtime_error(std::string(ctx) + ": " + sqlite3_errmsg(db));
}

// Runs `sql` and throws on failure.
inline void exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string msg = err ? err : sqlite3_errmsg(db);
        sqlite3_free(err);
        throw std::runtime_error("sqlite exec: " + msg);
    }
}

// Scope guard for a write transaction. BEGIN IMMEDIATE takes the write lock
// up front so a later statement cannot fail with SQLITE_BUSY halfway through.
// Unless commit() succeeds, the destructor rolls back; it never throws, so an
// in-flight exception is not masked by a failing ROLLBACK.
class Transaction
{
public:
    explicit Transaction(sqlite3* db)
        : db_(db)
    {
        exec(db_, "BEGIN IMMEDIATE;");
        open_ = true;
    }
    ~Transaction() noexcept
    {
        if (open_)
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    // If COMMIT fails the transaction stays open and the destructor rolls it back.
    void commit()
    {
        exec(db_, "COMMIT;");
        open_ = false;
    }

private:
    sqlite3* db_;
    bool open_ = false;
};

} // namespace thewatcher::server::sqlite
