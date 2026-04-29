// Regression tests for the table shapes used by Spring/Antelope's native
// chainbase tables. Exercises the patterns enumerated in the audit:
//
//   - auto-id primary + scalar `name`-style secondary (uint64 wrapper)
//   - auto-id primary + composite secondary of 2-3 `name` components
//   - auto-id primary + composite secondary `(uint128, uint64)`
//   - auto-id primary + double secondary (IEEE 754 sort-by-key bytes)
//   - auto-id primary + 32-byte digest secondary, encoded via a
//     `sortable_binary_category` adapter (the path Antelope's
//     fc::sha256 / transaction_id_type / digest_type travels through)

#include <catch2/catch_all.hpp>

#include <psitri_multiindex/table.hpp>

#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>

#include <algorithm>
#include <psitri/database.hpp>
#include <psitri/database_impl.hpp>
#include <psitri/transaction.hpp>
#include <psitri/write_session.hpp>
#include <psitri/write_session_impl.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace psitri_multiindex;

// ── Antelope-style `name`: a struct wrapping a uint64 ────────────────────
// Reflected as a record so psio::key emits its single field's BE bytes —
// memcmp-sortable. This is the canonical way to lift a "named scalar"
// (account_name / table_name / scope_name) into a composite key.
struct PmidxName
{
   std::uint64_t value;
   bool operator==(const PmidxName&) const = default;
   auto operator<=>(const PmidxName&) const = default;
};
PSIO_REFLECT(PmidxName, value)

// account_index (account_object.hpp): id pk + by_name secondary on name.
struct PmidxAccount
{
   std::uint64_t id;
   PmidxName     name;
};
PSIO_REFLECT(PmidxAccount, id, name)

// permission_index pattern: pk + composite(parent, id) + composite(owner, name).
struct PmidxPermission
{
   std::uint64_t id;
   std::uint64_t parent;
   PmidxName     owner;
   PmidxName     name;
};
PSIO_REFLECT(PmidxPermission, id, parent, owner, name)

// table_id_multi_index pattern: pk + composite(code, scope, table) of names.
struct PmidxTableId
{
   std::uint64_t id;
   PmidxName     code;
   PmidxName     scope;
   PmidxName     table;
};
PSIO_REFLECT(PmidxTableId, id, code, scope, table)

// generated_transaction pattern: pk + (sender_id: uint128 + account: name).
struct PmidxGeneratedTrx
{
   std::uint64_t  id;
   PmidxName      sender;
   psio::uint128  sender_id;
};
PSIO_REFLECT(PmidxGeneratedTrx, id, sender, sender_id)

// index_double pattern: pk + double secondary.
struct PmidxIndexDouble
{
   std::uint64_t id;
   double        secondary_key;
};
PSIO_REFLECT(PmidxIndexDouble, id, secondary_key)

// index_long_double pattern: pk + float128 secondary. Uses psio's
// 16-byte float128 type, which has built-in sortable_binary_category
// and binary_category adapters — psio::key applies the IEEE 754 sort
// transform, pssz writes 16 raw bytes.
struct PmidxIndexLongDouble
{
   std::uint64_t  id;
   psio::float128 secondary_key;
};
PSIO_REFLECT(PmidxIndexLongDouble, id, secondary_key)

// fc::sha256 / transaction_id_type stand-in: 32 raw bytes, treated as
// memcmp-sortable via a sortable_binary_category adapter. Fixed-width,
// so the wire and sort encodings coincide and we register the same
// codec in both slots — that's what would let the same digest type be
// embedded in both a pSSZ row body AND a sort key.
struct PmidxDigest
{
   std::array<std::uint8_t, 32> bytes{};
   bool operator==(const PmidxDigest&) const = default;
   auto operator<=>(const PmidxDigest&) const = default;
};

