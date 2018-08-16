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

#include "vast/format/bgpdump.hpp"

namespace vast {
namespace format {
namespace bgpdump {

bgpdump_parser::bgpdump_parser() {
  // Announce type.
  auto fields = std::vector<record_field>{
    {"timestamp", timestamp_type{}},
    {"source_ip", ip_address_type{}},
    {"source_as", count_type{}},
    {"prefix", ip_subnet_type{}},
    {"as_path", vector_type{count_type{}}},
    {"origin_as", count_type{}},
    {"origin", string_type{}},
    {"nexthop", ip_address_type{}},
    {"local_pref", count_type{}},
    {"med", count_type{}},
    {"community", string_type{}},
    {"atomic_aggregate", string_type{}},
    {"aggregator", string_type{}},
  };
  announce_type = record_type{fields}.name("bgpdump::announcement");
  // Route & withdraw type.
  route_type = record_type{std::move(fields)}.name("bgpdump::routing");
  auto withdraw_fields = std::vector<record_field>{
    {"timestamp", timestamp_type{}},
    {"source_ip", ip_address_type{}},
    {"source_as", count_type{}},
    {"prefix", ip_subnet_type{}},
  };
  withdraw_type =
    record_type{std::move(withdraw_fields)}.name("bgpdump::withdrawn");
  // State-change type.
  auto state_change_fields = std::vector<record_field>{
    {"timestamp", timestamp_type{}},
    {"source_ip", ip_address_type{}},
    {"source_as", count_type{}},
    {"old_state", string_type{}},
    {"new_state", string_type{}},
  };
  state_change_type =
    record_type{std::move(state_change_fields)}.name("bgpdump::state_change");
}

expected<void> reader::schema(const vast::schema& sch) {
  auto xs = {
    &parser_.announce_type,
    &parser_.route_type,
    &parser_.withdraw_type,
    &parser_.state_change_type,
  };
  return replace_if_congruent(xs, sch);
}

expected<schema> reader::schema() const {
  vast::schema sch;
  sch.add(parser_.announce_type);
  sch.add(parser_.route_type);
  sch.add(parser_.withdraw_type);
  sch.add(parser_.state_change_type);
  return sch;
}

const char* reader::name() const {
  return "bgpdump-reader";
}

} // namespace bgpdump
} // namespace format
} // namespace vast
