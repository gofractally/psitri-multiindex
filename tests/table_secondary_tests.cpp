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
