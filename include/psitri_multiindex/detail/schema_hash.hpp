#pragma once
//
// Compile-time-derived schema fingerprint for a reflected row type T.
//
// Walks `psio::reflect<T>` and builds a stable fingerprint string:
//
//   <type_name> | <member_name>:<member_fingerprint> | …
//
// For each member's type:
//   - Reflected nested type (PSIO_REFLECT'd): recurse, emitting the
//     same `{name|m:f|…}` shape — catches rename / reorder / type
//     changes inside the nested struct.
//   - Has a `psio::get_type_name` overload (primitives, std::string,
//     std::vector, std::array, std::optional, std::tuple, …, plus any
//     `PSIO_REFLECT_TYPENAME` registrations): use the spelled name.
//   - Otherwise: fall back to `anon{<sizeof>:<alignof>}`. Coarse but
//     stable; the user can promote to PSIO_REFLECT_TYPENAME for
//     better discrimination.
//
// xxh64'd to a 64-bit value at the end.

#include <psitri_multiindex/detail/xxhash.h>

#include <psio/get_type_name.hpp>
#include <psio/reflect.hpp>

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

namespace psitri_multiindex::detail
{
   namespace schema_fp
   {
      template <typename U>
      concept HasGetTypeName =
          requires { ::psio::get_type_name(static_cast<const U*>(nullptr)); };

      inline void append_size_t(std::string& fp, std::size_t v)
      {
         char buf[32];
         auto r = std::to_chars(buf, buf + sizeof(buf), v);
         fp.append(buf, r.ptr);
      }

      template <typename U>
      void append_fingerprint(std::string& fp);

      template <typename U>
      void append_member_typename(std::string& fp)
      {
         using R = ::psio::reflect<U>;
         if constexpr (::psio::is_reflected_v<U>)
         {
            fp.push_back('{');
            fp.append(R::name.data(), R::name.size());
            [&]<std::size_t... Js>(std::index_sequence<Js...>)
            {
               (
                   [&]
                   {
                      fp.push_back('|');
                      const auto mn = R::template member_name<Js>;
                      fp.append(mn.data(), mn.size());
                      fp.push_back(':');
                      append_fingerprint<
                          typename R::template member_type<Js>>(fp);
                   }(),
                   ...);
            }(std::make_index_sequence<R::member_count>{});
            fp.push_back('}');
         }
         else if constexpr (HasGetTypeName<U>)
         {
            const char* tn =
                ::psio::get_type_name(static_cast<const U*>(nullptr));
            fp.append(tn);
         }
         else
         {
            // Stable fallback for anonymous types (no PSIO_REFLECT, no
            // PSIO_REFLECT_TYPENAME). Catches size / alignment changes
            // but not renames or layout shuffles within the same size.
            fp.append("anon{");
            append_size_t(fp, sizeof(U));
            fp.push_back(':');
            append_size_t(fp, alignof(U));
            fp.push_back('}');
         }
      }

      template <typename U>
      void append_fingerprint(std::string& fp)
      {
         append_member_typename<U>(fp);
      }
   }  // namespace schema_fp

   template <typename T>
   std::uint64_t compute_schema_hash() noexcept
   {
      std::string fp;
      schema_fp::append_member_typename<T>(fp);
      return ::XXH64(fp.data(), fp.size(), /*seed=*/0);
   }
}  // namespace psitri_multiindex::detail
