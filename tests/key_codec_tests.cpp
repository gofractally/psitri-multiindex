#include <catch2/catch_all.hpp>

#include <psitri_multiindex/key_codec.hpp>

#include <cstring>
#include <string_view>
#include <vector>

using namespace psitri_multiindex;

namespace
{
   int memcmp_view(const std::vector<char>& a, const std::vector<char>& b)
   {
      const auto n = std::min(a.size(), b.size());
      if (n > 0)
         if (int c = std::memcmp(a.data(), b.data(), n); c != 0)
            return c < 0 ? -1 : 1;
      if (a.size() < b.size())
         return -1;
      if (a.size() > b.size())
         return 1;
      return 0;
   }
}  // namespace

TEST_CASE("prefix_byte_for_index maps 0→primary, 1+→secondary",
          "[key_codec][prefix]")
{
   REQUIRE(prefix_byte_for_index(0) == PRIMARY_PREFIX_BYTE);
   REQUIRE(prefix_byte_for_index(1) == FIRST_SECONDARY_BYTE);
   REQUIRE(prefix_byte_for_index(2) == FIRST_SECONDARY_BYTE + 1);
   REQUIRE(prefix_byte_for_index(MAX_SECONDARIES) == LAST_SECONDARY_BYTE);
}

TEST_CASE("encoded primary key has correct framing", "[key_codec][primary]")
{
   const std::string_view prefix = "tbl/";
   const auto             k      = encode_primary_key(prefix, std::uint64_t{0x42});
   REQUIRE(k.size() == prefix.size() + 1 + sizeof(std::uint64_t));
   REQUIRE(std::string_view(k.data(), prefix.size()) == prefix);
   REQUIRE(static_cast<std::uint8_t>(k[prefix.size()]) == PRIMARY_PREFIX_BYTE);
   // psio::to_key for unsigned int is big-endian.
   REQUIRE(static_cast<std::uint8_t>(k[k.size() - 1]) == 0x42);
   REQUIRE(static_cast<std::uint8_t>(k[k.size() - 2]) == 0x00);
}

TEST_CASE("encoded secondary key uses correct sub-prefix byte",
          "[key_codec][secondary]")
{
   const std::string_view prefix = "tbl/";
   const auto k1 = encode_secondary_key(prefix, /*one_based=*/1, std::uint64_t{0x42});
   const auto k2 = encode_secondary_key(prefix, /*one_based=*/2, std::uint64_t{0x42});
   REQUIRE(static_cast<std::uint8_t>(k1[prefix.size()]) == FIRST_SECONDARY_BYTE);
   REQUIRE(static_cast<std::uint8_t>(k2[prefix.size()]) == FIRST_SECONDARY_BYTE + 1);
}

TEST_CASE("primary keys are memcmp-sortable for unsigned ints",
          "[key_codec][sort]")
{
   const std::string_view prefix = "T/";
   auto a = encode_primary_key(prefix, std::uint64_t{1});
   auto b = encode_primary_key(prefix, std::uint64_t{2});
   auto c = encode_primary_key(prefix, std::uint64_t{0xffff'ffff});
   auto d = encode_primary_key(prefix, std::uint64_t{0xffff'ffff'ffff'ffff});
   REQUIRE(memcmp_view(a, b) < 0);
   REQUIRE(memcmp_view(b, c) < 0);
   REQUIRE(memcmp_view(c, d) < 0);
}

TEST_CASE("primary keys are memcmp-sortable for strings",
          "[key_codec][sort]")
{
   const std::string_view prefix = "T/";
   auto a = encode_primary_key(prefix, std::string("apple"));
   auto b = encode_primary_key(prefix, std::string("banana"));
   auto c = encode_primary_key(prefix, std::string("cherry"));
   REQUIRE(memcmp_view(a, b) < 0);
   REQUIRE(memcmp_view(b, c) < 0);
}

TEST_CASE("composite_key extracts a tuple in declared order",
          "[key_codec][composite]")
{
   struct row
   {
      std::uint64_t a;
      std::uint32_t b;
      std::string   c;
   };
   constexpr composite_key<&row::a, &row::b> k{};
   row                                       r{42, 7, "hello"};
   auto                                      t = k(r);
   STATIC_REQUIRE(std::tuple_size_v<decltype(t)> == 2);
   REQUIRE(std::get<0>(t) == 42);
   REQUIRE(std::get<1>(t) == 7);
}

TEST_CASE("primary keys with composite components remain sortable",
          "[key_codec][composite][sort]")
{
   // Two scalars concatenated produce a memcmp-sortable composite.
   const std::string_view prefix = "T/";
   auto a = encode_primary_key(prefix, std::make_tuple(std::uint64_t{1},
                                                       std::uint32_t{0xffff'ffff}));
   auto b = encode_primary_key(prefix, std::make_tuple(std::uint64_t{2},
                                                       std::uint32_t{0}));
   REQUIRE(memcmp_view(a, b) < 0);
}

TEST_CASE("encode_index_prefix returns just prefix ‖ index_byte",
          "[key_codec][prefix-iter]")
{
   const std::string_view prefix = "abc";
   auto                   p      = encode_index_prefix(prefix, PRIMARY_PREFIX_BYTE);
   REQUIRE(p.size() == prefix.size() + 1);
   REQUIRE(std::string_view(p.data(), prefix.size()) == prefix);
   REQUIRE(static_cast<std::uint8_t>(p.back()) == PRIMARY_PREFIX_BYTE);
}

TEST_CASE("lex_increment advances within prefix bound",
          "[key_codec][prefix-iter]")
{
   {
      std::vector<char> buf{'a', 'b', 'c'};
      REQUIRE(lex_increment(buf));
      REQUIRE(buf == std::vector<char>{'a', 'b', 'd'});
   }
   {
      std::vector<char> buf{'a', char(0xff), char(0xff)};
      REQUIRE(lex_increment(buf));
      REQUIRE(buf == std::vector<char>{'b'});
   }
   {
      std::vector<char> buf{char(0xff), char(0xff)};
      REQUIRE_FALSE(lex_increment(buf));
   }
}
