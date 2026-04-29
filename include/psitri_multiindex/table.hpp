#pragma once

// psitri-multiindex: a chainbase-style multi-index container backed by
// psitri trees with psio pSSZ-encoded records and view_of-based extractors.
//
// Public API shape (matches chainbase's conceptual model):
//
//   struct by_id;        // user-declared empty tag types
//   struct by_name;
//
//   using my_table = psitri_multiindex::table<row_type,
//      psitri_multiindex::ordered_unique    <by_id,    &row_type::id>,
//      psitri_multiindex::ordered_unique    <by_name,  &row_type::name>,
//      psitri_multiindex::ordered_non_unique<by_group, &row_type::group_id>>;
//
//   tbl.put(row);                                  // upsert (collision-checked)
//   tbl.insert(row);                               // alias for put
//   tbl.erase(pk_value);
//   tbl.remove(row);
//   auto row     = tbl.find<by_name>("alice");     // optional<T>
//   auto by_grp  = tbl.equal_range<by_group>(100); // iteration range
//   for (auto it = tbl.begin(); it != tbl.end(); ++it) ...   // primary order

#include <psitri_multiindex/key_codec.hpp>

#include <psio/cpo.hpp>
#include <psio/native_view.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/view.hpp>
#include <psitri/cursor.hpp>
#include <psitri/transaction.hpp>
#include <psitri/value_type.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psitri_multiindex
{
   // ── Index declaration tags ───────────────────────────────────────────────
   //
   // ordered_unique<Tag, Ex>     — Tag is the named identity, Ex is the key
   //                                extractor (member ptr or composite_key<...>).
   //                                Collisions on put are rejected with an
   //                                exception.
   // ordered_non_unique<Tag, Ex> — Same, but the encoded tree key is
   //                                automatically composed as (Ex(row), pk(row))
   //                                so the tree stays unique-by-construction.
   //                                Iteration on this tag returns rows in
   //                                ((Ex, pk)) order; queries by bare Ex value
   //                                use equal_range to span the matching block.

   template <typename Tag, auto Extractor>
   struct ordered_unique
   {
      using tag                          = Tag;
      static constexpr auto extractor    = Extractor;
      static constexpr bool is_unique    = true;
   };

   template <typename Tag, auto Extractor>
   struct ordered_non_unique
   {
      using tag                          = Tag;
      static constexpr auto extractor    = Extractor;
      static constexpr bool is_unique    = false;
   };

   // ── Exceptions ───────────────────────────────────────────────────────────

   class secondary_collision : public std::runtime_error
   {
     public:
      using std::runtime_error::runtime_error;
   };

   // ── Table header ─────────────────────────────────────────────────────────
   //
   // Stored at `prefix ‖ 0x00 ‖ 0x00`, encoded with pSSZ. Trailing optional
   // fields can be appended in future versions without breaking older readers
   // (pSSZ append-optional rule).

   struct table_header
   {
      std::uint64_t                next_id{0};       // auto-increment cursor
      std::uint64_t                schema_hash{0};   // xxh64 of canonical schema (placeholder)
      std::uint16_t                schema_version{0};
      std::uint16_t                flags{0};         // bit 0: row_count_enabled
      std::optional<std::uint64_t> row_count{};      // present iff (flags & 1)
   };

   // Flag bits.
   inline constexpr std::uint16_t TABLE_FLAG_ROW_COUNT_ENABLED = 0x0001;

   PSIO_REFLECT(table_header,
                next_id, schema_hash, schema_version, flags, row_count)

   // Construction options.
   struct table_options
   {
      bool track_row_count = false;
   };

   namespace detail
   {
      // Invoke an extractor (member pointer, member fn pointer, composite_key,
      // or any other callable) on a value of type T.
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
            return Extractor(v);
         }
      }

      // Compile-time tag → index resolver over a parameter pack of indexes.
      // Indexes is the pack ordered_unique<...>/ordered_non_unique<...>.
      template <typename Tag, typename... Indexes>
      struct index_of_tag;

      template <typename Tag, typename First, typename... Rest>
      struct index_of_tag<Tag, First, Rest...>
      {
        private:
         static constexpr bool match = std::is_same_v<typename First::tag, Tag>;

        public:
         static constexpr std::size_t value =
             match ? 0 : 1 + index_of_tag<Tag, Rest...>::value;
      };

      template <typename Tag>
      struct index_of_tag<Tag>
      {
         // Sentinel for "not found" — caught by static_assert.
         static constexpr std::size_t value = static_cast<std::size_t>(-1);
      };

      // Pack member access: nth_index_t<I, Idxs...> = I-th index type in the pack.
      template <std::size_t I, typename... Idxs>
      using nth_index_t =
          std::tuple_element_t<I, std::tuple<Idxs...>>;

      // psitri view helpers.
      inline psitri::key_view key_view_of(const std::vector<char>& v) noexcept
      {
         return psitri::key_view(v.data(), v.size());
      }
      inline psitri::value_view value_view_of(const std::vector<char>& v) noexcept
      {
         return psitri::value_view(v.data(), v.size());
      }
   }  // namespace detail

   // ── table ────────────────────────────────────────────────────────────────
   //
   // Primary index is the first template-after-T argument. All Indexes are
   // tag-wrapped. Index 0 is the primary; secondaries are 1.. in declaration
   // order, mapped to encoded prefix bytes 0x02..0xFF (see key_codec.hpp).

   template <typename T, typename Primary, typename... Secondaries>
   class table
   {
      static_assert(Primary::is_unique,
                    "primary index must be ordered_unique");

     public:
      using value_type       = T;
      using primary_tag      = typename Primary::tag;
      using primary_key_type = std::remove_cvref_t<
          decltype(detail::invoke_extractor<Primary::extractor>(std::declval<T>()))>;

      static constexpr std::size_t num_secondaries = sizeof...(Secondaries);

      // Compile-time index-by-tag lookup over Primary + Secondaries pack.
      // position 0 = primary, 1..N = secondaries. Returns the index struct
      // type via ::type and its 0-based position via ::position.
      // (Public so nested types like secondary_range_iterator can name it.)
      template <typename Tag>
      struct find_index_by_tag
      {
        private:
         static constexpr bool primary_match =
             std::is_same_v<typename Primary::tag, Tag>;

         using all_indexes = std::tuple<Primary, Secondaries...>;

         static constexpr std::size_t pos =
             primary_match ? 0
                           : 1 + detail::index_of_tag<Tag, Secondaries...>::value;

        public:
         static_assert(pos != static_cast<std::size_t>(-1),
                       "no index registered for this tag");
         using type = std::tuple_element_t<pos, all_indexes>;
         static constexpr std::size_t position = pos;
      };

      table(psitri::transaction& tx, std::string_view base_prefix,
            table_options opts = {})
          : _tx(&tx),
            _prefix(base_prefix.begin(), base_prefix.end()),
            _track_row_count(opts.track_row_count)
      {
         if (_track_row_count)
         {
            // Toggle the flag on; if the header doesn't exist yet, write
            // it. If a row_count was never populated, leave it at 0 — the
            // caller is responsible for either tracking from empty or
            // doing a one-time scan to populate it.
            auto h = read_header();
            if (!(h.flags & TABLE_FLAG_ROW_COUNT_ENABLED))
            {
               h.flags |= TABLE_FLAG_ROW_COUNT_ENABLED;
               if (!h.row_count.has_value())
                  h.row_count = 0;
               write_header(h);
            }
         }
      }

      table(const table&)            = delete;
      table& operator=(const table&) = delete;
      table(table&&)                 = default;
      table& operator=(table&&)      = default;

      // ── Mutations ─────────────────────────────────────────────────────

      void put(const T& row)
      {
         const auto pk_value = detail::invoke_extractor<Primary::extractor>(row);
         auto       pk_bytes = encode_primary_key(prefix_view(), pk_value);

         bool was_insert = false;
         if constexpr (num_secondaries > 0)
         {
            if (auto existing = _tx->template get<std::string>(detail::key_view_of(pk_bytes)))
            {
               // Existing row: extract old secondary keys via a view over the
               // encoded bytes (zero-copy on the diff path).
               auto v = psio::view<T, psio::pssz, psio::storage::const_borrow>{
                   std::span<const char>{existing->data(), existing->size()}};
               replace_secondaries_diff(v, row, pk_bytes,
                                        std::make_index_sequence<num_secondaries>{});
            }
            else
            {
               was_insert = true;
               write_all_secondaries(row, pk_bytes,
                                     std::make_index_sequence<num_secondaries>{});
            }
         }
         else if (_track_row_count)
         {
            // No secondaries → cheap probe for "row already there".
            was_insert = !_tx->template get<std::string>(
                              detail::key_view_of(pk_bytes))
                              .has_value();
         }

         auto packed = psio::encode(psio::pssz{}, row);
         _tx->upsert(detail::key_view_of(pk_bytes), detail::value_view_of(packed));

         if (was_insert && _track_row_count)
         {
            auto h = read_header();
            if (h.row_count.has_value())
               h.row_count = *h.row_count + 1;
            write_header(h);
         }
      }

      // Insert allocates the primary key from the table header's next_id
      // counter, sets row's primary-key field to the allocated id, and
      // writes the row. Returns the assigned primary key.
      //
      // Requires the primary extractor to be a member-pointer-to-data so
      // we can write the assigned id back into the row before encoding.
      // Compile error if the primary is a member function or composite key.
      template <typename Row = T>
      auto insert(Row row) -> primary_key_type
      {
         static_assert(
             std::is_member_object_pointer_v<decltype(Primary::extractor)>,
             "table::insert requires the primary extractor to be a "
             "data-member pointer (so the auto-allocated id can be assigned "
             "back into the row before write)");

         auto h     = read_header();
         auto id    = h.next_id;
         row.*Primary::extractor = static_cast<primary_key_type>(id);
         h.next_id  = id + 1;
         // Row-count bookkeeping is handled by put() below to avoid
         // double-counting; we only bump next_id here.
         write_header(h);

         put(row);
         return static_cast<primary_key_type>(id);
      }

      template <typename K>
      bool erase(const K& pk_value)
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);

         if constexpr (num_secondaries > 0)
         {
            auto existing = _tx->template get<std::string>(detail::key_view_of(pk_bytes));
            if (!existing)
               return false;
            auto v = psio::view<T, psio::pssz, psio::storage::const_borrow>{
                std::span<const char>{existing->data(), existing->size()}};
            erase_all_secondaries(v, pk_value,
                                  std::make_index_sequence<num_secondaries>{});
         }

         const bool removed = _tx->remove(detail::key_view_of(pk_bytes)) >= 0;
         if (removed && _track_row_count)
         {
            auto h = read_header();
            if (h.row_count.has_value() && *h.row_count > 0)
               h.row_count = *h.row_count - 1;
            write_header(h);
         }
         return removed;
      }

      bool remove(const T& row)
      {
         return erase(detail::invoke_extractor<Primary::extractor>(row));
      }

      // ── Reads ─────────────────────────────────────────────────────────

      // Primary-key lookup. Returns nullopt if not found.
      template <typename K>
      std::optional<T> get(const K& pk_value) const
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);
         auto raw      = _tx->template get<std::string>(detail::key_view_of(pk_bytes));
         if (!raw)
            return std::nullopt;
         return psio::decode<T>(psio::pssz{},
                                std::span<const char>(raw->data(), raw->size()));
      }

      template <typename K>
      bool contains(const K& pk_value) const
      {
         auto pk_bytes = encode_primary_key(prefix_view(), pk_value);
         return _tx->template get<std::string>(detail::key_view_of(pk_bytes)).has_value();
      }

      // Find a row by tag. For unique secondaries, returns the matching row
      // or nullopt. For the primary, returns the row at that primary key.
      // For non-unique secondaries, returns the FIRST row whose secondary
      // matches; use equal_range<Tag> for full iteration.
      template <typename Tag, typename K>
      std::optional<T> find(const K& key_value) const
      {
         using Idx = typename find_index_by_tag<Tag>::type;
         constexpr std::size_t one_based =
             find_index_by_tag<Tag>::position;  // 0 = primary, 1.. = secondaries

         if constexpr (one_based == 0)
         {
            return get(key_value);
         }
         else if constexpr (Idx::is_unique)
         {
            auto sk_bytes =
                encode_secondary_key(prefix_view(), one_based, key_value);
            auto pk_ref =
                _tx->template get<std::string>(detail::key_view_of(sk_bytes));
            if (!pk_ref)
               return std::nullopt;
            auto raw = _tx->template get<std::string>(
                psitri::key_view(pk_ref->data(), pk_ref->size()));
            if (!raw)
               return std::nullopt;
            return psio::decode<T>(
                psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
         }
         else
         {
            // Non-unique: encoded key is (sk, pk). Seek to lower_bound of
            // (sk_value, ...) and return the first match if its sk-prefix
            // is exactly sk_value.
            auto prefix =
                encode_secondary_key(prefix_view(), one_based, key_value);
            // For non-unique we encoded prefix||idx||to_key(sk). The
            // equal_range pattern: lower_bound at this prefix; first hit
            // whose key starts with this prefix is the first matching row.
            auto cur = _tx->lower_bound(detail::key_view_of(prefix));
            if (cur.is_end())
               return std::nullopt;
            auto k = cur.key();
            if (k.size() < prefix.size() ||
                std::memcmp(k.data(), prefix.data(), prefix.size()) != 0)
               return std::nullopt;
            // The cursor is on the secondary tree's key; its value is the
            // primary tree key bytes. Re-fetch primary.
            auto pk_ref = cur.template value<std::string>();
            if (!pk_ref)
               return std::nullopt;
            auto raw = _tx->template get<std::string>(
                psitri::key_view(pk_ref->data(), pk_ref->size()));
            if (!raw)
               return std::nullopt;
            return psio::decode<T>(
                psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
         }
      }

      // ── Iteration over the primary tree ──────────────────────────────

      class primary_iterator
      {
        public:
         using value_view = psio::view<T, psio::pssz, psio::storage::const_borrow>;

         primary_iterator(const table* tbl, psitri::cursor c)
             : _table(tbl), _cur(std::move(c))
         {
            check_in_range();
         }

         struct end_tag
         {
         };
         primary_iterator(const table* tbl, end_tag)
             : _table(tbl), _cur(tbl->_tx->read_cursor()), _at_end(true)
         {
         }

         // Default dereference returns an owned T. Convenient and lifetime-
         // safe; pays one decode per access. For zero-copy access see
         // `view(pin)` below.
         T operator*() const
         {
            auto v = _cur.template value<std::string>();
            return psio::decode<T>(
                psio::pssz{}, std::span<const char>(v->data(), v->size()));
         }

         // Zero-copy access. Caller provides a `psitri::value_pin` (typically
         // constructed once per iteration scope via `table::pin_values()`).
         // The returned view's bytes are valid as long as the pin lives and
         // the iterator stays at this row.
         value_view view(psitri::value_pin& pin) const noexcept
         {
            auto vv = _cur.value(pin);
            return value_view{std::span<const char>{vv.data(), vv.size()}};
         }

         primary_iterator& operator++()
         {
            _cur.next();
            check_in_range();
            return *this;
         }

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
            const auto kv       = _cur.key();
            const auto pfx_size = _table->_prefix.size() + 1;
            if (kv.size() < pfx_size ||
                std::memcmp(kv.data(), _table->_prefix.data(),
                            _table->_prefix.size()) != 0 ||
                static_cast<std::uint8_t>(kv.data()[_table->_prefix.size()]) !=
                    PRIMARY_PREFIX_BYTE)
            {
               _at_end = true;
            }
         }

         const table*   _table;
         psitri::cursor _cur;
         bool           _at_end = false;
      };

      // Construct a value_pin scoped to this table's session. Lifetime owns
      // the read lock; views obtained from iterators while the pin lives are
      // valid for direct mmap access.
      [[nodiscard]] psitri::value_pin pin_values() const
      {
         return _tx->read_cursor().pin_values();
      }

      primary_iterator begin() const
      {
         auto pfx = encode_index_prefix(prefix_view(), PRIMARY_PREFIX_BYTE);
         auto c   = _tx->lower_bound(psitri::key_view(pfx.data(), pfx.size()));
         return primary_iterator(this, std::move(c));
      }

      primary_iterator end() const
      {
         return primary_iterator(this, typename primary_iterator::end_tag{});
      }

      template <typename K>
      primary_iterator lower_bound(const K& pk_value) const
      {
         auto k = encode_primary_key(prefix_view(), pk_value);
         auto c = _tx->lower_bound(psitri::key_view(k.data(), k.size()));
         return primary_iterator(this, std::move(c));
      }

      template <typename K>
      primary_iterator upper_bound(const K& pk_value) const
      {
         auto k = encode_primary_key(prefix_view(), pk_value);
         auto c = _tx->upper_bound(psitri::key_view(k.data(), k.size()));
         return primary_iterator(this, std::move(c));
      }

      // ── Range query for non-unique secondary index ────────────────────
      //
      // A range over all rows whose secondary key (extracted by the index's
      // extractor) equals key_value. Iterates in (sk, pk) tree order; each
      // dereference yields a decoded row.

      template <typename Tag>
      class secondary_range_iterator
      {
        public:
         using index_type  = typename find_index_by_tag<Tag>::type;
         static_assert(!index_type::is_unique,
                       "secondary_range_iterator only valid for ordered_non_unique tags");

         secondary_range_iterator(const table*      tbl,
                                  psitri::cursor    c,
                                  std::vector<char> sk_prefix)
             : _table(tbl), _cur(std::move(c)), _sk_prefix(std::move(sk_prefix))
         {
            check_in_range();
         }

         struct end_tag
         {
         };
         secondary_range_iterator(const table* tbl, end_tag)
             : _table(tbl), _cur(tbl->_tx->read_cursor()), _at_end(true)
         {
         }

         T operator*() const
         {
            auto pk_ref = _cur.template value<std::string>();
            auto raw    = _table->_tx->template get<std::string>(
                psitri::key_view(pk_ref->data(), pk_ref->size()));
            return psio::decode<T>(
                psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
         }

         secondary_range_iterator& operator++()
         {
            _cur.next();
            check_in_range();
            return *this;
         }

         friend bool operator==(const secondary_range_iterator& a,
                                const secondary_range_iterator& b) noexcept
         {
            if (a._at_end || b._at_end)
               return a._at_end && b._at_end;
            const auto ak = a._cur.key();
            const auto bk = b._cur.key();
            return ak.size() == bk.size() &&
                   std::memcmp(ak.data(), bk.data(), ak.size()) == 0;
         }
         friend bool operator!=(const secondary_range_iterator& a,
                                const secondary_range_iterator& b) noexcept
         {
            return !(a == b);
         }

        private:
         void check_in_range()
         {
            if (_cur.is_end())
            {
               _at_end = true;
               return;
            }
            const auto k = _cur.key();
            if (k.size() < _sk_prefix.size() ||
                std::memcmp(k.data(), _sk_prefix.data(), _sk_prefix.size()) != 0)
               _at_end = true;
         }

         const table*      _table;
         psitri::cursor    _cur;
         std::vector<char> _sk_prefix;
         bool              _at_end = false;
      };

      template <typename Tag>
      struct secondary_range
      {
         secondary_range_iterator<Tag> _begin, _end;
         secondary_range_iterator<Tag> begin() const { return _begin; }
         secondary_range_iterator<Tag> end() const { return _end; }
      };

      template <typename Tag, typename K>
      secondary_range<Tag> equal_range(const K& key_value) const
      {
         using Idx = typename find_index_by_tag<Tag>::type;
         constexpr std::size_t one_based = find_index_by_tag<Tag>::position;
         static_assert(!Idx::is_unique,
                       "equal_range only valid for ordered_non_unique tags; "
                       "use find<Tag> for unique secondaries");

         auto prefix =
             encode_secondary_key(prefix_view(), one_based, key_value);
         auto c =
             _tx->lower_bound(psitri::key_view(prefix.data(), prefix.size()));
         return {secondary_range_iterator<Tag>(this, std::move(c), prefix),
                 secondary_range_iterator<Tag>(
                     this, typename secondary_range_iterator<Tag>::end_tag{})};
      }

      // ── Header / metadata ─────────────────────────────────────────────

      // Read the table header. Returns a default-constructed header when no
      // row has ever been written.
      table_header header() const { return read_header(); }

      // Returns the cached row count if tracking is enabled, otherwise
      // nullopt. Constant-time read of the header.
      std::optional<std::uint64_t> size() const
      {
         auto h = read_header();
         if (!(h.flags & TABLE_FLAG_ROW_COUNT_ENABLED))
            return std::nullopt;
         return h.row_count;
      }

      // ── Diagnostics ───────────────────────────────────────────────────

      std::string_view     prefix() const noexcept { return prefix_view(); }
      psitri::transaction& transaction() noexcept { return *_tx; }

     private:
      std::string_view prefix_view() const noexcept
      {
         return std::string_view(_prefix.data(), _prefix.size());
      }

      // Effective key extractor for a secondary index.
      //
      // For ordered_unique<Tag, Ex>:    just Ex(row).
      // For ordered_non_unique<Tag, Ex>: tuple(Ex(row), primary(row)) so the
      //                                   tree key is unique-by-construction.
      template <typename Idx, typename Row>
      static auto extract_secondary_key(const Row& row)
      {
         auto sk = detail::invoke_extractor<Idx::extractor>(row);
         if constexpr (Idx::is_unique)
            return sk;
         else
            return std::make_tuple(
                std::move(sk),
                detail::invoke_extractor<Primary::extractor>(row));
      }

      // Per-secondary helper: returns (one_based_index, encoded_secondary_key).
      template <std::size_t I, typename Row>
      auto encode_secondary_for_row(const Row& row) const
      {
         using Idx              = detail::nth_index_t<I, Secondaries...>;
         constexpr std::size_t one_based = I + 1;
         auto sk = extract_secondary_key<Idx>(row);
         return std::make_pair(one_based,
                               encode_secondary_key(prefix_view(), one_based, sk));
      }

      // Same but extracting from a view rather than a live row. Used on the
      // existing-record diff path for zero-copy reads.
      template <std::size_t I, typename View>
      auto encode_secondary_for_view(const View& view, const T& tmp_for_pk) const
      {
         // For now we materialize the field via the view's `get<N>()` and
         // re-encode. A future optimization is to extract directly from the
         // view's bytes when the extractor supports view input. We keep the
         // simpler form here; the actual avoided work is the full T decode,
         // not the per-field decode.
         (void)view;
         return encode_secondary_for_row<I>(tmp_for_pk);
      }

      // Write all secondaries for a fresh row (no existing record).
      template <typename Row, std::size_t... Is>
      void write_all_secondaries(const Row&         row,
                                 const std::vector<char>& pk_bytes,
                                 std::index_sequence<Is...>)
      {
         (write_one_secondary<Is>(row, pk_bytes), ...);
      }

      template <std::size_t I, typename Row>
      void write_one_secondary(const Row&               row,
                               const std::vector<char>& pk_bytes)
      {
         using Idx = detail::nth_index_t<I, Secondaries...>;
         auto [one_based, sk_bytes] = encode_secondary_for_row<I>(row);

         if constexpr (Idx::is_unique)
         {
            // Collision check: if this secondary key already exists and its
            // value (primary-key bytes) doesn't match the pk we're about to
            // write, reject.
            auto existing =
                _tx->template get<std::string>(detail::key_view_of(sk_bytes));
            if (existing)
            {
               const auto& bytes = *existing;
               if (bytes.size() != pk_bytes.size() ||
                   std::memcmp(bytes.data(), pk_bytes.data(), pk_bytes.size()) != 0)
               {
                  throw secondary_collision(
                      "psitri_multiindex: ordered_unique secondary key "
                      "already in use by a different primary");
               }
               // Same pk → no-op (re-put of identical row). Fall through.
            }
         }

         _tx->upsert(detail::key_view_of(sk_bytes),
                     detail::value_view_of(pk_bytes));
      }

      // Erase all secondaries for a row whose existence we've already
      // verified.  Pre-condition: the row's secondary entries point at this pk.
      template <typename View, typename PK, std::size_t... Is>
      void erase_all_secondaries(const View& v, const PK& pk_value,
                                 std::index_sequence<Is...>)
      {
         // For erase we need the actual stored secondary keys; decode the
         // row once and reuse for all Is.
         T row = psio::decode<T>(psio::pssz{}, psio::bytes(v));
         (void)pk_value;
         (erase_one_secondary<Is>(row), ...);
      }

      template <std::size_t I, typename Row>
      void erase_one_secondary(const Row& row)
      {
         auto [one_based, sk_bytes] = encode_secondary_for_row<I>(row);
         (void)one_based;
         _tx->remove(detail::key_view_of(sk_bytes));
      }

      // Diff-replace all secondaries given the existing row's view and the
      // new in-hand row.
      template <typename View, std::size_t... Is>
      void replace_secondaries_diff(const View&              old_view,
                                    const T&                 new_row,
                                    const std::vector<char>& new_pk_bytes,
                                    std::index_sequence<Is...>)
      {
         T old_row = psio::decode<T>(psio::pssz{}, psio::bytes(old_view));
         (diff_one_secondary<Is>(old_row, new_row, new_pk_bytes), ...);
      }

      template <std::size_t I>
      void diff_one_secondary(const T&                 old_row,
                              const T&                 new_row,
                              const std::vector<char>& new_pk_bytes)
      {
         using Idx                = detail::nth_index_t<I, Secondaries...>;
         constexpr std::size_t one = I + 1;

         auto old_sk_value = extract_secondary_key<Idx>(old_row);
         auto new_sk_value = extract_secondary_key<Idx>(new_row);
         if (old_sk_value == new_sk_value)
            return;  // unchanged — no work

         // Remove the old entry.
         auto old_bytes =
             encode_secondary_key(prefix_view(), one, old_sk_value);
         _tx->remove(detail::key_view_of(old_bytes));

         // Add the new entry, with collision check for unique indexes.
         if constexpr (Idx::is_unique)
         {
            auto new_bytes =
                encode_secondary_key(prefix_view(), one, new_sk_value);
            auto existing =
                _tx->template get<std::string>(detail::key_view_of(new_bytes));
            if (existing)
            {
               const auto& bytes = *existing;
               if (bytes.size() != new_pk_bytes.size() ||
                   std::memcmp(bytes.data(), new_pk_bytes.data(),
                               new_pk_bytes.size()) != 0)
               {
                  throw secondary_collision(
                      "psitri_multiindex: ordered_unique secondary key "
                      "would collide with a different primary on update");
               }
            }
            _tx->upsert(detail::key_view_of(new_bytes),
                        detail::value_view_of(new_pk_bytes));
         }
         else
         {
            auto new_bytes =
                encode_secondary_key(prefix_view(), one, new_sk_value);
            _tx->upsert(detail::key_view_of(new_bytes),
                        detail::value_view_of(new_pk_bytes));
         }
      }

      // ── Header storage ────────────────────────────────────────────────

      std::vector<char> header_key() const
      {
         std::vector<char> k;
         k.reserve(_prefix.size() + 2);
         k.insert(k.end(), _prefix.begin(), _prefix.end());
         k.push_back(static_cast<char>(META_PREFIX_BYTE));
         k.push_back(static_cast<char>(META_HEADER_SUBBYTE));
         return k;
      }

      table_header read_header() const
      {
         auto k   = header_key();
         auto raw = _tx->template get<std::string>(detail::key_view_of(k));
         if (!raw)
            return table_header{};
         return psio::decode<table_header>(
             psio::pssz{}, std::span<const char>(raw->data(), raw->size()));
      }

      void write_header(const table_header& h)
      {
         auto k      = header_key();
         auto packed = psio::encode(psio::pssz{}, h);
         _tx->upsert(detail::key_view_of(k), detail::value_view_of(packed));
      }

      psitri::transaction* _tx;
      std::vector<char>    _prefix;
      bool                 _track_row_count = false;
   };
}  // namespace psitri_multiindex
