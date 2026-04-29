#include <catch2/catch_all.hpp>

#include <psitri_multiindex/table.hpp>

#include <psio/reflect.hpp>
#include <psitri/database.hpp>
#include <psitri/database_impl.hpp>
#include <psitri/transaction.hpp>
#include <psitri/write_session.hpp>
#include <psitri/write_session_impl.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <unistd.h>

using namespace psitri_multiindex;

namespace pmidx_test
{
   struct account
   {
      std::uint64_t id;
      std::string   name;
      std::uint64_t balance;
      std::string   notes;
   };
   PSIO_REFLECT(account, id, name, balance, notes)

   struct test_db
   {
      std::filesystem::path                  dir;
      std::shared_ptr<psitri::database>      db;
      std::shared_ptr<psitri::write_session> ws;

      test_db()
      {
         const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
         dir           = std::filesystem::temp_directory_path() /
               ("pmidx_table_" + std::to_string(getpid()) + "_" + std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~test_db() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using accounts_table = table<account, &account::id>;
}  // namespace pmidx_test

using pmidx_test::account;
using pmidx_test::accounts_table;
using pmidx_test::test_db;

TEST_CASE("table: put + get round-trips a primary row", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");

   accounts.put(account{42, "alice", 100, "primary user"});

   auto v = accounts.get(std::uint64_t{42});
   REQUIRE(v.has_value());
   REQUIRE(v->id == 42);
   REQUIRE(v->name == "alice");
   REQUIRE(v->balance == 100);
   REQUIRE(v->notes == "primary user");

   tx.commit();
}

TEST_CASE("table: get on missing primary returns nullopt", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");
   REQUIRE_FALSE(accounts.get(std::uint64_t{999}).has_value());
   tx.commit();
}

TEST_CASE("table: put replaces existing row", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");

   accounts.put(account{1, "alice", 100, "v1"});
   accounts.put(account{1, "alice", 200, "v2"});

   auto v = accounts.get(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->balance == 200);
   REQUIRE(v->notes == "v2");

   tx.commit();
}

TEST_CASE("table: erase by key removes row", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");

   accounts.put(account{7, "bob", 50, ""});
   REQUIRE(accounts.contains(std::uint64_t{7}));
   REQUIRE(accounts.erase(std::uint64_t{7}));
   REQUIRE_FALSE(accounts.contains(std::uint64_t{7}));
   REQUIRE_FALSE(accounts.erase(std::uint64_t{7}));  // already gone

   tx.commit();
}

TEST_CASE("table: many primaries persist across commit", "[table][basic]")
{
   test_db t;
   {
      auto    tx = t.ws->start_transaction(0);
      accounts_table accounts(tx, "acc/");
      for (std::uint64_t k = 1; k <= 20; ++k)
         accounts.put(account{k, "user_" + std::to_string(k), k * 10, ""});
      tx.commit();
   }
   {
      auto tx = t.ws->start_transaction(0);
      accounts_table accounts(tx, "acc/");
      for (std::uint64_t k = 1; k <= 20; ++k)
      {
         auto v = accounts.get(k);
         REQUIRE(v.has_value());
         REQUIRE(v->balance == k * 10);
      }
      tx.commit();
   }
}

TEST_CASE("table: separate prefixes give separate keyspaces", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table table_a(tx, "A/");
   accounts_table table_b(tx, "B/");

   table_a.put(account{1, "alice", 100, ""});
   table_b.put(account{1, "bob", 200, ""});

   auto a = table_a.get(std::uint64_t{1});
   auto b = table_b.get(std::uint64_t{1});
   REQUIRE(a.has_value());
   REQUIRE(b.has_value());
   REQUIRE(a->name == "alice");
   REQUIRE(b->name == "bob");

   tx.commit();
}
