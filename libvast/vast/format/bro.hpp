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

#pragma once

#include <chrono>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <caf/ip_address.hpp>
#include <caf/ip_subnet.hpp>

#include "vast/concept/parseable/caf/ip_address.hpp"
#include "vast/concept/parseable/caf/ip_subnet.hpp"
#include "vast/concept/parseable/core.hpp"
#include "vast/concept/parseable/numeric.hpp"
#include "vast/concept/parseable/string/any.hpp"
#include "vast/data.hpp"
#include "vast/expected.hpp"
#include "vast/filesystem.hpp"
#include "vast/schema.hpp"

#include "vast/detail/line_range.hpp"
#include "vast/detail/string.hpp"

namespace vast {

class event;

namespace format {
namespace bro {

/// Parses non-container types.
template <class Iterator, class Attribute>
struct bro_parser {
  bro_parser(Iterator& f, const Iterator& l, Attribute& attr)
    : f_{f},
      l_{l},
      attr_{attr} {
  }

  template <class Parser>
  bool parse(const Parser& p) const {
    return p(f_, l_, attr_);
  }

  template <class T>
  bool operator()(const T&) const {
    return false;
  }

  bool operator()(const boolean_type&) const {
    return parse(parsers::tf);
  }

  bool operator()(const integer_type&) const {
    static auto p = parsers::i64 ->* [](integer x) { return x; };
    return parse(p);
  }

  bool operator()(const count_type&) const {
    static auto p = parsers::u64 ->* [](count x) { return x; };
    return parse(p);
  }

  bool operator()(const timestamp_type&) const {
    static auto p = parsers::real ->* [](real x) {
      auto i = std::chrono::duration_cast<timespan>(double_seconds(x));
      return timestamp{i};
    };
    return parse(p);
  }

  bool operator()(const timespan_type&) const {
    static auto p = parsers::real ->* [](real x) {
      return std::chrono::duration_cast<timespan>(double_seconds(x));
    };
    return parse(p);
  }

  bool operator()(const string_type&) const {
    static auto p = +parsers::any
      ->* [](std::string x) { return detail::byte_unescape(x); };
    return parse(p);
  }

  bool operator()(const pattern_type&) const {
    static auto p = +parsers::any
      ->* [](std::string x) { return detail::byte_unescape(x); };
    return parse(p);
  }

  bool operator()(const ip_address_type&) const {
    static auto p = parsers::addr ->* [](caf::ip_address x) { return x; };
    return parse(p);
  }

  bool operator()(const ip_subnet_type&) const {
    static auto p = parsers::net ->* [](caf::ip_subnet x) { return x; };
    return parse(p);
  }

  bool operator()(const port_type&) const {
    static auto p = parsers::u16
      ->* [](uint16_t x) { return port{x, port::unknown}; };
    return parse(p);
  }

  Iterator& f_;
  const Iterator& l_;
  Attribute& attr_;
};

/// Constructs a polymorphic Bro data parser.
template <class Iterator, class Attribute>
struct bro_parser_factory {
  using result_type = rule<Iterator, Attribute>;

  bro_parser_factory(const std::string& set_separator)
    : set_separator_{set_separator} {
  }

  template <class T>
  result_type operator()(const T&) const {
    return {};
  }

  result_type operator()(const boolean_type&) const {
    return parsers::tf;
  }

  result_type operator()(const integer_type&) const {
    return parsers::i64 ->* [](integer x) { return x; };
  }

  result_type operator()(const count_type&) const {
    return parsers::u64 ->* [](count x) { return x; };
  }

  result_type operator()(const timestamp_type&) const {
    return parsers::real ->* [](real x) {
      auto i = std::chrono::duration_cast<timespan>(double_seconds(x));
      return timestamp{i};
    };
  }

  result_type operator()(const timespan_type&) const {
    return parsers::real ->* [](real x) {
      return std::chrono::duration_cast<timespan>(double_seconds(x));
    };
  }

  result_type operator()(const string_type&) const {
    if (set_separator_.empty())
      return +parsers::any
        ->* [](std::string x) { return detail::byte_unescape(x); };
    else
      return +(parsers::any - set_separator_)
               ->*[](std::string x) { return detail::byte_unescape(x); };
  }

  result_type operator()(const pattern_type&) const {
    if (set_separator_.empty())
      return +parsers::any
        ->* [](std::string x) { return detail::byte_unescape(x); };
    else
      return +(parsers::any - set_separator_)
        ->* [](std::string x) { return detail::byte_unescape(x); };
  }

  result_type operator()(const ip_address_type&) const {
    return parsers::addr ->* [](caf::ip_address x) { return x; };
  }

  result_type operator()(const ip_subnet_type&) const {
    return parsers::net ->* [](caf::ip_subnet x) { return x; };
  }

  result_type operator()(const port_type&) const {
    return parsers::u16 ->* [](uint16_t x) { return port{x, port::unknown}; };
  }

  result_type operator()(const set_type& t) const {
    auto set_insert = [](std::vector<Attribute> v) {
      set s;
      for (auto& x : v)
        s.insert(std::move(x));
      return s;
    };
    return (caf::visit(*this, t.value_type) % set_separator_) ->* set_insert;
  }

  result_type operator()(const vector_type& t) const {
    return (caf::visit(*this, t.value_type) % set_separator_)
      ->* [](std::vector<Attribute> x) { return vector(std::move(x)); };
  }

  const std::string& set_separator_;
};

/// Constructs a Bro data parser from a type and set separator.
template <class Iterator, class Attribute = data>
rule<Iterator, Attribute>
make_bro_parser(const type& t, const std::string& set_separator = ",") {
  rule<Iterator, Attribute> r;
  auto sep = is_container(t) ? set_separator : "";
  return caf::visit(bro_parser_factory<Iterator, Attribute>{sep}, t);
}

/// Parses non-container Bro data.
template <class Iterator, class Attribute = data>
bool bro_basic_parse(const type& t, Iterator& f, const Iterator& l,
                     Attribute& attr) {
  return caf::visit(bro_parser<Iterator, Attribute>{f, l, attr}, t);
}

/// A Bro reader.
class reader {
public:
  reader() = default;

  /// Constructs a Bro reader.
  /// @param input The stream of logs to read.
  explicit reader(std::unique_ptr<std::istream> in);

  void reset(std::unique_ptr<std::istream> in);

  expected<event> read();

  expected<void> schema(const vast::schema& sch);

  expected<vast::schema> schema() const;

  const char* name() const;

private:
  using iterator_type = std::string_view::const_iterator;

  expected<void> parse_header();

  std::unique_ptr<std::istream> input_;
  std::unique_ptr<detail::line_range> lines_;
  std::string separator_ = " ";
  std::string set_separator_;
  std::string empty_field_;
  std::string unset_field_;
  int timestamp_field_ = -1;
  vast::schema schema_;
  type type_;
  record_type record_;
  std::vector<rule<iterator_type, data>> parsers_;
};

/// A Bro writer.
class writer {
public:
  writer() = default;
  writer(writer&&) = default;
  writer& operator=(writer&&) = default;

  /// Constructs a Bro writer.
  /// @param dir The path where to write the log file(s) to.
  writer(path dir);

  ~writer();

  expected<void> write(const event& e);

  expected<void> flush();

  const char* name() const;

private:
  path dir_;
  std::unordered_map<std::string, std::unique_ptr<std::ostream>> streams_;
};

} // namespace bro
} // namespace format
} // namespace vast

