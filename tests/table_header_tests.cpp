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

struct PmidxAutoidRow
{
   std::uint64_t id;
   std::string   name;
};
PSIO_REFLECT(PmidxAutoidRow, id, name)

namespace pmidx_header_test
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
               ("pmidx_hdr_" + std::to_string(getpid()) + "_" + std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using rows_table =
       table<PmidxAutoidRow, ordered_unique<by_id, &PmidxAutoidRow::id>>;
}  // namespace pmidx_header_test

using pmidx_header_test::by_id;
using pmidx_header_test::fixture;
using pmidx_header_test::rows_table;

TEST_CASE("header: empty table reports default header", "[table][header]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   auto h = rows.header();
   REQUIRE(h.next_id == 0);
   REQUIRE(h.flags == 0);
   REQUIRE_FALSE(h.row_count.has_value());

   tx.commit();
}

TEST_CASE("insert: allocates monotonically increasing primary keys",
          "[table][header][insert]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   auto pk0 = rows.insert(PmidxAutoidRow{0, "a"});
   auto pk1 = rows.insert(PmidxAutoidRow{0, "b"});
   auto pk2 = rows.insert(PmidxAutoidRow{0, "c"});
   REQUIRE(pk0 == 0);
   REQUIRE(pk1 == 1);
   REQUIRE(pk2 == 2);

   REQUIRE(rows.header().next_id == 3);

   // Rows are reachable at their assigned ids and carry the matching id field.
   {
      auto v = rows.get(std::uint64_t{0});
      REQUIRE(v.has_value());
      REQUIRE(v->id == 0);
      REQUIRE(v->name == "a");
   }
   {
      auto v = rows.get(std::uint64_t{2});
      REQUIRE(v.has_value());
      REQUIRE(v->id == 2);
      REQUIRE(v->name == "c");
   }

   tx.commit();
}

TEST_CASE("insert: header persists across transactions",
          "[table][header][insert]")
{
   fixture f;
   {
      auto       tx = f.ws->start_transaction(0);
      rows_table rows(tx, "R/");
      rows.insert(PmidxAutoidRow{0, "first"});
      rows.insert(PmidxAutoidRow{0, "second"});
      tx.commit();
   }
   {
      auto       tx = f.ws->start_transaction(0);
      rows_table rows(tx, "R/");
      REQUIRE(rows.header().next_id == 2);
      auto pk = rows.insert(PmidxAutoidRow{0, "third"});
      REQUIRE(pk == 2);
      REQUIRE(rows.header().next_id == 3);
      tx.commit();
   }
}

TEST_CASE("insert: subtx abort rolls back the auto-id allocation",
          "[table][header][insert][subtx]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   rows.insert(PmidxAutoidRow{0, "kept"});
   REQUIRE(rows.header().next_id == 1);

   {
      auto sub = tx.sub_transaction();
      rows.insert(PmidxAutoidRow{0, "ephemeral"});
      REQUIRE(rows.header().next_id == 2);
      sub.abort();
   }

   // Header counter rewinds with the rest of the trie.
   REQUIRE(rows.header().next_id == 1);
   REQUIRE_FALSE(rows.contains(std::uint64_t{1}));

   // Next insert reuses id=1.
   auto pk = rows.insert(PmidxAutoidRow{0, "reused"});
   REQUIRE(pk == 1);
   REQUIRE(rows.header().next_id == 2);

   tx.commit();
}

TEST_CASE("row_count: disabled by default → size() is nullopt",
          "[table][header][row_count]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/");

   rows.insert(PmidxAutoidRow{0, "a"});
   rows.insert(PmidxAutoidRow{0, "b"});

   REQUIRE_FALSE(rows.size().has_value());
   tx.commit();
}

TEST_CASE("row_count: opt-in tracks insert + erase",
          "[table][header][row_count]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/", table_options{.track_row_count = true});

   REQUIRE(rows.size().has_value());
   REQUIRE(*rows.size() == 0);

   rows.insert(PmidxAutoidRow{0, "a"});
   rows.insert(PmidxAutoidRow{0, "b"});
   rows.insert(PmidxAutoidRow{0, "c"});
   REQUIRE(*rows.size() == 3);

   rows.erase(std::uint64_t{1});
   REQUIRE(*rows.size() == 2);

   // re-put with the same pk doesn't change the count
   rows.put(PmidxAutoidRow{0, "v0"});  // pk=0 already exists → update
   REQUIRE(*rows.size() == 2);

   // put with a fresh pk increments
   rows.put(PmidxAutoidRow{99, "fresh"});
   REQUIRE(*rows.size() == 3);

   tx.commit();
}

TEST_CASE("row_count: subtx abort rolls back the count", "[table][header][row_count][subtx]")
{
   fixture    f;
   auto       tx = f.ws->start_transaction(0);
   rows_table rows(tx, "R/", table_options{.track_row_count = true});

   rows.insert(PmidxAutoidRow{0, "a"});
   REQUIRE(*rows.size() == 1);

   {
      auto sub = tx.sub_transaction();
      rows.insert(PmidxAutoidRow{0, "b"});
      rows.insert(PmidxAutoidRow{0, "c"});
      REQUIRE(*rows.size() == 3);
      sub.abort();
   }

   REQUIRE(*rows.size() == 1);

   tx.commit();
}
