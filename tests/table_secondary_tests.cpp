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

struct PmidxUser
{
   std::uint64_t id;        // primary
   std::string   name;      // unique secondary
   std::uint64_t group_id;  // non-unique secondary
};
PSIO_REFLECT(PmidxUser, id, name, group_id)

namespace pmidx_sec_test
{
   struct by_id;
   struct by_name;
   struct by_group;

   struct fixture
   {
      std::filesystem::path                  dir;
      std::shared_ptr<psitri::database>      db;
      std::shared_ptr<psitri::write_session> ws;

      fixture()
      {
         const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
         dir           = std::filesystem::temp_directory_path() /
               ("pmidx_sec_" + std::to_string(getpid()) + "_" + std::to_string(ts));
         std::filesystem::remove_all(dir);
         std::filesystem::create_directories(dir);
         db = psitri::database::open(dir);
         ws = db->start_write_session();
      }
      ~fixture() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
   };

   using users_table = table<PmidxUser,
                             ordered_unique<by_id,    &PmidxUser::id>,
                             ordered_unique<by_name,  &PmidxUser::name>,
                             ordered_non_unique<by_group, &PmidxUser::group_id>>;
}  // namespace pmidx_sec_test

using pmidx_sec_test::by_group;
using pmidx_sec_test::by_id;
using pmidx_sec_test::by_name;
using pmidx_sec_test::fixture;
using pmidx_sec_test::users_table;

TEST_CASE("named-tag: find<by_name> on a unique secondary",
          "[table][secondary][named-tag]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});

   auto v = users.find<by_name>(std::string("bob"));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 2);
   REQUIRE(v->name == "bob");

   tx.commit();
}

TEST_CASE("named-tag: find<by_name> on missing key returns nullopt",
          "[table][secondary][named-tag]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");
   users.put(PmidxUser{1, "alice", 100});
   REQUIRE_FALSE(users.find<by_name>(std::string("nonexistent")).has_value());
   tx.commit();
}

TEST_CASE("named-tag: replace updates the secondary index",
          "[table][secondary][named-tag]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{1, "alice2", 100});  // rename

   REQUIRE_FALSE(users.find<by_name>(std::string("alice")).has_value());
   auto v = users.find<by_name>(std::string("alice2"));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 1);

   tx.commit();
}

TEST_CASE("named-tag: erase clears all secondary entries",
          "[table][secondary][named-tag]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   REQUIRE(users.find<by_name>(std::string("alice")).has_value());

   users.erase(std::uint64_t{1});
   REQUIRE_FALSE(users.find<by_name>(std::string("alice")).has_value());

   tx.commit();
}

TEST_CASE("ordered_non_unique<by_group>: equal_range yields all matching rows",
          "[table][secondary][non-unique]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});
   users.put(PmidxUser{4, "dave",  100});

   std::vector<std::uint64_t> in_group_100;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{100}))
      in_group_100.push_back(u.id);

   // (group, id) tree order → ids 1, 2, 4 in ascending order.
   REQUIRE(in_group_100 == std::vector<std::uint64_t>{1, 2, 4});

   std::vector<std::uint64_t> in_group_200;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{200}))
      in_group_200.push_back(u.id);
   REQUIRE(in_group_200 == std::vector<std::uint64_t>{3});

   std::vector<std::uint64_t> in_group_999;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{999}))
      in_group_999.push_back(u.id);
   REQUIRE(in_group_999.empty());

   tx.commit();
}

TEST_CASE("ordered_non_unique<by_group>: find returns first matching row",
          "[table][secondary][non-unique]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{5, "eve",   100});
   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{3, "carol", 100});

   auto v = users.find<by_group>(std::uint64_t{100});
   REQUIRE(v.has_value());
   REQUIRE(v->id == 1);  // ids 1, 3, 5 in tree order — lowest pk wins

   tx.commit();
}

TEST_CASE("ordered_unique<by_name>: collision rejected",
          "[table][secondary][unique][collision]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});

   // Different primary, same unique secondary — must throw.
   REQUIRE_THROWS_AS(
       users.put(PmidxUser{2, "alice", 200}),
       psitri_multiindex::secondary_collision);

   // First row is intact.
   auto v = users.find<by_id>(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "alice");

   tx.commit();
}

