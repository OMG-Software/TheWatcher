#pragma once

#ifdef HAVE_LIBPQ

// RAII building blocks for the PostgreSQL store: an owning connection handle
// and a transaction guard.

#include <memory>
#include <stdexcept>
#include <string>

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

// Owns a PGconn. Converts implicitly to PGconn* so it can be passed straight
// to libpq.
class Connection
{
public:
    // Connects with `dsn`. PQconnectdb returns a handle even on failure; it is
    // owned before the status check, so a throwing constructor cannot leak it.
    explicit Connection(const std::string& dsn)
        : conn_(PQconnectdb(dsn.c_str()))
    {
        if (!conn_ || PQstatus(conn_.get()) != CONNECTION_OK)
            throw std::runtime_error("PostgreSQL connect failed: " +
                                     std::string(conn_ ? PQerrorMessage(conn_.get()) : "out of memory"));
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

private:
    std::unique_ptr<PGconn, ConnectionCloser> conn_;
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
    explicit Transaction(PGconn* conn)
        : conn_(conn)
    {
        if (!run_command(conn_, "BEGIN"))
            throw std::runtime_error(std::string("BEGIN failed: ") + PQerrorMessage(conn_));
        open_ = true;
    }
    ~Transaction() noexcept
    {
        if (open_)
            run_command(conn_, "ROLLBACK");
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    // If COMMIT fails the destructor attempts a rollback.
    void commit()
    {
        if (!run_command(conn_, "COMMIT"))
            throw std::runtime_error(std::string("COMMIT failed: ") + PQerrorMessage(conn_));
        open_ = false;
    }

private:
    PGconn* conn_;
    bool open_ = false;
};

} // namespace thewatcher::server::postgres

#endif // HAVE_LIBPQ
