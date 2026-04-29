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
#include <vector>

using namespace psitri_multiindex;

namespace pmidx_iter_test
{
   struct row
   {
      std::uint64_t id;
      std::string   name;
   };
   PSIO_REFLECT(row, id, name)

   struct fixture
   {
      std::filesystem::path                  dir;
      std::shared_ptr<psitri::database>      db;
      std::shared_ptr<psitri::write_session> ws;

      fixture()
      {
         const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
         dir           = std::filesystem::temp_directory_path() /
               ("pmidx_iter_" + std::to_string(getpid()) + "_" + std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using rows_table = table<row, &row::id>;
}  // namespace pmidx_iter_test

using pmidx_iter_test::fixture;
using pmidx_iter_test::row;
using pmidx_iter_test::rows_table;

TEST_CASE("iter: begin/end on empty table is equal", "[table][iter]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");
   REQUIRE(rows.begin() == rows.end());
   tx.commit();
}

TEST_CASE("iter: begin walks all rows in primary order", "[table][iter]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   // Insert out of order.
   for (std::uint64_t k : {std::uint64_t{5}, std::uint64_t{1}, std::uint64_t{3},
                           std::uint64_t{2}, std::uint64_t{4}})
      rows.put(row{k, "v" + std::to_string(k)});

   std::vector<std::uint64_t> seen;
   for (auto it = rows.begin(); it != rows.end(); ++it)
      seen.push_back((*it).id);

   REQUIRE(seen == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
   tx.commit();
}

TEST_CASE("iter: lower_bound returns iterator at >= key",
          "[table][iter][bounds]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 10; k <= 50; k += 10)
      rows.put(row{k, ""});

   {
      auto it = rows.lower_bound(std::uint64_t{25});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 30);
   }
   {
      auto it = rows.lower_bound(std::uint64_t{30});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 30);  // exact hit
   }
   {
      auto it = rows.lower_bound(std::uint64_t{60});
      REQUIRE(it == rows.end());  // past last
   }
   tx.commit();
}

TEST_CASE("iter: upper_bound returns iterator at > key",
          "[table][iter][bounds]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 10; k <= 50; k += 10)
      rows.put(row{k, ""});

   {
      auto it = rows.upper_bound(std::uint64_t{30});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 40);  // strictly greater than 30
   }
   {
      auto it = rows.upper_bound(std::uint64_t{50});
      REQUIRE(it == rows.end());  // nothing > 50
   }
   tx.commit();
}

TEST_CASE("iter: range iteration via lower_bound + upper_bound",
          "[table][iter][bounds]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 1; k <= 20; ++k)
      rows.put(row{k, ""});

   std::vector<std::uint64_t> seen;
   auto it  = rows.lower_bound(std::uint64_t{5});
   auto end = rows.upper_bound(std::uint64_t{15});
   for (; it != end; ++it)
      seen.push_back((*it).id);
   REQUIRE(seen == std::vector<std::uint64_t>{5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
   tx.commit();
}

TEST_CASE("iter: stops at table prefix boundary", "[table][iter][isolation]")
{
   // Two tables on the same transaction with different prefixes; iterating
   // one must not bleed into the other's rows even though they share a tree.
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   rows_table table_a(tx, "A/");
   rows_table table_b(tx, "B/");

   for (std::uint64_t k = 1; k <= 5; ++k)
      table_a.put(row{k, "a" + std::to_string(k)});
   for (std::uint64_t k = 1; k <= 5; ++k)
      table_b.put(row{k, "b" + std::to_string(k)});

   std::vector<std::string> a_names;
   for (auto it = table_a.begin(); it != table_a.end(); ++it)
      a_names.push_back((*it).name);
   REQUIRE(a_names == std::vector<std::string>{"a1", "a2", "a3", "a4", "a5"});

   std::vector<std::string> b_names;
   for (auto it = table_b.begin(); it != table_b.end(); ++it)
      b_names.push_back((*it).name);
   REQUIRE(b_names == std::vector<std::string>{"b1", "b2", "b3", "b4", "b5"});

   tx.commit();
}