TEST_CASE("ordered_unique<by_name>: same-pk re-put is fine",
          "[table][secondary][unique]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   REQUIRE_NOTHROW(users.put(PmidxUser{1, "alice", 100}));   // identical
   REQUIRE_NOTHROW(users.put(PmidxUser{1, "alice", 200}));   // same pk, different group

   auto v = users.find<by_id>(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->group_id == 200);

   tx.commit();
}

TEST_CASE("ordered_unique<by_name>: collision on update rejected",
          "[table][secondary][unique][collision]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   200});

   // Try to rename bob → alice. Collides with id=1's name.
   REQUIRE_THROWS_AS(
       users.put(PmidxUser{2, "alice", 200}),
       psitri_multiindex::secondary_collision);

   tx.commit();
}

TEST_CASE("put: collision on update is atomic — no partial secondary state",
          "[table][secondary][unique][collision][atomic]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   200});

   // Rename bob → alice with a group-id change too. The by_name change
   // collides; pre-validation must abort BEFORE removing bob's existing
   // by_name entry or rewriting bob's by_group entry.
   REQUIRE_THROWS_AS(users.put(PmidxUser{2, "alice", 999}),
                     psitri_multiindex::secondary_collision);

   // Both rows still resolve unchanged via every index.
   {
      auto a = users.find<by_name>(std::string("alice"));
      REQUIRE(a.has_value());
      REQUIRE(a->id == 1);
      REQUIRE(a->group_id == 100);
   }
   {
      auto b = users.find<by_name>(std::string("bob"));
      REQUIRE(b.has_value());
      REQUIRE(b->id == 2);
      REQUIRE(b->group_id == 200);
   }

   // by_group still has the old (group=200, pk=2) entry — not 999.
   std::vector<std::uint64_t> g200;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{200}))
      g200.push_back(u.id);
   REQUIRE(g200 == std::vector<std::uint64_t>{2});

   std::vector<std::uint64_t> g999;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{999}))
      g999.push_back(u.id);
   REQUIRE(g999.empty());

   tx.commit();
}

TEST_CASE("put: collision on insert is atomic — no partial secondary state",
          "[table][secondary][unique][collision][atomic]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});

   // New pk=2 with name=alice (collides) and group=999. No primary row
   // exists for pk=2, so the by_group entry is fresh — pre-validation
   // must reject before writing the (group=999, pk=2) entry.
   REQUIRE_THROWS_AS(users.put(PmidxUser{2, "alice", 999}),
                     psitri_multiindex::secondary_collision);

   REQUIRE_FALSE(users.find<by_id>(std::uint64_t{2}).has_value());

   std::vector<std::uint64_t> g999;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{999}))
      g999.push_back(u.id);
   REQUIRE(g999.empty());

   tx.commit();
}

TEST_CASE("tag-iter: begin<Tag>/end<Tag> over a unique secondary",
          "[table][secondary][tag-iter]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{3, "carol", 100});
   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});

   std::vector<std::string> by_name_order;
   for (auto it = users.begin<by_name>(); it != users.end<by_name>(); ++it)
      by_name_order.push_back((*it).name);
   REQUIRE(by_name_order == std::vector<std::string>{"alice", "bob", "carol"});

   tx.commit();
}

TEST_CASE("tag-iter: begin<Tag>/end<Tag> over a non-unique secondary",
          "[table][secondary][tag-iter]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   // Mixed insertion order; tag iteration walks (group, pk) in tree order.
   users.put(PmidxUser{2, "bob",   200});
   users.put(PmidxUser{4, "dave",  100});
   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{3, "carol", 200});

   std::vector<std::pair<std::uint64_t, std::uint64_t>> by_group_order;
   for (auto it = users.begin<by_group>(); it != users.end<by_group>(); ++it)
   {
      auto u = *it;
      by_group_order.emplace_back(u.group_id, u.id);
   }
   REQUIRE(by_group_order ==
           (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
               {100, 1}, {100, 4}, {200, 2}, {200, 3}}));

   tx.commit();
}

TEST_CASE("tag-iter: lower_bound<Tag> on a unique secondary",
          "[table][secondary][tag-iter][bounds]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice",  100});
   users.put(PmidxUser{2, "bob",    100});
   users.put(PmidxUser{3, "carol",  100});
   users.put(PmidxUser{4, "dave",   100});

   {
      auto it = users.lower_bound<by_name>(std::string("bob"));
      REQUIRE(it != users.end<by_name>());
      REQUIRE((*it).name == "bob");
   }
   {
      auto it = users.lower_bound<by_name>(std::string("biz"));
      REQUIRE(it != users.end<by_name>());
      REQUIRE((*it).name == "bob");
   }
   {
      auto it = users.lower_bound<by_name>(std::string("zz"));
      REQUIRE(it == users.end<by_name>());
   }

   tx.commit();
}

