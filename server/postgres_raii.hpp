#pragma once

#ifdef HAVE_LIBPQ

// RAII building blocks for the PostgreSQL store: an owning, self-healing
// connection handle, and a transaction guard.

#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <libpq-fe.h>

namespace thewatcher::server::postgres
{

// Closes a PGconn. PQfinish accepts nullptr, but unique_ptr never calls the
// deleter for one.
struct ConnectionCloser
{
    void operator()(PGconn* conn) const noexcept
    {
        PQfinish(conn);
    }
};

class Transaction;

// Owns a PGconn and keeps it usable. Statements run through exec() and
// exec_params(), which first re-establish a dead connection (PQreset, a few
// attempts), so a pgBouncer restart or a failover is survived without
// restarting the server process.
//
// Reconnect policy:
//  - Outside a transaction a connection found dead before a statement is
//    reset, then the statement runs.
//  - If the connection dies during a read-only statement (SELECT), the
//    statement is retried once on the fresh connection.
//  - A write that fails because the connection died is NOT retried: it may
//    already have been applied. It throws; the next call reconnects.
//  - Inside a transaction nothing is reset: the session state would be lost
//    mid-transaction. The call throws, the guard rolls back, and the next
//    call reconnects.
class Connection
{
public:
    // Connects with `dsn`. PQconnectdb returns a handle even on failure; it is
    // owned before the status check, so a throwing constructor cannot leak it.
    explicit Connection(std::string dsn)
        : dsn_(std::move(dsn))
        , conn_(PQconnectdb(dsn_.c_str()))
    {
        if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK)
            throw std::runtime_error("PostgreSQL connect failed: " +
                                     std::string(conn_ ? PQerrorMessage(conn_.get()) : "out of memory"));
        // The idempotent migrations (CREATE ... IF NOT EXISTS) raise a NOTICE
        // per object on every start; they are not worth printing to stderr.
        PQsetNoticeProcessor(
            conn_.get(),
            [](void*, const char*) {
            },
            nullptr);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;
    ~Connection() = default;

    PGconn* get() const noexcept
    {
        return conn_.get();
    }
    operator PGconn*() const noexcept
    {
        return conn_.get();
    }

    // Same contracts as PQexec / PQexecParams, plus reconnection. The caller
    // owns the result (wrap it in an RAII holder).
    // Test seam: called with the SQL text just before each statement is sent
    // (after any reconnect, never for BEGIN/COMMIT/ROLLBACK). Lets a test cut
    // the connection while a statement or transaction is in flight. Unset, and
    // so a no-op, in production.
    using StatementHook = std::function<void(const char* sql)>;
    void set_statement_hook(StatementHook hook)
    {
        statement_hook_ = std::move(hook);
    }

    PGresult* exec(const char* sql)
    {
        return run(sql, [&] {
            return PQexec(conn_.get(), sql);
        });
    }
    PGresult* exec_params(const char* sql, int n_params, const Oid* param_types, const char* const* param_values,
                          const int* param_lengths, const int* param_formats, int result_format)
    {
        return run(sql, [&] {
            return PQexecParams(conn_.get(), sql, n_params, param_types, param_values, param_lengths, param_formats,
                                result_format);
        });
    }

    // Re-establishes the connection when it is bad: up to `attempts` resets
    // with a short, growing pause. Throws when it stays down or when a
    // transaction is open.
    void ensure_connected(int attempts = 3)
    {
        if (PQstatus(conn_.get()) == CONNECTION_OK)
            return;
        if (in_transaction_)
            throw std::runtime_error("PostgreSQL connection lost inside a transaction: " +
                                     std::string(PQerrorMessage(conn_.get())));
        for (int attempt = 1; attempt <= attempts; ++attempt)
        {
            PQreset(conn_.get());
            if (PQstatus(conn_.get()) == CONNECTION_OK)
                return;
            if (attempt < attempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
        }
        throw std::runtime_error("PostgreSQL reconnect failed: " + std::string(PQerrorMessage(conn_.get())));
    }

private:
    friend class Transaction;

    // True for statements that are safe to repeat.
    static bool is_read_only(const char* sql)
    {
        while (*sql != '\0' && std::isspace(static_cast<unsigned char>(*sql)))
            ++sql;
        const char* select = "SELECT";
        for (int i = 0; i < 6; ++i)
            if (std::toupper(static_cast<unsigned char>(sql[i])) != select[i])
                return false;
        return true;
    }

    template <typename Call>
    PGresult* run(const char* sql, Call&& call)
    {
        ensure_connected();
        if (statement_hook_)
            statement_hook_(sql);
        PGresult* res = call();
        if (PQstatus(conn_.get()) == CONNECTION_OK || in_transaction_ || !is_read_only(sql))
            return res;
        PQclear(res);
        ensure_connected();
        return call();
    }

    std::string dsn_;
    std::unique_ptr<PGconn, ConnectionCloser> conn_;
    bool in_transaction_ = false;
    StatementHook statement_hook_;
};

// Runs a command that returns no rows. Returns whether it succeeded; the
// result is always cleared.
inline bool run_command(PGconn* conn, const char* sql) noexcept
{
    PGresult* res = PQexec(conn, sql);
    const bool ok = res != nullptr && PQresultStatus(res) == PGRES_COMMAND_OK;
    PQclear(res);
    return ok;
}

// Scope guard for a transaction. Unless commit() succeeds, the destructor
// rolls back; it never throws, so an in-flight exception is not masked by a
// failing ROLLBACK (for example on a dropped connection).
class Transaction
{
public:
    explicit Transaction(Connection& conn)
        : conn_(conn)
    {
        conn_.ensure_connected(); // reconnect before, never inside, a transaction
        if (!run_command(conn_.get(), "BEGIN"))
            throw std::runtime_error(std::string("BEGIN failed: ") + PQerrorMessage(conn_.get()));
        conn_.in_transaction_ = true;
        open_ = true;
    }
    ~Transaction() noexcept
    {
        if (open_)
        {
            run_command(conn_.get(), "ROLLBACK");
            conn_.in_transaction_ = false;
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    // If COMMIT fails the destructor attempts a rollback.
    void commit()
    {
        if (!run_command(conn_.get(), "COMMIT"))
            throw std::runtime_error(std::string("COMMIT failed: ") + PQerrorMessage(conn_.get()));
        open_ = false;
        conn_.in_transaction_ = false;
    }

private:
    Connection& conn_;
    bool open_ = false;
};

} // namespace thewatcher::server::postgres

#endif // HAVE_LIBPQ