struct pmidx_digest_codec
{
   static std::size_t packsize(const PmidxDigest&) noexcept { return 32; }
   static void encode(const PmidxDigest& d, std::vector<char>& s)
   {
      s.insert(s.end(), reinterpret_cast<const char*>(d.bytes.data()),
               reinterpret_cast<const char*>(d.bytes.data()) + 32);
   }
   static PmidxDigest decode(std::span<const char> b) noexcept
   {
      PmidxDigest d;
      std::memcpy(d.bytes.data(), b.data(), 32);
      return d;
   }
   static psio::codec_status validate(std::span<const char> b) noexcept
   {
      return b.size() < 32
                 ? psio::codec_fail("PmidxDigest: short buffer", 0, "digest")
                 : psio::codec_ok();
   }
   static psio::codec_status validate_strict(std::span<const char> b) noexcept
   {
      return validate(b);
   }
};

PSIO_ADAPTER(PmidxDigest, psio::binary_category,          pmidx_digest_codec)
PSIO_ADAPTER(PmidxDigest, psio::sortable_binary_category, pmidx_digest_codec)

// transaction_multi_index pattern: pk + by_trx_id (32-byte digest).
struct PmidxTransaction
{
   std::uint64_t id;
   PmidxDigest   trx_id;
};
PSIO_REFLECT(PmidxTransaction, id, trx_id)

namespace antelope_shapes
{
   struct by_id;
   struct by_name;
   struct by_parent_id;
   struct by_owner;
   struct by_code_scope_table;
   struct by_sender_id;
   struct by_secondary;
   struct by_trx_id;

   struct fixture
   {
      std::filesystem::path                  dir;
      std::shared_ptr<psitri::database>      db;
      std::shared_ptr<psitri::write_session> ws;

      fixture()
      {
         const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
         dir           = std::filesystem::temp_directory_path() /
               ("pmidx_antelope_" + std::to_string(getpid()) + "_" +
                std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };
}  // namespace antelope_shapes

using antelope_shapes::by_code_scope_table;
using antelope_shapes::by_id;
using antelope_shapes::by_name;
using antelope_shapes::by_owner;
using antelope_shapes::by_parent_id;
using antelope_shapes::by_secondary;
using antelope_shapes::by_sender_id;
using antelope_shapes::by_trx_id;
using antelope_shapes::fixture;

// ── account_index ────────────────────────────────────────────────────────
//
// auto-id primary + ordered_unique<by_name> on the wrapped uint64 name.

TEST_CASE("antelope: account_index — auto-id + by_name on uint64 wrapper",
          "[antelope][account]")
{
   using accounts =
       table<PmidxAccount,
             ordered_unique<by_id,   &PmidxAccount::id>,
             ordered_unique<by_name, &PmidxAccount::name>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   accounts t(tx, "acc/");

   t.insert(PmidxAccount{0, PmidxName{2}});
   t.insert(PmidxAccount{0, PmidxName{1}});
   t.insert(PmidxAccount{0, PmidxName{3}});

   // by_name is sortable: 1 < 2 < 3 → first is the row with name.value=1.
   auto it = t.begin<by_name>();
   REQUIRE(it != t.end<by_name>());
   REQUIRE((*it).name.value == 1);
   ++it;
   REQUIRE((*it).name.value == 2);

   auto v = t.find<by_name>(PmidxName{2});
   REQUIRE(v.has_value());
   REQUIRE(v->id == 0);

   tx.commit();
}

// ── permission_index ────────────────────────────────────────────────────
//
// auto-id primary + composite secondaries.
//   - by_parent_id: composite(parent, id)  — non-unique (parent repeats)
//   - by_owner:     composite(owner, name) — unique

TEST_CASE("antelope: permission_index — composite secondaries on names + ids",
          "[antelope][permission]")
{
   using perms =
       table<PmidxPermission,
             ordered_unique    <by_id,        &PmidxPermission::id>,
             ordered_non_unique<by_parent_id, composite_key<&PmidxPermission::parent,
                                                            &PmidxPermission::id>{}>,
             ordered_unique    <by_owner,     composite_key<&PmidxPermission::owner,
                                                            &PmidxPermission::name>{}>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   perms   t(tx, "p/");

   // 3 rows under parent=10, plus one under parent=20.
   t.insert(PmidxPermission{0, 10, PmidxName{1}, PmidxName{100}});
   t.insert(PmidxPermission{0, 10, PmidxName{2}, PmidxName{200}});
   t.insert(PmidxPermission{0, 20, PmidxName{1}, PmidxName{101}});
   t.insert(PmidxPermission{0, 10, PmidxName{1}, PmidxName{300}});

   // by_parent_id: tuples ordered by (parent, id). Parent=10 block has
   // ids 0, 1, 3 in that order.
   std::vector<std::uint64_t> parent10_ids;
   for (auto&& r : t.equal_range<by_parent_id>(std::uint64_t{10}))
      parent10_ids.push_back(r.id);
   REQUIRE(parent10_ids == std::vector<std::uint64_t>{0, 1, 3});

   // by_owner is unique on (owner, name). Lookup pairs.
   auto v = t.find<by_owner>(
       std::make_tuple(PmidxName{1}, PmidxName{100}));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 0);