TEST_CASE("tag-iter: upper_bound<Tag> on a unique secondary",
          "[table][secondary][tag-iter][bounds]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice",  100});
   users.put(PmidxUser{2, "bob",    100});
   users.put(PmidxUser{3, "carol",  100});

   {
      auto it = users.upper_bound<by_name>(std::string("alice"));
      REQUIRE(it != users.end<by_name>());
      REQUIRE((*it).name == "bob");
   }
   {
      auto it = users.upper_bound<by_name>(std::string("carol"));
      REQUIRE(it == users.end<by_name>());
   }

   tx.commit();
}

TEST_CASE("tag-iter: upper_bound<Tag> on a non-unique secondary skips block",
          "[table][secondary][tag-iter][bounds]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});
   users.put(PmidxUser{4, "dave",  300});

   {
      // upper_bound(100) lands on the first row with group > 100, which is
      // (group=200, pk=3) — NOT (group=100, pk=2).
      auto it = users.upper_bound<by_group>(std::uint64_t{100});
      REQUIRE(it != users.end<by_group>());
      auto u = *it;
      REQUIRE(u.group_id == 200);
      REQUIRE(u.id == 3);
   }
   {
      auto it = users.upper_bound<by_group>(std::uint64_t{300});
      REQUIRE(it == users.end<by_group>());
   }

   tx.commit();
}

TEST_CASE("tag-iter: equal_range<Tag> on a unique secondary returns 0 or 1",
          "[table][secondary][tag-iter][bounds]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   200});

   {
      std::vector<std::uint64_t> hits;
      for (auto&& u : users.equal_range<by_name>(std::string("alice")))
         hits.push_back(u.id);
      REQUIRE(hits == std::vector<std::uint64_t>{1});
   }
   {
      std::vector<std::uint64_t> hits;
      for (auto&& u : users.equal_range<by_name>(std::string("missing")))
         hits.push_back(u.id);
      REQUIRE(hits.empty());
   }

   tx.commit();
}

TEST_CASE("tag-iter: secondary_iterator::seek re-positions within a tag",
          "[table][secondary][tag-iter][seek]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});
   users.put(PmidxUser{4, "dave",  100});
   users.put(PmidxUser{5, "eve",   300});

   // Seek over a unique secondary.
   {
      auto it = users.begin<by_name>();
      REQUIRE((*it).name == "alice");
      it.seek(std::string("carol"));
      REQUIRE((*it).name == "carol");
      it.seek(std::string("zz"));
      REQUIRE(it == users.end<by_name>());
   }

   // Seek over a non-unique secondary; jumps across (sk, *) blocks.
   {
      auto it = users.begin<by_group>();
      REQUIRE((*it).group_id == 100);  // first row in group=100 block
      it.seek(std::uint64_t{200});
      REQUIRE((*it).group_id == 200);
      it.seek(std::uint64_t{150});     // first row with group >= 150 → 200
      REQUIRE((*it).group_id == 200);
      it.seek(std::uint64_t{999});
      REQUIRE(it == users.end<by_group>());
   }

   tx.commit();
}

TEST_CASE("tag-iter: contains<Tag> probes without decoding the row",
          "[table][secondary][tag-iter][contains]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});

   REQUIRE(users.contains<by_id>(std::uint64_t{1}));
   REQUIRE_FALSE(users.contains<by_id>(std::uint64_t{99}));

   REQUIRE(users.contains<by_name>(std::string("alice")));
   REQUIRE_FALSE(users.contains<by_name>(std::string("missing")));

   REQUIRE(users.contains<by_group>(std::uint64_t{100}));     // non-unique, has rows
   REQUIRE_FALSE(users.contains<by_group>(std::uint64_t{999}));  // empty group

   tx.commit();
}

