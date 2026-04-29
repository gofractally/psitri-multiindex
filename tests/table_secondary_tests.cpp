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

namespace pmidx_sec_test
{
   struct user
   {
      std::uint64_t id;          // primary
      std::string   name;        // secondary 1 (unique)
      std::uint64_t group_id;    // secondary 2 (non-unique → composite with id)
   };
   PSIO_REFLECT(user, id, name, group_id)

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

   using users_table = table<user,
                             /*primary  */ &user::id,
                             /*secondary*/ &user::name,
                             /*secondary*/ composite_key<&user::group_id, &user::id>{}>;
}  // namespace pmidx_sec_test

using pmidx_sec_test::fixture;
using pmidx_sec_test::user;
using pmidx_sec_test::users_table;

TEST_CASE("secondary: lookup by unique name returns the row",
          "[table][secondary]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(user{1, "alice", 100});
   users.put(user{2, "bob", 100});
   users.put(user{3, "carol", 200});

   auto v = users.get_by_secondary<1>(std::string("bob"));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 2);
   REQUIRE(v->name == "bob");

   tx.commit();
}

TEST_CASE("secondary: lookup by missing key returns nullopt",
          "[table][secondary]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");
   users.put(user{1, "alice", 100});
   REQUIRE_FALSE(users.get_by_secondary<1>(std::string("nonexistent")).has_value());
   tx.commit();
}

TEST_CASE("secondary: replace updates the secondary index",
          "[table][secondary]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(user{1, "alice", 100});
   users.put(user{1, "alice2", 100});  // rename

   // Old name should no longer find the row.
   REQUIRE_FALSE(users.get_by_secondary<1>(std::string("alice")).has_value());
   // New name should.
   auto v = users.get_by_secondary<1>(std::string("alice2"));
   REQUIRE(v.has_value());
   REQUIRE(v->id == 1);

   tx.commit();
}

TEST_CASE("secondary: erase clears all secondary entries",
          "[table][secondary]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(user{1, "alice", 100});
   REQUIRE(users.get_by_secondary<1>(std::string("alice")).has_value());

   users.erase(std::uint64_t{1});
   REQUIRE_FALSE(users.get_by_secondary<1>(std::string("alice")).has_value());

   tx.commit();
}

TEST_CASE("secondary: composite key supports same value across rows",
          "[table][secondary][composite]")
{
   // group_id alone is not unique (two users in group 100), so the secondary
   // is declared as composite_key<group_id, id> — the trailing id makes
   // every secondary key unique by construction.
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(user{1, "alice", 100});
   users.put(user{2, "bob",   100});
   users.put(user{3, "carol", 200});

   // get_by_secondary<2> with full composite (group_id, id) finds the exact row.
   auto v = users.get_by_secondary<2>(std::make_tuple(std::uint64_t{100},
                                                      std::uint64_t{2}));
   REQUIRE(v.has_value());
   REQUIRE(v->name == "bob");

   // A different (group_id, id) tuple finds the other row in the group.
   auto w = users.get_by_secondary<2>(std::make_tuple(std::uint64_t{100},
                                                      std::uint64_t{1}));
   REQUIRE(w.has_value());
   REQUIRE(w->name == "alice");

   tx.commit();
}

TEST_CASE("secondary: subtx abort restores secondary indexes too",
          "[table][secondary][subtx]")
{
   fixture f;
   auto    tx = f.ws->start_transaction(0);
   users_table users(tx, "U/");

   users.put(user{1, "alice", 100});
   {
      auto sub = tx.sub_transaction();
      users.put(user{1, "alice2", 100});
      sub.abort();
   }

   // Both primary AND the secondary entry must be back at the pre-sub state.
   auto v = users.get(std::uint64_t{1});
   REQUIRE(v.has_value());
   REQUIRE(v->name == "alice");

   REQUIRE(users.get_by_secondary<1>(std::string("alice")).has_value());
   REQUIRE_FALSE(users.get_by_secondary<1>(std::string("alice2")).has_value());

   tx.commit();
}
