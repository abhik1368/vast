/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#include <cmath>

#include "vast/base.hpp"
#include "vast/concept/parseable/numeric/integral.hpp"
#include "vast/concept/parseable/to.hpp"
#include "vast/concept/parseable/vast/base.hpp"
#include "vast/value_index.hpp"

namespace vast {
namespace {

optional<std::string> extract_attribute(const type& t, const std::string& key) {
  for (auto& attr : t.attributes())
    if (attr.key == key && attr.value)
      return attr.value;
  return {};
}

optional<base> parse_base(const type& t) {
  if (auto a = extract_attribute(t, "base")) {
    if (auto b = to<base>(*a))
      return *b;
    return {};
  }
  return base::uniform<64>(10);
}

} // namespace <anonymous>

// -- value_index --------------------------------------------------------------

value_index::~value_index() {
  // nop
}

std::unique_ptr<value_index> value_index::make(const type& t) {
  struct factory {
    using result_type = std::unique_ptr<value_index>;
    result_type operator()(const none_type&) const {
      return nullptr;
    }
    result_type operator()(const boolean_type&) const {
      return std::make_unique<arithmetic_index<boolean>>();
    }
    result_type operator()(const integer_type& t) const {
      auto b = parse_base(t);
      if (!b)
        return nullptr;
      return std::make_unique<arithmetic_index<integer>>(std::move(*b));
    }
    result_type operator()(const count_type& t) const {
      auto b = parse_base(t);
      if (!b)
        return nullptr;
      return std::make_unique<arithmetic_index<count>>(std::move(*b));
    }
    result_type operator()(const real_type& t) const {
      auto b = parse_base(t);
      if (!b)
        return nullptr;
      return std::make_unique<arithmetic_index<real>>(std::move(*b));
    }
    result_type operator()(const timespan_type& t) const {
      auto b = parse_base(t);
      if (!b)
        return nullptr;
      return std::make_unique<arithmetic_index<timespan>>(std::move(*b));
    }
    result_type operator()(const timestamp_type& t) const {
      auto b = parse_base(t);
      if (!b)
        return nullptr;
      return std::make_unique<arithmetic_index<timestamp>>(std::move(*b));
    }
    result_type operator()(const string_type& t) const {
      auto max_length = size_t{1024};
      if (auto a = extract_attribute(t, "max_length")) {
        if (auto x = to<size_t>(*a))
          max_length = *x;
        else
          return nullptr;
      }
      return std::make_unique<string_index>(max_length);
    }
    result_type operator()(const pattern_type&) const {
      return nullptr;
    }
    result_type operator()(const ip_address_type&) const {
      return std::make_unique<address_index>();
    }
    result_type operator()(const ip_subnet_type&) const {
      return std::make_unique<subnet_index>();
    }
    result_type operator()(const port_type&) const {
      return std::make_unique<port_index>();
    }
    result_type operator()(const enumeration_type&) const {
      return nullptr;
    }
    result_type operator()(const vector_type& t) const {
      auto max_size = size_t{1024};
      if (auto a = extract_attribute(t, "max_size")) {
        if (auto x = to<size_t>(*a))
          max_size = *x;
        else
          return nullptr;
      }
      return std::make_unique<sequence_index>(t.value_type, max_size);
    }
    result_type operator()(const set_type& t) const {
      auto max_size = size_t{1024};
      if (auto a = extract_attribute(t, "max_size")) {
        if (auto x = to<size_t>(*a))
          max_size = *x;
        else
          return nullptr;
      }
      return std::make_unique<sequence_index>(t.value_type, max_size);
    }
    result_type operator()(const map_type&) const {
      return nullptr;
    }
    result_type operator()(const record_type&) const {
      return nullptr;
    }
    result_type operator()(const alias_type& t) const {
      return caf::visit(*this, t.value_type);
    }
  };
  return caf::visit(factory{}, t);
}

expected<void> value_index::append(data_view x) {
  return append(x, offset());
}

expected<void> value_index::append(data_view x, id pos) {
  auto off = mask_.size();
  if (pos < off)
    // Can only append at the end
    return make_error(ec::unspecified, pos, '<', off);
  if (caf::holds_alternative<caf::none_t>(x)) {
    none_.append_bits(false, pos - none_.size());
    none_.append_bit(true);
  } else if (!append_impl(x, pos)) {
    return make_error(ec::unspecified, "append_impl");
  }
  mask_.append_bits(false, pos - off);
  mask_.append_bit(true);
  return {};
}

expected<ids> value_index::lookup(relational_operator op, data_view x) const {
  if (caf::holds_alternative<caf::none_t>(x)) {
    if (op == equal)
      return none_ & mask_;
    if (op == not_equal)
      return ~none_ & mask_;
    return make_error(ec::unsupported_operator, op);
  }
  auto result = lookup_impl(op, x);
  if (!result)
    return result;
  return (*result - none_) & mask_;
}

value_index::size_type value_index::offset() const {
  return mask_.size();
}

// -- string_index -------------------------------------------------------------

string_index::string_index(size_t max_length) : max_length_{max_length} {
}

void string_index::init() {
  if (length_.coder().storage().empty()) {
    size_t components = std::log10(max_length_);
    if (max_length_ % 10 != 0)
      ++components;
    length_ = length_bitmap_index{base::uniform(10, components)};
  }
}

bool string_index::append_impl(data_view x, id pos) {
  auto str = caf::get_if<view<std::string>>(&x);
  if (!str)
    return false;
  init();
  auto length = str->size();
  if (length > max_length_)
    length = max_length_;
  if (length > chars_.size())
    chars_.resize(length, char_bitmap_index{8});
  for (auto i = 0u; i < length; ++i) {
    chars_[i].skip(pos - chars_[i].size());
    chars_[i].append(static_cast<uint8_t>((*str)[i]));
  }
  length_.skip(pos - length_.size());
  length_.append(length);
  return true;
}

expected<ids>
string_index::lookup_impl(relational_operator op, data_view x) const {
  return caf::visit(detail::overload(
    [&](auto x) -> expected<ids> {
      return make_error(ec::type_clash, materialize(x));
    },
    [&](view<std::string> str) -> expected<ids> {
      auto str_size = str.size();
      if (str_size > max_length_)
        str_size = max_length_;
      switch (op) {
        default:
          return make_error(ec::unsupported_operator, op);
        case equal:
        case not_equal: {
          if (str_size == 0) {
            auto result = length_.lookup(equal, 0);
            if (op == not_equal)
              result.flip();
            return result;
          }
          if (str_size > chars_.size())
            return ids{offset(), op == not_equal};
          auto result = length_.lookup(less_equal, str_size);
          if (all<0>(result))
            return ids{offset(), op == not_equal};
          for (auto i = 0u; i < str_size; ++i) {
            auto b = chars_[i].lookup(equal, static_cast<uint8_t>(str[i]));
            result &= b;
            if (all<0>(result))
              return ids{offset(), op == not_equal};
          }
          if (op == not_equal)
            result.flip();
          return result;
        }
        case ni:
        case not_ni: {
          if (str_size == 0)
            return ids{offset(), op == ni};
          if (str_size > chars_.size())
            return ids{offset(), op == not_ni};
          // TODO: Be more clever than iterating over all k-grams (#45).
          ids result{offset(), false};
          for (auto i = 0u; i < chars_.size() - str_size + 1; ++i) {
            ids substr{offset(), true};
            auto skip = false;
            for (auto j = 0u; j < str_size; ++j) {
              auto bm = chars_[i + j].lookup(equal, str[j]);
              if (all<0>(bm)) {
                skip = true;
                break;
              }
              substr &= bm;
            }
            if (!skip)
              result |= substr;
          }
          if (op == not_ni)
            result.flip();
          return result;
        }
      }
    },
    [&](view<vector> xs) { return detail::container_lookup(*this, op, xs); },
    [&](view<set> xs) { return detail::container_lookup(*this, op, xs); }
  ), x);
}

// -- address_index ------------------------------------------------------------

void address_index::init() {
  if (bytes_[0].coder().storage().empty())
    // Initialize on first to make deserialization feasible.
    bytes_.fill(byte_index{8});
}

bool address_index::append_impl(data_view x, id pos) {
  init();
  auto addr = caf::get_if<view<caf::ip_address>>(&x);
  if (!addr)
    return false;
  auto& bytes = addr->data();
  for (auto i = 0u; i < 16; ++i) {
    bytes_[i].skip(pos - bytes_[i].size());
    bytes_[i].append(bytes[i]);
  }
  v4_.skip(pos - v4_.size());
  v4_.append(addr->embeds_v4());
  return true;
}

expected<ids>
address_index::lookup_impl(relational_operator op, data_view d) const {
  return caf::visit(detail::overload(
    [&](auto x) -> expected<ids> {
      return make_error(ec::type_clash, materialize(x));
    },
    [&](view<caf::ip_address> x) -> expected<ids> {
      if (!(op == equal || op == not_equal))
        return make_error(ec::unsupported_operator, op);
      auto result = x.embeds_v4() ? v4_.coder().storage() : ids{offset(), true};
      for (auto i = x.embeds_v4() ? 12u : 0u; i < 16; ++i) {
        auto bm = bytes_[i].lookup(equal, x.data()[i]);
        result &= bm;
        if (all<0>(result))
          return ids{offset(), op == not_equal};
      }
      if (op == not_equal)
        result.flip();
      return result;
    },
    [&](view<caf::ip_subnet> x) -> expected<ids> {
      if (!(op == in || op == not_in))
        return make_error(ec::unsupported_operator, op);
      auto topk = x.prefix_length();
      if (topk == 0)
        return make_error(ec::unspecified, "invalid IP subnet length: ", topk);
      auto is_v4 = x.network_address().embeds_v4();
      if ((is_v4 ? topk + 96 : topk) == 128)
        // Asking for /32 or /128 membership is equivalent to an equality lookup.
        return lookup_impl(op == in ? equal : not_equal, x.network_address());
      auto result = is_v4 ? v4_.coder().storage() : ids{offset(), true};
      auto& bytes = x.network_address().data();
      size_t i = is_v4 ? 12 : 0;
      for ( ; i < 16 && topk >= 8; ++i, topk -= 8)
        result &= bytes_[i].lookup(equal, bytes[i]);
      for (auto j = 0u; j < topk; ++j) {
        auto bit = 7 - j;
        auto& bm = bytes_[i].coder().storage()[bit];
        result &= (bytes[i] >> bit) & 1 ? ~bm : bm;
      }
      if (op == not_in)
        result.flip();
      return result;
    },
    [&](view<vector> xs) { return detail::container_lookup(*this, op, xs); },
    [&](view<set> xs) { return detail::container_lookup(*this, op, xs); }
  ), d);
}

// -- subnet_index -------------------------------------------------------------

void subnet_index::init() {
  if (length_.coder().storage().empty())
    length_ = prefix_index{128 + 1}; // Valid prefixes range from /0 to /128.
}

bool subnet_index::append_impl(data_view x, id pos) {
  if (auto sn = caf::get_if<view<caf::ip_subnet>>(&x)) {
    init();
    length_.skip(pos - length_.size());
    length_.append(sn->prefix_length());
    return static_cast<bool>(network_.append(sn->network_address(), pos));
  }
  return false;
}

expected<ids>
subnet_index::lookup_impl(relational_operator op, data_view d) const {
  return caf::visit(detail::overload(
    [&](auto x) -> expected<ids> {
      return make_error(ec::type_clash, materialize(x));
    },
    [&](view<caf::ip_subnet> x) -> expected<ids> {
      switch (op) {
        default:
          return make_error(ec::unsupported_operator, op);
        case equal:
        case not_equal: {
          auto result = network_.lookup(equal, x.network_address());
          if (!result)
            return result;
          auto n = length_.lookup(equal, x.prefix_length());
          *result &= n;
          if (op == not_equal)
            result->flip();
          return result;
        }
        case in:
        case not_in: {
          // For a subnet index U and subnet x, the in operator signifies a
          // subset relationship such that `U in x` translates to U ⊆ x, i.e.,
          // the lookup returns all subnets in U that are a subset of x.
          auto result = network_.lookup(in, x);
          if (!result)
            return result;
          *result &= length_.lookup(greater_equal, x.prefix_length());
          if (op == not_in)
            result->flip();
          return result;
        }
        case ni:
        case not_ni: {
          // For a subnet index U and subnet x, the ni operator signifies a
          // subset relationship such that `U ni x` translates to U ⊇ x, i.e.,
          // the lookup returns all subnets in U that include x.
          ids result;
          for (uint8_t i = 1; i <= x.prefix_length(); ++i) {
            auto xs = network_.lookup(in,
                                      caf::ip_subnet{x.network_address(), i});
            if (!xs)
              return xs;
            *xs &= length_.lookup(equal, i);
            result |= *xs;
          }
          if (op == not_ni)
            result.flip();
          return result;
        }
      }
    },
    [&](view<vector> xs) { return detail::container_lookup(*this, op, xs); },
    [&](view<set> xs) { return detail::container_lookup(*this, op, xs); }
  ), d);
}

// -- port_index ---------------------------------------------------------------

void port_index::init() {
  if (num_.coder().storage().empty()) {
    num_ = number_index{base::uniform(10, 5)}; // [0, 2^16)
    proto_ = protocol_index{4}; // unknown, tcp, udp, icmp
  }
}

bool port_index::append_impl(data_view x, id pos) {
  if (auto p = caf::get_if<view<port>>(&x)) {
    init();
    num_.skip(pos - num_.size());
    num_.append(p->number());
    proto_.skip(pos - proto_.size());
    proto_.append(p->type());
    return true;
  }
  return false;
}

expected<ids>
port_index::lookup_impl(relational_operator op, data_view d) const {
  if (offset() == 0) // FIXME: why do we need this check again?
    return ids{};
  return caf::visit(detail::overload(
    [&](auto x) -> expected<ids> {
      return make_error(ec::type_clash, materialize(x));
    },
    [&](view<port> x) -> expected<ids> {
      if (op == in || op == not_in)
        return make_error(ec::unsupported_operator, op);
      auto n = num_.lookup(op, x.number());
      if (all<0>(n))
        return ids{offset(), false};
      if (x.type() != port::unknown)
        n &= proto_.lookup(equal, x.type());
      return n;
    },
    [&](view<vector> xs) { return detail::container_lookup(*this, op, xs); },
    [&](view<set> xs) { return detail::container_lookup(*this, op, xs); }
  ), d);
}

// -- sequence_index -----------------------------------------------------------

sequence_index::sequence_index(vast::type t, size_t max_size)
  : max_size_{max_size},
    value_type_{std::move(t)} {
}

void sequence_index::init() {
  if (size_.coder().storage().empty()) {
    size_t components = std::log10(max_size_);
    if (max_size_ % 10 != 0)
      ++components;
    size_ = size_bitmap_index{base::uniform(10, components)};
  }
}

bool sequence_index::append_impl(data_view x, id pos) {
  if (auto xs = caf::get_if<view<vector>>(&x))
    return container_append(**xs, pos);
  if (auto xs = caf::get_if<view<set>>(&x))
    return container_append(**xs, pos);
  return false;
}

expected<ids>
sequence_index::lookup_impl(relational_operator op, data_view x) const {
  if (!(op == ni || op == not_ni))
    return make_error(ec::unsupported_operator, op);
  if (elements_.empty())
    return ids{};
  auto result = elements_[0]->lookup(equal, x);
  if (!result)
    return result;
  for (auto i = 1u; i < elements_.size(); ++i) {
    auto mbm = elements_[i]->lookup(equal, x);
    if (mbm)
      *result |= *mbm;
    else
      return mbm;
  }
  if (op == not_ni)
    result->flip();
  return result;
}

void serialize(caf::serializer& sink, const sequence_index& idx) {
  sink & static_cast<const value_index&>(idx);
  sink & idx.value_type_;
  sink & idx.max_size_;
  sink & idx.size_;
  // Polymorphic indexes.
  std::vector<detail::value_index_inspect_helper> xs;
  xs.reserve(idx.elements_.size());
  std::transform(idx.elements_.begin(),
                 idx.elements_.end(),
                 std::back_inserter(xs),
                 [&](auto& vi) {
                   auto& t = const_cast<type&>(idx.value_type_);
                   auto& x = const_cast<std::unique_ptr<value_index>&>(vi);
                   return detail::value_index_inspect_helper{t, x};
                 });
  sink & xs;
}

void serialize(caf::deserializer& source, sequence_index& idx) {
  source & static_cast<value_index&>(idx);
  source & idx.value_type_;
  source & idx.max_size_;
  source & idx.size_;
  // Polymorphic indexes.
  size_t n;
  auto construct = [&] {
    idx.elements_.resize(n);
    std::vector<detail::value_index_inspect_helper> xs;
    xs.reserve(n);
    std::transform(idx.elements_.begin(),
                   idx.elements_.end(),
                   std::back_inserter(xs),
                   [&](auto& vi) {
                     auto& t = idx.value_type_;
                     auto& x = vi;
                     return detail::value_index_inspect_helper{t, x};
                   });
    for (auto& x : xs)
      source & x;
    return error{};
  };
  auto e = error::eval(
    [&] { return source.begin_sequence(n); },
    [&] { return construct(); },
    [&] { return source.end_sequence(); }
  );
  if (e)
    VAST_RAISE_ERROR("cannot serialize sequence index");
}

} // namespace vast
