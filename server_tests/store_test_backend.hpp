// The store under test for store_test.cpp.
//
// By default the store is an in-memory SqliteStore. Built with
// THEWATCHER_STORE_TEST_POSTGRES (and HAVE_LIBPQ) it is a PostgresStore in a
// private, throwaway schema, so the whole scenario list runs against both
// backends. Scenarios use it as:
//
//     TestStoreHandle test_store;
//     Store& store = test_store.store();
#pragma once

#include "../server/store_sqlite.hpp"

#include <memory>

#ifdef THEWATCHER_STORE_TEST_POSTGRES
#include "postgres_test_support.hpp"
#endif

namespace thewatcher::server
{

class TestStoreHandle
{
public:
#ifdef THEWATCHER_STORE_TEST_POSTGRES
    TestStoreHandle()
        : store_(std::make_unique<PostgresStore>(schema_.dsn()))
    {
    }
#else
    TestStoreHandle()
        : store_(std::make_unique<SqliteStore>(":memory:"))
    {
    }
#endif

    Store& store()
    {
        return *store_;
    }

private:
#ifdef THEWATCHER_STORE_TEST_POSTGRES
    test::TestSchema schema_; // declared first: created before, dropped after, the store
#endif
    std::unique_ptr<Store> store_;
};

} // namespace thewatcher::server