   // Different (owner, name) → different row.
   auto v2 = t.find<by_owner>(
       std::make_tuple(PmidxName{1}, PmidxName{300}));
   REQUIRE(v2.has_value());
   REQUIRE(v2->id == 3);

   // Collision: same (owner, name) on a fresh row (different id) is rejected.
   REQUIRE_THROWS_AS(
       t.insert(PmidxPermission{0, 99, PmidxName{1}, PmidxName{100}}),
       psitri_multiindex::secondary_collision);

   tx.commit();
}

// ── table_id_multi_index ─────────────────────────────────────────────────
//
// 3-component composite secondary on name types.

TEST_CASE("antelope: table_id_multi_index — 3-name composite secondary",
          "[antelope][table_id]")
{
   using table_ids = table<
       PmidxTableId,
       ordered_unique<by_id,               &PmidxTableId::id>,
       ordered_unique<by_code_scope_table, composite_key<&PmidxTableId::code,
                                                         &PmidxTableId::scope,
                                                         &PmidxTableId::table>{}>>;

   fixture   f;
   auto      tx = f.ws->start_transaction(0);
   table_ids t(tx, "t/");

   t.insert(PmidxTableId{0, PmidxName{1}, PmidxName{1}, PmidxName{1}});
   t.insert(PmidxTableId{0, PmidxName{1}, PmidxName{1}, PmidxName{2}});
   t.insert(PmidxTableId{0, PmidxName{1}, PmidxName{2}, PmidxName{1}});
   t.insert(PmidxTableId{0, PmidxName{2}, PmidxName{1}, PmidxName{1}});

   // 4 distinct triples → 4 rows discoverable in code-scope-table order.
   std::vector<std::uint64_t> ordered;
   for (auto it = t.begin<by_code_scope_table>();
        it != t.end<by_code_scope_table>(); ++it)
      ordered.push_back((*it).id);
   REQUIRE(ordered == std::vector<std::uint64_t>{0, 1, 2, 3});

   // Exact lookup on the triple.
   auto v = t.find<by_code_scope_table>(
       std::make_tuple(PmidxName{1}, PmidxName{2}, PmidxName{1}));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 2);

   // Collision attempt with the same triple at a different pk.
   REQUIRE_THROWS_AS(
       t.insert(PmidxTableId{0, PmidxName{1}, PmidxName{1}, PmidxName{1}}),
       psitri_multiindex::secondary_collision);

   tx.commit();
}

// ── generated_transaction shape with uint128 component ───────────────────

