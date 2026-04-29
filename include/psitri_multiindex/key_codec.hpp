#pragma once

#include <psio/cpo.hpp>
#include <psio/key.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psitri_multiindex
{
   // Sub-prefix bytes inside a table's prefix space.
   //
   //   prefix ‖ 0x00 ‖ 0x00          → hot header        (the future home of metadata)
   //   prefix ‖ 0x00 ‖ 0x01          → canonical schema  (cold)
   //   prefix ‖ 0x01 ‖ pk_bytes      → primary tree
   //   prefix ‖ 0x02..0xFF ‖ sk_bytes → secondary trees (1..254)
   //
   // 0x00 is the metadata namespace, 0x01 is the primary, secondaries start at 0x02.
   static constexpr std::uint8_t META_PREFIX_BYTE     = 0x00;
   static constexpr std::uint8_t META_HEADER_SUBBYTE  = 0x00;
   static constexpr std::uint8_t META_SCHEMA_SUBBYTE  = 0x01;
   static constexpr std::uint8_t PRIMARY_PREFIX_BYTE  = 0x01;
   static constexpr std::uint8_t FIRST_SECONDARY_BYTE = 0x02;
   static constexpr std::uint8_t LAST_SECONDARY_BYTE  = 0xFF;
   static constexpr std::size_t  MAX_SECONDARIES = LAST_SECONDARY_BYTE - FIRST_SECONDARY_BYTE + 1;

   // Index 0 is primary, indexes 1..MAX_SECONDARIES are secondaries.
   constexpr std::uint8_t prefix_byte_for_index(std::size_t idx) noexcept
   {
      // idx == 0 → primary (0x01)
      // idx >= 1 → secondary at FIRST_SECONDARY_BYTE + (idx-1)
      return idx == 0 ? PRIMARY_PREFIX_BYTE
                      : static_cast<std::uint8_t>(FIRST_SECONDARY_BYTE + (idx - 1));
   }

   namespace detail
   {
      template <typename T>
      struct is_tuple : std::false_type
      {
      };
      template <typename... Ts>
      struct is_tuple<std::tuple<Ts...>> : std::true_type
      {
      };

      // Append memcmp-sortable bytes for `value` to `sink`. Tuples are
      // encoded by concatenating each element's encoding (matches v1's
      // to_key tuple convention; v3's key format itself does not yet
      // handle std::tuple natively).
      template <typename Key>
      void append_key_bytes(const Key& value, std::vector<char>& sink)
      {
         if constexpr (is_tuple<Key>::value)
         {
            std::apply(
                [&](const auto&... elems)
                { (append_key_bytes(elems, sink), ...); },
                value);
         }
         else
         {
            psio::encode(psio::key{}, value, sink);
         }
      }
   }  // namespace detail

   // Encode prefix ‖ index_byte ‖ key_bytes into a single byte vector.
   // Key bytes are produced by psio::encode(psio::key{}, ...) — memcmp-sortable.
   // Tuple keys are encoded by concatenating their components (matches v1).
   //
   // The base_prefix is the per-table opaque byte string (chosen at table
   // construction time). The result is a complete trie key.
   template <typename Key>
   std::vector<char> encode_index_key(std::string_view      base_prefix,
                                      std::uint8_t          index_byte,
                                      const Key&            key)
   {
      std::vector<char> out;
      out.reserve(base_prefix.size() + 1);
      out.insert(out.end(), base_prefix.begin(), base_prefix.end());
      out.push_back(static_cast<char>(index_byte));
      detail::append_key_bytes(key, out);
      return out;
   }

   // Encode just prefix ‖ index_byte (no key body). Useful for "iterate this whole index".
   inline std::vector<char> encode_index_prefix(std::string_view base_prefix,
                                                std::uint8_t     index_byte)
   {
      std::vector<char> out;
      out.reserve(base_prefix.size() + 1);
      out.insert(out.end(), base_prefix.begin(), base_prefix.end());
      out.push_back(static_cast<char>(index_byte));
      return out;
   }

   // Increment a byte buffer to the lexicographic successor (for upper_bound on prefix).
   // Returns true on success, false if the buffer is all 0xff (no successor exists).
   inline bool lex_increment(std::vector<char>& buf) noexcept
   {
      while (!buf.empty() && static_cast<std::uint8_t>(buf.back()) == 0xff)
         buf.pop_back();
      if (buf.empty())
         return false;
      ++reinterpret_cast<std::uint8_t&>(buf.back());
      return true;
   }

   // Wrapper for "the primary tree's key for a given primary value".
   template <typename PK>
   std::vector<char> encode_primary_key(std::string_view base_prefix, const PK& pk)
   {
      return encode_index_key(base_prefix, PRIMARY_PREFIX_BYTE, pk);
   }

   // Wrapper for "the secondary tree N's key for a given secondary value".
   // N is 1-based — N==1 means the first secondary at byte 0x02.
   template <typename SK>
   std::vector<char> encode_secondary_key(std::string_view base_prefix,
                                          std::size_t      one_based_secondary,
                                          const SK&        sk)
   {
      return encode_index_key(base_prefix, prefix_byte_for_index(one_based_secondary), sk);
   }

   // composite_key<KExtractors...>: extract a tuple of keys, encoded by concatenation
   // (psio::to_key on a std::tuple already emits concatenated components).
   template <auto... Extractors>
   struct composite_key
   {
      template <typename T>
      auto operator()(const T& v) const
      {
         return std::make_tuple(invoke_extractor(Extractors, v)...);
      }

     private:
      template <typename Ex, typename T>
      static decltype(auto) invoke_extractor(Ex extractor, const T& v)
      {
         if constexpr (std::is_member_pointer_v<Ex>)
            return v.*extractor;
         else
            return extractor(v);
      }
   };

   // Apply an extractor to a value, returning the key.
   template <auto Extractor, typename T>
   decltype(auto) apply_extractor(const T& v)
   {
      if constexpr (std::is_member_pointer_v<decltype(Extractor)>)
         return v.*Extractor;
      else
         return Extractor(v);
   }

   // composite_key<...> is itself a callable that returns a tuple.
   template <auto... Es, typename T>
   auto apply_extractor_helper(composite_key<Es...> ck, const T& v)
   {
      return ck(v);
   }
}  // namespace psitri_multiindex
