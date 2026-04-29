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

namespace pmidx_subtx_test
{
   struct row_a
   {
      std::uint64_t id;
      std::string   name;
   };
   PSIO_REFLECT(row_a, id, name)

   struct row_b
   {
      std::uint64_t id;
      std::uint64_t balance;
   };
   PSIO_REFLECT(row_b, id, balance)

   struct fixture
   {
      std::filesystem::path                  dir;
      std::shared_ptr<psitri::database>      db;
      std::shared_ptr<psitri::write_session> ws;

      fixture()
      {
         const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
         dir           = std::filesystem::temp_directory_path() /
               ("pmidx_subtx_" + std::to_string(getpid()) + "_" + std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using table_a = table<row_a, &row_a::id>;
   using table_b = table<row_b, &row_b::id>;
}  // namespace pmidx_subtx_test

using pmidx_subtx_test::fixture;
using pmidx_subtx_test::row_a;
using pmidx_subtx_test::row_b;
using pmidx_subtx_test::table_a;
using pmidx_subtx_test::table_b;

TEST_CASE("subtx: commit makes writes visible to enclosing tx",
          "[table][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   table_a t(tx, "A/");

   {
      auto sub = tx.sub_transaction();
      t.put(row_a{1, "alice"});
      sub.commit();
   }

   auto v = t.get(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "alice");

   tx.commit();
}

TEST_CASE("subtx: abort rolls back writes from same sub", "[table][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   table_a t(tx, "A/");

   t.put(row_a{1, "v0"});

   {
      auto sub = tx.sub_transaction();
      t.put(row_a{1, "v1"});
      sub.abort();
   }

   // After abort, the row should be back at v0.
   auto v = t.get(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "v0");

   tx.commit();
}

TEST_CASE("subtx: abort discards inserts from same sub", "[table][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   table_a t(tx, "A/");

   {
      auto sub = tx.sub_transaction();
      t.put(row_a{99, "ephemeral"});
      sub.abort();
   }
   REQUIRE_FALSE(t.contains(std::uint64_t{99}));
   tx.commit();
}

TEST_CASE("subtx: abort is atomic across two tables sharing the tx",
          "[table][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   table_a ta(tx, "A/");
   table_b tb(tx, "B/");

   ta.put(row_a{1, "alice"});
   tb.put(row_b{1, 100});

   {
      auto sub = tx.sub_transaction();
      ta.put(row_a{1, "ALICE_NEW"});
      tb.put(row_b{1, 999});
      sub.abort();
   }

   auto a = ta.get(std::uint64_t{1});
   auto b = tb.get(std::uint64_t{1});
   REQUIRE(a.has_value());
   REQUIRE(b.has_value());
   REQUIRE(a->name == "alice");
   REQUIRE(b->balance == 100);

   tx.commit();
}

TEST_CASE("subtx: nested subs LIFO commit/abort", "[table][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   table_a t(tx, "A/");

   t.put(row_a{1, "v0"});

   {
      auto outer = tx.sub_transaction();
      t.put(row_a{1, "v1"});

      {
         auto inner = tx.sub_transaction();
         t.put(row_a{1, "v2"});
         inner.abort();   // discards v2, exposes v1 to outer
      }

      auto v = t.get(std::uint64_t{1});
      REQUIRE(v.has_value());
      REQUIRE(v->name == "v1");

      outer.commit();      // merges v1 into the parent tx
   }

   auto v = t.get(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "v1");

   tx.commit();
}

TEST_CASE("subtx: large-value abort rollback (regression for value_node bug)",
          "[table][subtx]")
{
   // Mirrors the spring ram_tests scenario that uncovered the abort_frame
   // value_node bug we fixed. Confirm the fix is in psitri main and reaches
   // through to the table layer.
   fixture f;

   {
      auto    tx = f.ws->start_transaction(0);
      table_a t(tx, "A/");
      for (std::uint64_t k = 1; k <= 10; ++k)
         t.put(row_a{k, std::string(1780, 'a')});
      tx.commit();
   }

   {
      auto    tx = f.ws->start_transaction(0);
      table_a t(tx, "A/");
      {
         auto sub = tx.sub_transaction();
         for (std::uint64_t k = 1; k <= 10; ++k)
            t.put(row_a{k, std::string(1790, 'b')});
         sub.abort();
      }

      for (std::uint64_t k = 1; k <= 10; ++k)
      {
         auto v = t.get(k);
         REQUIRE(v.has_value());
         REQUIRE(v->name.size() == 1780);
         REQUIRE(v->name.front() == 'a');
      }
      tx.commit();
   }
}
