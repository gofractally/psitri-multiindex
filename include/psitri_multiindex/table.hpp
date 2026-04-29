#pragma once

#include <psitri_multiindex/key_codec.hpp>

#include <psio/cpo.hpp>
#include <psio/pssz.hpp>
#include <psitri/cursor.hpp>
#include <psitri/transaction.hpp>
#include <psitri/value_type.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psitri_multiindex
{
   namespace detail
   {
      template <auto Extractor, typename T>
      decltype(auto) invoke_extractor(const T& v)
      {
         using ExT = decltype(Extractor);
         if constexpr (std::is_member_pointer_v<ExT>)
         {
            if constexpr (std::is_member_function_pointer_v<ExT>)
               return (v.*Extractor)();
            else
               return v.*Extractor;
         }
         else
         {
            // composite_key<...> or free-standing functor
            return Extractor(v);
         }
      }

      // psitri::key_view from a std::vector<char> bytes range.
      inline psitri::key_view key_view_of(const std::vector<char>& v) noexcept
      {
         return psitri::key_view(v.data(), v.size());
      }
      // psitri::value_view from a std::vector<char>.
      inline psitri::value_view value_view_of(const std::vector<char>& v) noexcept
      {
         return psitri::value_view(v.data(), v.size());
      }
   }  // namespace detail

   // Multi-index table over a psitri transaction.
   //
   // T is the row type (must be PSIO_REFLECT'd for fracpack).
   // Primary is an extractor for the primary key (member pointer or
   // composite_key<...>). Each Secondary is the same.
   //
   // Storage layout (see design.md):
   //   prefix ‖ 0x01 ‖ to_key(pk)         → fracpack(row)
   //   prefix ‖ 0x02 ‖ to_key(sk_1)       → prefix ‖ 0x01 ‖ to_key(pk)
   //   prefix ‖ 0x03 ‖ to_key(sk_2)       → ...
   //
   // Non-unique secondaries should use composite_key<sk_extractor, primary_extractor>
   // so the encoded tree key is unique by construction.
   template <typename T, auto Primary, auto... Secondaries>
   class table
   {
     public:
      using value_type      = T;
      using primary_key_type =
          std::remove_cvref_t<decltype(detail::invoke_extractor<Primary>(std::declval<T>()))>;

      static constexpr std::size_t num_secondaries = sizeof...(Secondaries);

      table(psitri::transaction& tx, std::string_view base_prefix)
          : _tx(&tx), _prefix(base_prefix.begin(), base_prefix.end())
      {
      }

      table(const table&)            = delete;
      table& operator=(const table&) = delete;
      table(table&&)                 = default;
      table& operator=(table&&)      = default;

      // ── Mutations ─────────────────────────────────────────────────────

      // Insert or replace a row. If a row with the same primary key already
      // exists, secondary entries that point to differing secondary keys are
      // removed and replaced; the primary value is overwritten.
      void put(const T& row)
      {
         auto pk_bytes = encode_primary_key(prefix_view(), detail::invoke_extractor<Primary>(row));

         if constexpr (num_secondaries > 0)
         {
            // If a row already exists, fetch it to compute key diffs.
            if (auto existing = _tx->get<std::string>(detail::key_view_of(pk_bytes)))
            {
               T old_row = psio::decode<T>(psio::pssz{}, std::span<const char>(existing->data(),
                                                                              existing->size()));
               replace_secondaries_diff(old_row, row, pk_bytes);
            }
            else
            {
               write_all_secondaries(row, pk_bytes);
            }
         }

         auto packed = psio::encode(psio::pssz{}, row);
         _tx->upsert(detail::key_view_of(pk_bytes), detail::value_view_of(packed));
      }

      // Erase by primary key value. Returns true if a row was removed.
      template <typename K>
      bool erase(const K& pk_value)
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);

         if constexpr (num_secondaries > 0)
         {
            // Need to read the existing row to find its secondary key values.
            auto existing = _tx->get<std::string>(detail::key_view_of(pk_bytes));
            if (!existing)
               return false;
            T old_row = psio::decode<T>(psio::pssz{}, std::span<const char>(existing->data(),
                                                                 existing->size()));
            erase_all_secondaries(old_row);
         }

         return _tx->remove(detail::key_view_of(pk_bytes)) >= 0;
      }

      // Erase by full row reference (used after a get).
      bool remove(const T& row)
      {
         return erase(detail::invoke_extractor<Primary>(row));
      }

      // ── Reads ─────────────────────────────────────────────────────────

      // Look up a row by its primary key. Returns nullopt if not found.
      template <typename K>
      std::optional<T> get(const K& pk_value) const
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);
         auto raw      = _tx->get<std::string>(detail::key_view_of(pk_bytes));
         if (!raw)
            return std::nullopt;
         return psio::decode<T>(psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
      }

      // Check existence without decoding.
      template <typename K>
      bool contains(const K& pk_value) const
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);
         return _tx->get<std::string>(detail::key_view_of(pk_bytes)).has_value();
      }

      // Look up a primary key via a secondary index. Secondary indexes are 1-based:
      // index 1 = first declared Secondary, index 2 = second, ...
      //
      // Returns the row whose secondary value matches, or nullopt.
      template <std::size_t SecondaryIdx, typename SK>
      std::optional<T> get_by_secondary(const SK& sk_value) const
      {
         static_assert(SecondaryIdx >= 1 && SecondaryIdx <= num_secondaries,
                       "Secondary index out of range");
         auto sk_bytes = encode_secondary_key(prefix_view(), SecondaryIdx, sk_value);
         auto pk_ref   = _tx->get<std::string>(detail::key_view_of(sk_bytes));
         if (!pk_ref)
            return std::nullopt;
         auto raw = _tx->get<std::string>(
             psitri::key_view(pk_ref->data(), pk_ref->size()));
         if (!raw)
            return std::nullopt;
         return psio::decode<T>(psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
      }

      // ── Iteration ─────────────────────────────────────────────────────

      // Forward iterator over the primary tree, yielding decoded rows.
      // Reads only — modifications during iteration are observable through
      // psitri's snapshot semantics (the iterator's cursor pins a view).
      class primary_iterator
      {
        public:
         primary_iterator(const table* tbl, psitri::cursor c)
             : _table(tbl), _cur(std::move(c))
         {
            check_in_range();
         }

         // Past-the-end constructor.
         struct end_tag {};
         primary_iterator(const table* tbl, end_tag)
             : _table(tbl),
               _cur(tbl->_tx->read_cursor()),
               _at_end(true)
         {
         }

         T operator*() const
         {
            auto v = _cur.template value<std::string>();
            return psio::decode<T>(psio::pssz{}, std::span<const char>(v->data(), v->size()));
         }

         primary_iterator& operator++()
         {
            _cur.next();
            check_in_range();
            return *this;
         }

         // Equality is "both at end" or "both pointing at the same key".
         friend bool operator==(const primary_iterator& a,
                                const primary_iterator& b) noexcept
         {
            if (a._at_end || b._at_end)
               return a._at_end && b._at_end;
            const auto ak = a._cur.key();
            const auto bk = b._cur.key();
            return ak.size() == bk.size() &&
                   std::memcmp(ak.data(), bk.data(), ak.size()) == 0;
         }
         friend bool operator!=(const primary_iterator& a,
                                const primary_iterator& b) noexcept
         {
            return !(a == b);
         }

         // Returns the encoded key bytes of the current row (including prefix).
         std::string_view raw_key() const noexcept
         {
            const auto k = _cur.key();
            return std::string_view(k.data(), k.size());
         }

        private:
         void check_in_range()
         {
            if (_cur.is_end())
            {
               _at_end = true;
               return;
            }
            const auto kv = _cur.key();
            const auto pfx_size = _table->_prefix.size() + 1;
            // Must start with prefix ‖ PRIMARY_PREFIX_BYTE.
            if (kv.size() < pfx_size ||
                std::memcmp(kv.data(), _table->_prefix.data(),
                            _table->_prefix.size()) != 0 ||
                static_cast<std::uint8_t>(kv.data()[_table->_prefix.size()]) !=
                    PRIMARY_PREFIX_BYTE)
            {
               _at_end = true;
            }
         }

         const table*    _table;
         psitri::cursor  _cur;
         bool            _at_end = false;
      };

      // Iterator at the first primary key.
      primary_iterator begin() const
      {
         auto pfx = encode_index_prefix(prefix_view(), PRIMARY_PREFIX_BYTE);
         auto c   = _tx->lower_bound(psitri::key_view(pfx.data(), pfx.size()));
         return primary_iterator(this, std::move(c));
      }

      // Past-the-end iterator.
      primary_iterator end() const
      {
         return primary_iterator(this, typename primary_iterator::end_tag{});
      }

      // First primary row whose key >= pk_value.
      template <typename K>
      primary_iterator lower_bound(const K& pk_value) const
      {
         auto k = encode_primary_key(prefix_view(), pk_value);
         auto c = _tx->lower_bound(psitri::key_view(k.data(), k.size()));
         return primary_iterator(this, std::move(c));
      }

      // First primary row whose key > pk_value.
      template <typename K>
      primary_iterator upper_bound(const K& pk_value) const
      {
         auto k = encode_primary_key(prefix_view(), pk_value);
         auto c = _tx->upper_bound(psitri::key_view(k.data(), k.size()));
         return primary_iterator(this, std::move(c));
      }

      // ── Diagnostics ───────────────────────────────────────────────────

      std::string_view prefix() const noexcept { return prefix_view(); }
      psitri::transaction& transaction() noexcept { return *_tx; }

     private:
      std::string_view prefix_view() const noexcept
      {
         return std::string_view(_prefix.data(), _prefix.size());
      }

      // Walk the parameter pack and materialize each secondary's encoded
      // (key, value) pair, calling `op(idx_byte, sk_bytes, pk_bytes)`.
      template <typename Row, typename Op, std::size_t... Is>
      void for_each_secondary(const Row& row, const std::vector<char>& pk_bytes,
                              Op&& op, std::index_sequence<Is...>) const
      {
         (
             [&] {
                constexpr std::size_t one_based = Is + 1;
                auto sk_bytes = encode_secondary_key(
                    prefix_view(), one_based,
                    detail::invoke_extractor<Secondary_at_v<Is>>(row));
                op(one_based, sk_bytes, pk_bytes);
             }(),
             ...);
      }

      // Helper template: index into the Secondaries... pack at compile time.
      template <std::size_t I>
      static constexpr auto Secondary_at_v_impl()
      {
         constexpr auto pack = std::tuple{Secondaries...};
         return std::get<I>(pack);
      }
      template <std::size_t I>
      static constexpr auto Secondary_at_v = Secondary_at_v_impl<I>();

      // Write all secondary entries pointing at this primary key.
      void write_all_secondaries(const T& row, const std::vector<char>& pk_bytes)
      {
         for_each_secondary(row, pk_bytes,
            [this](std::size_t, const std::vector<char>& sk, const std::vector<char>& pk) {
               _tx->upsert(detail::key_view_of(sk), detail::value_view_of(pk));
            },
            std::make_index_sequence<num_secondaries>{});
      }

      // Erase all secondary entries for this row.
      void erase_all_secondaries(const T& row)
      {
         std::vector<char> dummy;  // pk_bytes unused in erase
         for_each_secondary(row, dummy,
            [this](std::size_t, const std::vector<char>& sk, const std::vector<char>&) {
               _tx->remove(detail::key_view_of(sk));
            },
            std::make_index_sequence<num_secondaries>{});
      }

      // Compare old vs new secondary keys; remove any that changed and
      // re-insert the new value.
      void replace_secondaries_diff(const T& old_row, const T& new_row,
                                    const std::vector<char>& pk_bytes)
      {
         diff_secondaries(old_row, new_row, pk_bytes,
                          std::make_index_sequence<num_secondaries>{});
      }

      template <std::size_t... Is>
      void diff_secondaries(const T& old_row, const T& new_row,
                            const std::vector<char>& pk_bytes,
                            std::index_sequence<Is...>)
      {
         (
             [&] {
                constexpr std::size_t one_based = Is + 1;
                constexpr auto extractor       = Secondary_at_v<Is>;
                auto old_sk = detail::invoke_extractor<extractor>(old_row);
                auto new_sk = detail::invoke_extractor<extractor>(new_row);
                if (!(old_sk == new_sk))
                {
                   auto old_bytes =
                       encode_secondary_key(prefix_view(), one_based, old_sk);
                   _tx->remove(detail::key_view_of(old_bytes));
                   auto new_bytes =
                       encode_secondary_key(prefix_view(), one_based, new_sk);
                   _tx->upsert(detail::key_view_of(new_bytes),
                               detail::value_view_of(pk_bytes));
                }
             }(),
             ...);
      }

      psitri::transaction* _tx;
      std::vector<char>    _prefix;
   };
}  // namespace psitri_multiindex
