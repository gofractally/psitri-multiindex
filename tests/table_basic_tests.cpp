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

struct PmidxAccount
{
   std::uint64_t id;
   std::string   name;
   std::uint64_t balance;
   std::string   notes;
};
PSIO_REFLECT(PmidxAccount, id, name, balance, notes)

namespace pmidx_basic_test
{
   struct by_id;

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

   using accounts_table =
       table<PmidxAccount, ordered_unique<by_id, &PmidxAccount::id>>;
}  // namespace pmidx_basic_test

using pmidx_basic_test::accounts_table;
using pmidx_basic_test::by_id;
using pmidx_basic_test::test_db;

TEST_CASE("table: put + get round-trips a primary row", "[table][basic]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");

   accounts.put(PmidxAccount{42, "alice", 100, "primary user"});

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

   accounts.put(PmidxAccount{1, "alice", 100, "v1"});
   accounts.put(PmidxAccount{1, "alice", 200, "v2"});

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

   accounts.put(PmidxAccount{7, "bob", 50, ""});
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
      auto           tx = t.ws->start_transaction(0);
      accounts_table accounts(tx, "acc/");
      for (std::uint64_t k = 1; k <= 20; ++k)
         accounts.put(PmidxAccount{k, "user_" + std::to_string(k), k * 10, ""});
      tx.commit();
   }
   {
      auto           tx = t.ws->start_transaction(0);
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

   table_a.put(PmidxAccount{1, "alice", 100, ""});
   table_b.put(PmidxAccount{1, "bob", 200, ""});

   auto a = table_a.get(std::uint64_t{1});
   auto b = table_b.get(std::uint64_t{1});
   REQUIRE(a.has_value());
   REQUIRE(b.has_value());
   REQUIRE(a->name == "alice");
   REQUIRE(b->name == "bob");

   tx.commit();
}

TEST_CASE("table: find<by_id> is the primary lookup",
          "[table][basic][named-tag]")
{
   test_db        t;
   auto           tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/");
   accounts.put(PmidxAccount{42, "alice", 100, ""});

   auto v = accounts.find<by_id>(std::uint64_t{42});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "alice");

   tx.commit();
}

TEST_CASE("table: clear() wipes every row + isolates from neighbour tables",
          "[table][basic][clear]")
{
   test_db t;
   auto    tx = t.ws->start_transaction(0);
   accounts_table table_a(tx, "A/");
   accounts_table table_b(tx, "B/");

   for (std::uint64_t k = 1; k <= 5; ++k)
   {
      table_a.put(PmidxAccount{k, "a" + std::to_string(k), k, ""});
      table_b.put(PmidxAccount{k, "b" + std::to_string(k), k, ""});
   }

   table_a.clear();

   for (std::uint64_t k = 1; k <= 5; ++k)
      REQUIRE_FALSE(table_a.contains(k));
   REQUIRE(table_a.begin() == table_a.end());

   // Sibling table B is untouched.
   for (std::uint64_t k = 1; k <= 5; ++k)
   {
      auto v = table_b.get(k);
      REQUIRE(v.has_value());
      REQUIRE(v->name == "b" + std::to_string(k));
   }

   tx.commit();
}

TEST_CASE("table: clear() preserves track_row_count and resets count to 0",
          "[table][basic][clear][row_count]")
{
   test_db        t;
   auto           tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/", table_options{.track_row_count = true});

   accounts.put(PmidxAccount{1, "a", 1, ""});
   accounts.put(PmidxAccount{2, "b", 2, ""});
   REQUIRE(accounts.size().value() == 2);

   accounts.clear();
   REQUIRE(accounts.size().has_value());
   REQUIRE(accounts.size().value() == 0);

   accounts.put(PmidxAccount{3, "c", 3, ""});
   REQUIRE(accounts.size().value() == 1);

   tx.commit();
}

TEST_CASE("table: erase_range erases [lo, hi) on the primary tree",
          "[table][basic][erase_range]")
{
   test_db        t;
   auto           tx = t.ws->start_transaction(0);
   accounts_table accounts(tx, "acc/", table_options{.track_row_count = true});

   for (std::uint64_t k = 1; k <= 10; ++k)
      accounts.put(PmidxAccount{k, "u" + std::to_string(k), k, ""});
   REQUIRE(accounts.size().value() == 10);

   const auto erased = accounts.erase_range(std::uint64_t{3}, std::uint64_t{7});
   REQUIRE(erased == 4);  // pks 3, 4, 5, 6

   for (std::uint64_t k = 1; k <= 10; ++k)
   {
      const bool should_exist = (k < 3) || (k >= 7);
      REQUIRE(accounts.contains(k) == should_exist);
   }
   REQUIRE(accounts.size().value() == 6);

   // No-op range.
   REQUIRE(accounts.erase_range(std::uint64_t{50}, std::uint64_t{60}) == 0);
   REQUIRE(accounts.size().value() == 6);

   tx.commit();
}