TEST_CASE("tag-iter: count<Tag> reports the right cardinality",
          "[table][secondary][tag-iter][count]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});
   users.put(PmidxUser{4, "dave",  100});

   REQUIRE(users.count<by_id>(std::uint64_t{1}) == 1);
   REQUIRE(users.count<by_id>(std::uint64_t{99}) == 0);
   REQUIRE(users.count<by_name>(std::string("alice")) == 1);
   REQUIRE(users.count<by_name>(std::string("missing")) == 0);
   REQUIRE(users.count<by_group>(std::uint64_t{100}) == 3);
   REQUIRE(users.count<by_group>(std::uint64_t{200}) == 1);
   REQUIRE(users.count<by_group>(std::uint64_t{999}) == 0);

   tx.commit();
}

TEST_CASE("modify: callback mutates the row and updates indexes",
          "[table][modify]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});

   const bool ok = users.modify(std::uint64_t{1}, [](PmidxUser& u) {
      u.name     = "alice2";
      u.group_id = 999;
   });
   REQUIRE(ok);

   REQUIRE_FALSE(users.contains<by_name>(std::string("alice")));
   auto v = users.find<by_name>(std::string("alice2"));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 1);
   REQUIRE(v->group_id == 999);

   // Missing pk → false, callback not invoked.
   const bool ok2 =
       users.modify(std::uint64_t{99}, [](PmidxUser& u) { u.name = "x"; });
   REQUIRE_FALSE(ok2);

   tx.commit();
}

TEST_CASE("modify: callback changing the primary key throws",
          "[table][modify]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});

   REQUIRE_THROWS_AS(users.modify(std::uint64_t{1},
                                  [](PmidxUser& u) { u.id = 42; }),
                     std::logic_error);

   tx.commit();
}

TEST_CASE("erase_range: secondaries are cleaned up for every erased row",
          "[table][secondary][erase_range]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   users.put(PmidxUser{2, "bob",   100});
   users.put(PmidxUser{3, "carol", 200});
   users.put(PmidxUser{4, "dave",  100});

   // Erase ids in [2, 4) — bob and carol.
   const auto erased = users.erase_range(std::uint64_t{2}, std::uint64_t{4});
   REQUIRE(erased == 2);

   REQUIRE(users.contains<by_id>(std::uint64_t{1}));
   REQUIRE_FALSE(users.contains<by_id>(std::uint64_t{2}));
   REQUIRE_FALSE(users.contains<by_id>(std::uint64_t{3}));
   REQUIRE(users.contains<by_id>(std::uint64_t{4}));

   // Unique secondary entries for the erased rows are gone.
   REQUIRE_FALSE(users.contains<by_name>(std::string("bob")));
   REQUIRE_FALSE(users.contains<by_name>(std::string("carol")));
   REQUIRE(users.contains<by_name>(std::string("alice")));

   // Non-unique secondary's group=100 block now only references {1, 4}.
   std::vector<std::uint64_t> g100;
   for (auto&& u : users.equal_range<by_group>(std::uint64_t{100}))
      g100.push_back(u.id);
   REQUIRE(g100 == std::vector<std::uint64_t>{1, 4});

   // group=200 (carol) is gone.
   REQUIRE(users.count<by_group>(std::uint64_t{200}) == 0);

   tx.commit();
}

TEST_CASE("clear: wipes primary + every secondary",
          "[table][secondary][clear]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   for (std::uint64_t k = 1; k <= 4; ++k)
      users.put(PmidxUser{k, "u" + std::to_string(k), k * 100});

   users.clear();

   REQUIRE(users.begin() == users.end());
   for (std::uint64_t k = 1; k <= 4; ++k)
      REQUIRE_FALSE(users.contains<by_id>(k));
   REQUIRE_FALSE(users.contains<by_name>(std::string("u1")));
   REQUIRE(users.begin<by_group>() == users.end<by_group>());

   // Re-populate after clear works as expected.
   users.put(PmidxUser{42, "fresh", 999});
   REQUIRE(users.contains<by_id>(std::uint64_t{42}));
   REQUIRE(users.find<by_name>(std::string("fresh"))->id == 42);

   tx.commit();
}

TEST_CASE("named-tag: subtx abort restores secondary indexes too",
          "[table][secondary][named-tag][subtx]")
{
   fixture     f;
   auto        tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(PmidxUser{1, "alice", 100});
   {
      auto sub = tx.sub_transaction();
      users.put(PmidxUser{1, "alice2", 100});
      sub.abort();
   }

   auto v = users.find<by_id>(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "alice");

   REQUIRE(users.find<by_name>(std::string("alice")).has_value());
   REQUIRE_FALSE(users.find<by_name>(std::string("alice2")).has_value());

   tx.commit();
}
