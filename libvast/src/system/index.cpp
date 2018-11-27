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

#include <chrono>
#include <deque>
#include <unordered_set>

#include <caf/all.hpp>
#include <caf/detail/unordered_flat_map.hpp>

#include "vast/concept/parseable/to.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/bitmap.hpp"
#include "vast/concept/printable/vast/error.hpp"
#include "vast/concept/printable/vast/expression.hpp"
#include "vast/concept/printable/vast/uuid.hpp"
#include "vast/detail/assert.hpp"
#include "vast/event.hpp"
#include "vast/expression_visitors.hpp"
#include "vast/ids.hpp"
#include "vast/json.hpp"
#include "vast/load.hpp"
#include "vast/logger.hpp"
#include "vast/save.hpp"

#include "vast/system/accountant.hpp"
#include "vast/system/index.hpp"
#include "vast/system/partition.hpp"
#include "vast/system/task.hpp"

#include "vast/detail/cache.hpp"

using namespace caf;
using namespace std::chrono;

namespace vast::system {

namespace {

/// Maps partition IDs to INDEXER actors for resolving a query.
using query_map = caf::detail::unordered_flat_map<uuid, std::vector<actor>>;

[[maybe_unused]]
auto get_ids(query_map& xs) {
  std::vector<uuid> ys;
  ys.reserve(xs.size());
  std::transform(xs.begin(), xs.end(), std::back_inserter(ys),
                 [](auto& kvp) { return kvp.first; });
  return ys;
}

struct collector_state {
  caf::detail::unordered_flat_map<uuid, std::pair<size_t, ids>> open_requests;
  std::string name;
  collector_state(local_actor* self) : name("collector-") {
    name += std::to_string(self->id());
  }
};

behavior collector(stateful_actor<collector_state>* self, actor master) {
  // Ask master for initial work.
  self->send(master, worker_atom::value, self);
  return {
    [=](expression& expr, query_map& qm, actor& client) {
      VAST_DEBUG(self, "got a new query for", qm.size(), "partitions:",
                 get_ids(qm));
      VAST_ASSERT(self->state.open_requests.empty());
      for (auto& kvp : qm) {
        auto& id = kvp.first;
        auto& indexers = kvp.second;
        VAST_DEBUG(self, "asks", indexers.size(),
                   "INDEXER actor(s) for partition", id);
        self->state.open_requests[id] = std::make_pair(indexers.size(), ids{});
        for (auto& indexer : indexers)
          self->request(indexer, infinite, expr).then([=](ids& sub_result) {
            auto& [num_indexers, result] = self->state.open_requests[id];
            result |= sub_result;
            if (--num_indexers == 0) {
              VAST_DEBUG(self, "collected all sub results for partition", id);
              self->send(client, std::move(result));
              self->state.open_requests.erase(id);
              // Ask master for more work after receiving the last sub result.
              if (self->state.open_requests.empty()) {
                VAST_DEBUG(self, "asks INDEX for new work");
                self->send(master, worker_atom::value, self);
              }
            }
          });
      }
    }};
}

} // namespace <anonymous>

partition_ptr index_state::partition_factory::operator()(const uuid& id) const {
  // There are three options for loading a partition: 1) it is active, 2) it is
  // unpersisted, or 3) it needs be loaded from disk.
  auto& active = st_->active;
  if (active != nullptr && active->id() == id)
    return active;
  auto pred = [&](auto& kvp) { return kvp.first->id() == id; };
  auto& xs = st_->unpersisted;
  if (auto i = std::find_if(xs.begin(), xs.end(), pred); i != xs.end())
    return i->first;
  VAST_DEBUG(st_->self, "loads partition", id);
  return make_partition(st_->self->system(), st_->self, st_->dir, id);
}

index_state::index_state()
  // Arbitrary default value, overridden in ::init.
  : lru_partitions(10, partition_lookup{}, partition_factory{this}) {
  // nop
}

index_state::~index_state() {
  VAST_TRACE("");
  flush_to_disk();
}

caf::error index_state::init(event_based_actor* self, const path& dir,
                             size_t max_partition_size,
                             size_t in_mem_partitions,
                             size_t taste_partitions) {
  VAST_TRACE(VAST_ARG(dir), VAST_ARG(max_partition_size),
             VAST_ARG(in_mem_partitions), VAST_ARG(taste_partitions));
  // Set the synopsis factory for the meta index.
  if (auto factory = get_synopsis_factory(self->system())) {
    auto [id, fun] = *factory;
    VAST_DEBUG(name, "uses custom meta index synopsis factory", id);
    meta_idx.factory(id, fun);
  } else if (factory.error()) {
    VAST_ERROR(name, "failed to retrieve synopsis factory",
               self->system().render(factory.error()));
    return factory.error();
  } else {
    VAST_DEBUG(name, "uses default meta index synopsis factory");
  }
  meta_idx.set_synopsis_option("max-partition-size",
                               caf::config_value{max_partition_size});
  // Set members.
  this->self = self;
  this->dir = dir;
  this->max_partition_size = max_partition_size;
  this->lru_partitions.size(in_mem_partitions);
  this->taste_partitions = taste_partitions;
  // Read persistent state.
  if (auto err = load_from_disk())
    return err;
  // Callback for the stream stage for creating a new partition when the
  // current one becomes full.
  auto fac = [this]() -> partition_ptr {
    // Persist meta data and the state of all INDEXER actors when the active
    // partition becomes full.
    if (active != nullptr) {
      active->flush_to_disk();
      auto& mgr = active->manager();
      // Store this partition as unpersisted to make sure we're not attempting
      // to load it from disk until it is safe to do so.
      unpersisted.emplace_back(active, mgr.indexer_count());
      auto& id = active->id();
      mgr.for_each([&](const actor& indexer) {
        this->self->request(indexer, infinite, persist_atom::value).then([=] {
          auto pred = [=](auto& kvp) { return kvp.first->id() == id; };
          auto& xs = unpersisted;
          auto i = std::find_if(xs.begin(), xs.end(), pred);
          if (i == xs.end()) {
            VAST_ERROR(this->self,
                       "received an invalid response to a 'persist' message");
            return;
          }
          if (--i->second == 0) {
            VAST_DEBUG(this->self, "successfully persisted", id);
            xs.erase(i);
          }
        });
      });
    }
    // Create a new active partition.
    auto id = uuid::random();
    VAST_DEBUG(this->self, "starts a new partition:", id);
    active = make_partition(this->self->system(), this->self, this->dir, id);
    // Register the new active partition at the stream manager.
    return active;
  };
  stage = self->make_continuous_stage<indexer_stage_driver>(meta_idx, fac,
                                                            max_partition_size);
  return caf::none;
}

caf::error index_state::load_from_disk() {
  VAST_TRACE("");
  // Nothing to load is not an error.
  if (!exists(dir)) {
    VAST_DEBUG(self, "found no directory to load from");
    return caf::none;
  }
  if (auto fname = meta_index_filename(); exists(fname)) {
    if (auto err = load(self->system(), fname, meta_idx)) {
      VAST_ERROR(self, "failed to load meta index:",
                 self->system().render(err));
      return err;
    }
    VAST_INFO(self, "loaded meta index");
  }
  return caf::none;
}

caf::error index_state::flush_to_disk() {
  VAST_TRACE("");
  // Flush meta index to disk.
  if (auto err = save(self->system(), meta_index_filename(), meta_idx)) {
    VAST_ERROR(self, "failed to save meta index:",
               self->system().render(err));
    return err;
  } else {
    VAST_INFO(self, "saved meta index");
  }
  return caf::none;
}

path index_state::meta_index_filename() const {
  return dir / "meta";
}

bool index_state::worker_available() {
  return !idle_workers.empty();
}

caf::actor index_state::next_worker() {
  auto result = std::move(idle_workers.back());
  idle_workers.pop_back();
  return result;
}

behavior index(stateful_actor<index_state>* self, const path& dir,
               size_t max_partition_size, size_t in_mem_partitions,
               size_t taste_partitions, size_t num_workers) {
  VAST_ASSERT(max_partition_size > 0);
  VAST_ASSERT(in_mem_partitions > 0);
  VAST_INFO(self, "spawned:", VAST_ARG(max_partition_size),
            VAST_ARG(in_mem_partitions), VAST_ARG(taste_partitions));
  if (auto err = self->state.init(self, dir, max_partition_size,
                                  in_mem_partitions, taste_partitions)) {
    self->quit(std::move(err));
    return {};
  }
  auto accountant = accountant_type{};
  if (auto a = self->system().registry().get(accountant_atom::value))
    accountant = actor_cast<accountant_type>(a);
  auto locate_indexers = [=](const expression& expr, auto begin, auto end) {
    query_map result;
    for (; begin != end; ++begin) {
      auto& part = self->state.lru_partitions.get_or_add(*begin);
      auto indexers = part->get_indexers(expr);
      VAST_ASSERT(!indexers.empty());
      result.emplace(part->id(), std::move(indexers));
    }
    return result;
  };
  // Launch workers for resolving queries.
  for (size_t i = 0; i < num_workers; ++i)
    self->spawn(collector, self);
  // We switch between has_worker behavior and the default behavior (which
  // simply waits for a worker).
  self->set_default_handler(caf::skip);
  self->state.has_worker.assign(
    [=](expression& expr) -> result<uuid, size_t, size_t> {
      auto& st = self->state;
      // Sanity check.
      if (self->current_sender() == nullptr) {
        VAST_ERROR(self, "got an anonymous query (ignored)");
        return sec::invalid_argument;
      }
      // Get all potentially matching partitions.
      auto candidates = st.meta_idx.lookup(expr);
      // Report no result if no candidates are found.
      if (candidates.empty()) {
        VAST_DEBUG(self, "returns without result: no partitions qualify");
        return {uuid::nil(), 0, 0};
      }
      // Every return after this points uses up the worker.
      auto guard = caf::detail::make_scope_guard([&] {
        if (!st.worker_available())
          self->unbecome();
      });
      // Allows the client to query further results after initial taste.
      auto query_id = uuid::nil();
      // Store how many partitions hit and how many we scheduled for the
      // initial taste.
      size_t hits = candidates.size();
      size_t scheduled = st.taste_partitions;
      // Collects all INDEXER actors that we query for the initial taste.
      query_map qm;
      // Deliver everything in one shot if the candidate set fits into our
      // taste partitions threshold.
      if (hits <= st.taste_partitions) {
        VAST_DEBUG(self, "can schedule all partitions immediately");
        scheduled = hits;
        qm = locate_indexers(expr, candidates.begin(), candidates.end());
      } else {
        query_id = uuid::random();
        VAST_DEBUG(self, "schedules first", st.taste_partitions,
                   "partition(s) for query", query_id);
        // Prefer partitions that are currently in our cache.
        std::partition(candidates.begin(), candidates.end(),
                       [&](const uuid& candidate) {
                         return st.lru_partitions.contains(candidate);
                       });
        // Get all INDEXER actors for the taste and store remaining candidates
        // for later.
        auto first = candidates.begin();
        auto last_taste = first + st.taste_partitions;
        qm = locate_indexers(expr, first, last_taste);
        candidates.erase(first, last_taste);
        using ls = index_state::lookup_state;
        st.pending.emplace(query_id, ls{expr, std::move(candidates)});
      }
      self->send(st.next_worker(), std::move(expr), std::move(qm),
                 actor_cast<actor>(self->current_sender()));
      return {std::move(query_id), hits, scheduled};
    },
    [=](const uuid& query_id, size_t num_partitions) {
      auto& st = self->state;
      // A zero as second argument means the client drops further results.
      if (num_partitions == 0) {
        VAST_DEBUG(self, "dropped remaining results for query ID", query_id);
        st.pending.erase(query_id);
        return;
      }
      // Sanity checks.
      if (self->current_sender() == nullptr) {
        VAST_ERROR(self, "got an anonymous query (ignored)");
        return;
      }
      auto pending_iter = st.pending.find(query_id);
      if (pending_iter == st.pending.end()) {
        VAST_WARNING(self, "got a request for unknown query ID", query_id);
        return;
      }
      VAST_DEBUG(self, "schedules", num_partitions,
                 "more partition(s) for query ID", query_id);
      // Every return after this points uses up the worker.
      auto guard = caf::detail::make_scope_guard([&] {
        if (!st.worker_available())
          self->unbecome();
      });
      // Prefer partitions that are currently in our cache.
      auto& candidates = pending_iter->second.partitions;
      std::partition(candidates.begin(), candidates.end(),
                     [&](const uuid& candidate) {
                       return st.lru_partitions.contains(candidate);
                     });
      // Collect all INDEXER actors that we need to query.
      auto& expr = pending_iter->second.expr;
      auto first = candidates.begin();
      auto last = first + std::min(num_partitions, candidates.size());
      auto qm = locate_indexers(expr, first, last);
      // Forward request to worker.
      self->send(st.next_worker(), expr, std::move(qm),
                 actor_cast<actor>(self->current_sender()));
      // Cleanup.
      if (last == candidates.end()) {
        VAST_DEBUG(self, "exhausted all partitions for query ID", query_id);
        st.pending.erase(pending_iter);
      } else {
        candidates.erase(first, last);
        VAST_DEBUG(self, "has", candidates.size(),
                   "partitions left for query ID", query_id);
      }
    },
    [=](worker_atom, caf::actor& worker) {
      self->state.idle_workers.emplace_back(std::move(worker));
    },
    [=](caf::stream<table_slice_ptr> in) {
      VAST_DEBUG(self, "got a new source");
      return self->state.stage->add_inbound_path(in);
    }
  );
  return {
    [=](worker_atom, caf::actor& worker) {
      auto& st = self->state;
      st.idle_workers.emplace_back(std::move(worker));
      self->become(keep_behavior, st.has_worker);
    },
    [=](caf::stream<table_slice_ptr> in) {
      VAST_DEBUG(self, "got a new source");
      return self->state.stage->add_inbound_path(in);
    }
  };
}

} // namespace vast::system