TEST_CASE("antelope: generated_transaction — composite(name, uint128)",
          "[antelope][generated_transaction]")
{
   using gen = table<
       PmidxGeneratedTrx,
       ordered_unique<by_id,        &PmidxGeneratedTrx::id>,
       ordered_unique<by_sender_id, composite_key<&PmidxGeneratedTrx::sender,
                                                  &PmidxGeneratedTrx::sender_id>{}>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   gen     t(tx, "gx/");

   const psio::uint128 lo{0};
   const psio::uint128 mid = (psio::uint128{1} << 64);  // beyond 64-bit range
   const psio::uint128 hi  = ~psio::uint128{0};         // max

   t.insert(PmidxGeneratedTrx{0, PmidxName{1}, lo});
   t.insert(PmidxGeneratedTrx{0, PmidxName{1}, mid});
   t.insert(PmidxGeneratedTrx{0, PmidxName{1}, hi});

   // by_sender_id orders by (sender, sender_id). All under sender=1, so
   // we walk in sender_id ascending order: lo, mid, hi.
   std::vector<std::uint64_t> walk;
   for (auto it = t.begin<by_sender_id>(); it != t.end<by_sender_id>(); ++it)
      walk.push_back((*it).id);
   REQUIRE(walk == std::vector<std::uint64_t>{0, 1, 2});

   // Exact lookup.
   auto v = t.find<by_sender_id>(std::make_tuple(PmidxName{1}, mid));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 1);

   tx.commit();
}

// ── index_double shape ───────────────────────────────────────────────────
//
// psio::key encodes double with sign-flipped big-endian; memcmp on those
// bytes matches IEEE 754 less-than for normal (non-NaN) doubles. This is
// the encoding `index_double_index` would land on once ported. We don't
// support the soft_double_less comparator directly, but we get the same
// ordering for finite values out of the existing key encoding.

TEST_CASE("antelope: index_double — by_secondary sorts IEEE 754 doubles",
          "[antelope][index_double]")
{
   using idouble = table<
       PmidxIndexDouble,
       ordered_unique<by_id,        &PmidxIndexDouble::id>,
       ordered_unique<by_secondary, &PmidxIndexDouble::secondary_key>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   idouble t(tx, "id/");

   const std::vector<double> vs = {
       -1e300,
       -1.5,
       -0.001,
       0.0,
       0.001,
       1.5,
       1e300,
   };
   for (std::size_t i = 0; i < vs.size(); ++i)
      t.insert(PmidxIndexDouble{0, vs[i]});

   std::vector<double> walked;
   for (auto it = t.begin<by_secondary>(); it != t.end<by_secondary>(); ++it)
      walked.push_back((*it).secondary_key);
   REQUIRE(walked == vs);  // already inserted in ascending order

   // lower_bound at 0.0 — first row with secondary >= 0.0.
   auto it = t.lower_bound<by_secondary>(0.0);
   REQUIRE(it != t.end<by_secondary>());
   REQUIRE((*it).secondary_key == 0.0);

   tx.commit();
}

// ── transaction_multi_index — by_trx_id on a 32-byte digest ──────────────
//
// The digest type is non-reflected; psio::key dispatches into its
// `sortable_binary_category` adapter for both encoding and ordering.
// This is the path Antelope's fc::sha256 / transaction_id_type land on.

