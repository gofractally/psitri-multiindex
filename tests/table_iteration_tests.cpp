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

struct PmidxIterRow
{
   std::uint64_t id;
   std::string   name;
};
PSIO_REFLECT(PmidxIterRow, id, name)

namespace pmidx_iter_test
{
   struct by_id;

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

   using rows_table =
       table<PmidxIterRow, ordered_unique<by_id, &PmidxIterRow::id>>;
}  // namespace pmidx_iter_test

using pmidx_iter_test::by_id;
using pmidx_iter_test::fixture;
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
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k : {std::uint64_t{5}, std::uint64_t{1}, std::uint64_t{3},
                           std::uint64_t{2}, std::uint64_t{4}})
      rows.put(PmidxIterRow{k, "v" + std::to_string(k)});

   std::vector<std::uint64_t> seen;
   for (auto it = rows.begin(); it != rows.end(); ++it)
      seen.push_back((*it).id);

   REQUIRE(seen == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
   tx.commit();
}

TEST_CASE("iter: lower_bound returns iterator at >= key",
          "[table][iter][bounds]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 10; k <= 50; k += 10)
      rows.put(PmidxIterRow{k, ""});

   {
      auto it = rows.lower_bound(std::uint64_t{25});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 30);
   }
   {
      auto it = rows.lower_bound(std::uint64_t{30});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 30);
   }
   {
      auto it = rows.lower_bound(std::uint64_t{60});
      REQUIRE(it == rows.end());
   }
   tx.commit();
}

TEST_CASE("iter: upper_bound returns iterator at > key",
          "[table][iter][bounds]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 10; k <= 50; k += 10)
      rows.put(PmidxIterRow{k, ""});

   {
      auto it = rows.upper_bound(std::uint64_t{30});
      REQUIRE(it != rows.end());
      REQUIRE((*it).id == 40);
   }
   {
      auto it = rows.upper_bound(std::uint64_t{50});
      REQUIRE(it == rows.end());
   }
   tx.commit();
}

TEST_CASE("iter: range iteration via lower_bound + upper_bound",
          "[table][iter][bounds]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 1; k <= 20; ++k)
      rows.put(PmidxIterRow{k, ""});

   std::vector<std::uint64_t> seen;
   auto it  = rows.lower_bound(std::uint64_t{5});
   auto end = rows.upper_bound(std::uint64_t{15});
   for (; it != end; ++it)
      seen.push_back((*it).id);
   REQUIRE(seen == std::vector<std::uint64_t>{5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
   tx.commit();
}

TEST_CASE("iter: zero-copy view via value_pin",
          "[table][iter][zero-copy]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 1; k <= 5; ++k)
      rows.put(PmidxIterRow{k, "row_" + std::to_string(k)});

   // pin_values returns a value_pin that pins the segment for the
   // remaining iteration. View bytes are valid as long as `pin` lives.
   {
      auto pin = rows.pin_values();
      std::vector<std::uint64_t> seen;
      for (auto it = rows.begin(); it != rows.end(); ++it)
      {
         auto v = it.view(pin);                           // zero-copy
         seen.push_back(v.template get<0>());             // arithmetic by value
      }
      REQUIRE(seen == std::vector<std::uint64_t>{1, 2, 3, 4, 5});
   }

   tx.commit();
}

TEST_CASE("iter: seek re-positions an existing iterator", "[table][iter][seek]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k : {std::uint64_t{10}, std::uint64_t{20}, std::uint64_t{30},
                           std::uint64_t{40}, std::uint64_t{50}})
      rows.put(PmidxIterRow{k, "v" + std::to_string(k)});

   auto it = rows.begin();
   REQUIRE(it != rows.end());
   REQUIRE((*it).id == 10);

   // Re-seek forward.
   it.seek(std::uint64_t{30});
   REQUIRE(it != rows.end());
   REQUIRE((*it).id == 30);

   // Re-seek backward.
   it.seek(std::uint64_t{15});
   REQUIRE(it != rows.end());
   REQUIRE((*it).id == 20);

   // Re-seek past last.
   it.seek(std::uint64_t{99});
   REQUIRE(it == rows.end());

   // Re-seek back into range from end.
   it.seek(std::uint64_t{40});
   REQUIRE(it != rows.end());
   REQUIRE((*it).id == 40);

   tx.commit();
}

TEST_CASE("iter: pin keeps captured views readable after the iterator advances",
          "[table][iter][zero-copy][pin]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   for (std::uint64_t k = 1; k <= 5; ++k)
      rows.put(PmidxIterRow{k, "row_" + std::to_string(k)});

   // Pin once, capture every row's view during the walk, then check each
   // captured view AFTER iteration has finished. This exercises the
   // contract that pin holds the segments mapped — the bytes the views
   // point at must still be readable as long as no mutation has happened
   // and the pin is alive.
   //
   // value_pin is non-movable, so the pin lives as a local; the captured
   // views live in a vector alongside it.
   auto pin = rows.pin_values();

   using view_t = rows_table::primary_iterator::value_view;
   std::vector<view_t> views;
   for (auto it = rows.begin(); it != rows.end(); ++it)
      views.push_back(it.view(pin));

   REQUIRE(views.size() == 5);
   for (std::size_t i = 0; i < views.size(); ++i)
      REQUIRE(views[i].template get<0>() == i + 1);

   tx.commit();
}

TEST_CASE("iter: stops at table prefix boundary", "[table][iter][isolation]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table table_a(tx, "A/");
   rows_table table_b(tx, "B/");

   for (std::uint64_t k = 1; k <= 5; ++k)
      table_a.put(PmidxIterRow{k, "a" + std::to_string(k)});
   for (std::uint64_t k = 1; k <= 5; ++k)
      table_b.put(PmidxIterRow{k, "b" + std::to_string(k)});

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
