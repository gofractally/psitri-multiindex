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

// ── Two structurally-different row types over the same prefix ────────────
//
// Both PSIO_REFLECT'd; they differ only in member-type AND member name to
// guarantee compute_schema_hash<>() produces distinct fingerprints. The
// test fixture stamps RowV1 first, then re-opens with RowV2 to exercise
// the strict / lenient / overwrite paths.

struct PmidxSchemaRowV1
{
   std::uint64_t id;
   std::string   value_v1;
};
PSIO_REFLECT(PmidxSchemaRowV1, id, value_v1)

struct PmidxSchemaRowV2
{
   std::uint64_t id;
   std::uint64_t value_v2;
};
PSIO_REFLECT(PmidxSchemaRowV2, id, value_v2)

namespace pmidx_schema_test
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
               ("pmidx_schema_" + std::to_string(getpid()) + "_" +
                std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using v1_table = table<PmidxSchemaRowV1,
                          ordered_unique<by_id, &PmidxSchemaRowV1::id>>;
   using v2_table = table<PmidxSchemaRowV2,
                          ordered_unique<by_id, &PmidxSchemaRowV2::id>>;
}  // namespace pmidx_schema_test

using pmidx_schema_test::by_id;
using pmidx_schema_test::fixture;
using pmidx_schema_test::v1_table;
using pmidx_schema_test::v2_table;

TEST_CASE("schema: distinct row types produce distinct schema_hash",
          "[table][schema]")
{
   const auto h1 = detail::compute_schema_hash<PmidxSchemaRowV1>();
   const auto h2 = detail::compute_schema_hash<PmidxSchemaRowV2>();
   REQUIRE(h1 != 0);
   REQUIRE(h2 != 0);
   REQUIRE(h1 != h2);
}

TEST_CASE("schema: identical row types produce identical schema_hash",
          "[table][schema]")
{
   REQUIRE(detail::compute_schema_hash<PmidxSchemaRowV1>() ==
           detail::compute_schema_hash<PmidxSchemaRowV1>());
}

TEST_CASE("schema: fresh table stamps the runtime hash",
          "[table][schema]")
{
   fixture  f;
   auto     tx = f.ws->start_transaction(0);
   v1_table rows(tx, "S/");

   const auto h = rows.header();
   REQUIRE(h.schema_hash == detail::compute_schema_hash<PmidxSchemaRowV1>());
   REQUIRE(h.schema_version == 0);

   tx.commit();
}

TEST_CASE("schema: schema_options::version stamps + retrieves",
          "[table][schema]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/", table_options{.schema = {.version = 7}});
      REQUIRE(rows.header().schema_version == 7);
      tx.commit();
   }
   // Re-opening with the same version is fine.
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/", table_options{.schema = {.version = 7}});
      REQUIRE(rows.header().schema_version == 7);
      tx.commit();
   }
}

TEST_CASE("schema: strict mode throws on hash mismatch",
          "[table][schema][strict]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/");
      rows.put(PmidxSchemaRowV1{1, "v1"});
      tx.commit();
   }

   auto tx = f.ws->start_transaction(0);
   try
   {
      v2_table rows(tx, "S/");  // strict by default
      FAIL("expected schema_mismatch");
   }
   catch (const psitri_multiindex::schema_mismatch& e)
   {
      REQUIRE(e.expected_hash !=
              detail::compute_schema_hash<PmidxSchemaRowV1>());
      REQUIRE(e.found_hash ==
              detail::compute_schema_hash<PmidxSchemaRowV1>());
   }
}

TEST_CASE("schema: lenient mode proceeds despite hash mismatch",
          "[table][schema][lenient]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/");
      rows.put(PmidxSchemaRowV1{1, "v1"});
      tx.commit();
   }

   auto     tx = f.ws->start_transaction(0);
   REQUIRE_NOTHROW(v2_table(tx, "S/", table_options{.schema = {
                                          .mode = schema_mode::lenient}}));
   tx.commit();
}

TEST_CASE("schema: overwrite mode re-stamps the runtime hash",
          "[table][schema][overwrite]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/");
      rows.put(PmidxSchemaRowV1{1, "v1"});
      tx.commit();
   }

   {
      auto     tx = f.ws->start_transaction(0);
      v2_table rows(tx, "S/", table_options{.schema = {
                                  .mode = schema_mode::overwrite}});
      REQUIRE(rows.header().schema_hash ==
              detail::compute_schema_hash<PmidxSchemaRowV2>());
      tx.commit();
   }

   // After overwrite, strict mode on V2 should now succeed (hash matches).
   {
      auto     tx = f.ws->start_transaction(0);
      REQUIRE_NOTHROW(v2_table(tx, "S/"));
      tx.commit();
   }
}

TEST_CASE("schema: forward-version always throws unless allow_forward",
          "[table][schema][forward]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/", table_options{.schema = {.version = 5}});
      tx.commit();
   }

   // Runtime expects v3 — stored is v5 (newer).
   {
      auto tx = f.ws->start_transaction(0);
      REQUIRE_THROWS_AS(
          v1_table(tx, "S/", table_options{.schema = {.version = 3}}),
          psitri_multiindex::schema_mismatch);
   }

   // allow_forward = true: opening succeeds.
   {
      auto tx = f.ws->start_transaction(0);
      REQUIRE_NOTHROW(v1_table(
          tx, "S/", table_options{.schema = {.version       = 3,
                                              .allow_forward = true}}));
      tx.commit();
   }

   // Forward-version check fires even in lenient mode (it's a separate
   // safety net from the hash check).
   {
      auto tx = f.ws->start_transaction(0);
      REQUIRE_THROWS_AS(
          v1_table(tx, "S/",
                   table_options{.schema = {.mode    = schema_mode::lenient,
                                             .version = 3}}),
          psitri_multiindex::schema_mismatch);
   }
}

TEST_CASE("schema: same-prefix re-open with same row type round-trips data",
          "[table][schema]")
{
   fixture f;
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/");
      rows.put(PmidxSchemaRowV1{42, "preserved"});
      tx.commit();
   }
   {
      auto     tx = f.ws->start_transaction(0);
      v1_table rows(tx, "S/");
      auto     v = rows.get(std::uint64_t{42});
      REQUIRE(v.has_value());
      REQUIRE(v->value_v1 == "preserved");
      tx.commit();
   }
}
