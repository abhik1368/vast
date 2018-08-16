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

#include "vast/data.hpp"

#include "vast/concept/printable/caf/ip_address.hpp"
#include "vast/concept/printable/caf/ip_subnet.hpp"
#include "vast/concept/printable/core/printer.hpp"
#include "vast/concept/printable/numeric.hpp"
#include "vast/concept/printable/print.hpp"
#include "vast/concept/printable/std/chrono.hpp"
#include "vast/concept/printable/string.hpp"
#include "vast/concept/printable/vast/none.hpp"
#include "vast/concept/printable/vast/pattern.hpp"
#include "vast/concept/printable/vast/port.hpp"
#include "vast/concept/printable/vast/type.hpp"

#include "vast/detail/overload.hpp"
#include "vast/detail/string.hpp"

namespace vast {

struct data_printer : printer<data_printer> {
  using attribute = data;

  template <class Iterator>
  bool print(Iterator& out, const data& d) const {
    return caf::visit(detail::overload(
      [&](const auto& x) {
        return make_printer<std::decay_t<decltype(x)>>{}(out, x);
      },
      [&](integer x) {
        return printers::integral<integer, policy::force_sign>(out, x);
      },
      [&](const std::string& x) {
        // TODO: create a printer that escapes the output on the fly, as opposed
        // to going through an extra copy.
        auto escaped = printers::str ->* [](const std::string& str) {
          return detail::byte_escape(str, "\"");
        };
        auto p = '"' << escaped << '"';
        return p(out, x);
      }
    ), d);
  }
};

template <>
struct printer_registry<data> {
  using type = data_printer;
};

namespace printers {
  auto const data = data_printer{};
} // namespace printers

struct vector_printer : printer<vector_printer> {
  using attribute = vector;

  template <class Iterator>
  bool print(Iterator& out, const vector& v) const {
    auto p = '[' << ~(data_printer{} % ", ") << ']';
    return p.print(out, v);
  }
};

template <>
struct printer_registry<vector> {
  using type = vector_printer;
};

struct set_printer : printer<set_printer> {
  using attribute = set;

  template <class Iterator>
  bool print(Iterator& out, const set& s) const {
    auto p = '{' << ~(data_printer{} % ", ") << '}';
    return p.print(out, s);
  }
};

template <>
struct printer_registry<set> {
  using type = set_printer;
};

struct map_printer : printer<map_printer> {
  using attribute = map;

  template <class Iterator>
  bool print(Iterator& out, const map& t) const {
    auto pair = (data_printer{} << " -> " << data_printer{});
    auto p = '{' << ~(pair % ", ") << '}';
    return p.print(out, t);
  }
};

template <>
struct printer_registry<map> {
  using type = map_printer;
};

} // namespace vast