TEST_CASE("antelope: transaction_multi_index — by_trx_id on a 32-byte digest",
          "[antelope][transaction][digest]")
{
   using trxs = table<
       PmidxTransaction,
       ordered_unique<by_id,     &PmidxTransaction::id>,
       ordered_unique<by_trx_id, &PmidxTransaction::trx_id>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   trxs    t(tx, "trx/");

   PmidxDigest d_a{}, d_b{}, d_c{};
   d_a.bytes[0]  = 0x10;
   d_b.bytes[0]  = 0x20;
   d_c.bytes[31] = 0xFF;  // sorts after d_a, d_b (first byte is zero)

   t.insert(PmidxTransaction{0, d_b});
   t.insert(PmidxTransaction{0, d_a});
   t.insert(PmidxTransaction{0, d_c});

   // Tag walk: d_c (0x00..FF) < d_a (0x10..) < d_b (0x20..) in
   // memcmp order over 32 bytes — first byte differs only for a, b;
   // c starts with 0x00 so sorts first.
   std::vector<std::uint64_t> ids;
   for (auto it = t.begin<by_trx_id>(); it != t.end<by_trx_id>(); ++it)
      ids.push_back((*it).id);
   REQUIRE(ids.size() == 3);
   // The order is by digest bytes lexicographically.
   auto trxs_in_order =
       std::vector<PmidxDigest>{d_c, d_a, d_b};
   for (std::size_t i = 0; i < ids.size(); ++i)
   {
      auto v = t.get(ids[i]);
      REQUIRE(v.has_value());
      REQUIRE(v->trx_id == trxs_in_order[i]);
   }

   // Exact digest lookup.
   auto v = t.find<by_trx_id>(d_a);
   REQUIRE(v.has_value());
   REQUIRE(v->trx_id == d_a);

   // Collision: same digest at a different pk — must throw.
   REQUIRE_THROWS_AS(t.insert(PmidxTransaction{0, d_a}),
                     psitri_multiindex::secondary_collision);

   tx.commit();
}

// ── index_long_double_index — by_secondary on float128 ───────────────────
//
// Spring's `index_long_double_index` uses Berkeley softfloat's
// `float128_t` for deterministic IEEE 754 binary128 sorting. psio's
// `float128` is layout-compatible (16 bytes, two u64 limbs); the
// sortable_binary_category adapter applies the standard sign-flip /
// bit-not transform so memcmp matches f128_lt for finite non-NaN
// values. -0.0 canonicalises to +0.0 in the sort key.

namespace {
   psio::float128 from_double(double d)
   {
      std::uint64_t bits;
      std::memcpy(&bits, &d, 8);
      const std::uint64_t sign   = (bits >> 63) & 1;
      const std::uint64_t exp64  = (bits >> 52) & 0x7ff;
      const std::uint64_t frac64 = bits & ((1ull << 52) - 1);
      psio::float128      out{};
      if (exp64 == 0 && frac64 == 0)
      {
         out.limb[1] = sign << 63;
         return out;
      }
      const std::uint64_t exp128 = exp64 + (16383 - 1023);
      out.limb[1] = (sign << 63) | (exp128 << 48) | (frac64 >> 4);
      out.limb[0] = (frac64 & 0xf) << 60;
      return out;
   }
}  // namespace

TEST_CASE("antelope: index_long_double_index — by_secondary on float128",
          "[antelope][index_long_double]")
{
   using ild = table<
       PmidxIndexLongDouble,
       ordered_unique<by_id,        &PmidxIndexLongDouble::id>,
       ordered_unique<by_secondary, &PmidxIndexLongDouble::secondary_key>>;

   fixture f;
   auto    tx = f.ws->start_transaction(0);
   ild     t(tx, "ild/");

   // Insert in arbitrary order; iteration must come out sorted by IEEE
   // 754 ordering on the secondary key.
   const std::vector<double> ds = {
       1e100, -1.5, 0.0, 3.14, -1e-300, -1e100, 1e-300, 2.5,
   };
   for (double x : ds)
      t.insert(PmidxIndexLongDouble{0, from_double(x)});

   std::vector<double> sorted_in = ds;
   std::sort(sorted_in.begin(), sorted_in.end());

   std::vector<psio::float128> walked;
   for (auto it = t.begin<by_secondary>(); it != t.end<by_secondary>(); ++it)
      walked.push_back((*it).secondary_key);
   REQUIRE(walked.size() == ds.size());
   for (std::size_t i = 0; i < walked.size(); ++i)
      REQUIRE(walked[i] == from_double(sorted_in[i]));

   // lower_bound at zero — first row with secondary >= 0.0.
   auto it = t.lower_bound<by_secondary>(from_double(0.0));
   REQUIRE(it != t.end<by_secondary>());
   REQUIRE((*it).secondary_key == from_double(0.0));

   tx.commit();
}
