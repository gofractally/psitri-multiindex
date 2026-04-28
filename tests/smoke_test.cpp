// Smoke test for psitri-multiindex.
//
// Confirms that:
//   1. psio headers include and compile when used in the same TU as psitri.
//   2. psitri's basic_database opens and round-trips a key.
//   3. The two libraries link together cleanly.
//
// This is intentionally minimal — its purpose is to validate the build
// plumbing, not the eventual multi-index API.

#include <psio/fracpack.hpp>

#include <psitri/database.hpp>
#include <psitri/database_impl.hpp>
#include <psitri/transaction.hpp>
#include <psitri/value_type.hpp>
#include <psitri/write_session.hpp>
#include <psitri/write_session_impl.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace
{
   std::filesystem::path unique_dbdir(const char* tag)
   {
      const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
      auto       p  = std::filesystem::temp_directory_path() /
               ("pmidx_" + std::string(tag) + "_" + std::to_string(getpid()) + "_" +
                std::to_string(ts));
      std::filesystem::remove_all(p);
      std::filesystem::create_directories(p);
      return p;
   }
}  // namespace

int main()
{
   const auto dir = unique_dbdir("smoke");

   // psitri round-trip: open db, write a key, read it back.
   {
      auto db  = psitri::database::open(dir);
      auto ses = db->start_write_session();
      auto tx  = ses->start_transaction(0);
      tx.upsert(psitri::to_key("hello"), psitri::to_value("world"));
      tx.commit();

      // Re-read on a fresh transaction.
      auto tx2 = ses->start_transaction(0);
      auto v   = tx2.get<std::string>(psitri::to_key("hello"));
      tx2.abort();

      if (!v.has_value())
      {
         std::fprintf(stderr, "psitri get returned no value\n");
         std::filesystem::remove_all(dir);
         return 1;
      }
      if (*v != "world")
      {
         std::fprintf(stderr, "psitri value mismatch: got '%s'\n", v->c_str());
         std::filesystem::remove_all(dir);
         return 1;
      }
   }

   std::filesystem::remove_all(dir);

   // psio reachability: instantiate something that pulls in fracpack
   // declarations to confirm the include path resolves and the headers
   // compile under our cxx_std_23 + boost setup.
   (void)sizeof(psio::input_stream);

   std::printf("psitri-multiindex smoke test passed\n");
   return 0;
}
