/**
 * @file src/hdr_runtime_owner.cpp
 * @brief Thread-safe ownership and conditional rollback for runtime overrides.
 */
#include "hdr_runtime_owner.h"

#include <algorithm>
#include <utility>

namespace hdr_runtime_owner {
  namespace {
    std::uint64_t next_id(std::uint64_t &counter) {
      // Zero is reserved as the invalid value in public tokens.
      if (counter == 0) {
        counter = 1;
      }
      return counter++;
    }
  }  // namespace

  std::optional<std::string> manager::value_for(
    const override_map_t &map,
    const std::string &key) {
    const auto iterator = map.find(key);
    if (iterator == map.end()) {
      return std::nullopt;
    }
    return iterator->second;
  }

  std::unordered_map<std::string, manager::key_delta_t> manager::build_deltas(
    const override_map_t &prior_map,
    const override_map_t &candidate_map) {
    std::unordered_map<std::string, key_delta_t> deltas;
    deltas.reserve(prior_map.size() + candidate_map.size());

    for (const auto &[key, value] : prior_map) {
      deltas[key].prior = value;
    }
    for (const auto &[key, value] : candidate_map) {
      deltas[key].candidate = value;
    }

    for (auto iterator = deltas.begin(); iterator != deltas.end();) {
      if (iterator->second.prior == iterator->second.candidate) {
        iterator = deltas.erase(iterator);
      } else {
        ++iterator;
      }
    }
    return deltas;
  }

  override_map_t manager::conditionally_rollback(
    const override_map_t &current_map,
    const std::unordered_map<std::string, key_delta_t> &deltas) {
    auto replacement = current_map;
    for (const auto &[key, delta] : deltas) {
      if (value_for(replacement, key) != delta.candidate) {
        continue;
      }

      if (delta.prior.has_value()) {
        replacement[key] = *delta.prior;
      } else {
        replacement.erase(key);
      }
    }
    return replacement;
  }

  manager::owner_state_t manager::snapshot_owner(const cohort_t &cohort) {
    return owner_state_t {
      .generation = cohort.generation,
      .owner_token = cohort.owner_token,
      .map_revision = cohort.map_revision,
      .candidate_map = cohort.candidate_map,
      .deltas = cohort.deltas,
      .participants = cohort.participants,
      .phase = cohort.phase,
    };
  }

  manager::cohort_t manager::restore_owner(owner_state_t owner) {
    return cohort_t {
      .generation = owner.generation,
      .owner_token = owner.owner_token,
      .map_revision = owner.map_revision,
      .prior_map = owner.candidate_map,
      .candidate_map = std::move(owner.candidate_map),
      .deltas = std::move(owner.deltas),
      .participants = std::move(owner.participants),
      .phase = owner.phase,
      .prior_owner = std::nullopt,
    };
  }

  candidate_transaction_t manager::begin_candidate(const override_map_t &prior_map) {
    std::lock_guard lock(mutex_);

    const auto generation = next_id(next_generation_);
    std::optional<owner_state_t> prior_owner;
    if (cohort_.has_value() && cohort_->phase == phase_e::retained_paused) {
      prior_owner = snapshot_owner(*cohort_);
    }
    provisional_ = provisional_t {
      .transaction = candidate_transaction_t {.generation = generation},
      .prior_map = prior_map,
      .prior_owner = std::move(prior_owner),
    };
    // A new candidate supersedes the previous lease for publication purposes,
    // but retain a paused owner so a failed replacement can restore both its
    // map and its ownership metadata.
    cohort_.reset();
    return provisional_->transaction;
  }

  std::optional<lease_t> manager::apply_candidate(
    const candidate_transaction_t &transaction,
    const override_map_t &candidate_map) {
    std::lock_guard lock(mutex_);
    if (!provisional_.has_value() || provisional_->transaction.generation != transaction.generation) {
      return std::nullopt;
    }

    const auto participant = next_id(next_participant_);
    const auto map_revision = ++map_revision_;
    cohort_t cohort {
      .generation = transaction.generation,
      .owner_token = lease_token_t {
        .generation = transaction.generation,
        .participant = participant,
      },
      .map_revision = map_revision,
      .prior_map = provisional_->prior_map,
      .candidate_map = candidate_map,
      .deltas = build_deltas(provisional_->prior_map, candidate_map),
      .participants = {{participant, true}},
      .phase = phase_e::awaiting_stream,
      .prior_owner = std::move(provisional_->prior_owner),
    };
    cohort_ = std::move(cohort);
    provisional_.reset();

    return lease_t {
      .token = cohort_->owner_token,
      .map_revision = cohort_->map_revision,
      .candidate_map = cohort_->candidate_map,
    };
  }

  bool manager::update_candidate(
    const lease_token_t &token,
    const override_map_t &candidate_map) {
    std::lock_guard lock(mutex_);
    if (!cohort_.has_value() || cohort_->phase != phase_e::awaiting_stream ||
        cohort_->generation != token.generation) {
      return false;
    }

    const auto participant = cohort_->participants.find(token.participant);
    if (participant == cohort_->participants.end() || !participant->second) {
      return false;
    }

    cohort_->candidate_map = candidate_map;
    cohort_->deltas = build_deltas(cohort_->prior_map, candidate_map);
    cohort_->map_revision = ++map_revision_;
    return true;
  }

  std::optional<lease_token_t> manager::add_participant(const lease_token_t &token) {
    std::lock_guard lock(mutex_);
    if (!cohort_.has_value() || cohort_->phase != phase_e::awaiting_stream
        || cohort_->generation != token.generation) {
      return std::nullopt;
    }

    const auto participant = cohort_->participants.find(token.participant);
    if (participant == cohort_->participants.end() || !participant->second) {
      return std::nullopt;
    }

    const auto new_participant = next_id(next_participant_);
    cohort_->participants.emplace(new_participant, true);
    return lease_token_t {
      .generation = cohort_->generation,
      .participant = new_participant,
    };
  }

  commit_status_e manager::commit(const lease_token_t &token) {
    std::lock_guard lock(mutex_);
    if (!cohort_.has_value() || cohort_->generation != token.generation) {
      return commit_status_e::stale;
    }

    const auto participant = cohort_->participants.find(token.participant);
    if (participant == cohort_->participants.end()) {
      return commit_status_e::stale;
    }
    if (cohort_->phase == phase_e::active) {
      return commit_status_e::already_committed;
    }
    if (!participant->second) {
      return commit_status_e::stale;
    }

    cohort_->phase = phase_e::active;
    for (auto &entry : cohort_->participants) {
      entry.second = false;
    }
    return commit_status_e::committed;
  }

  void manager::retain_paused() {
    std::lock_guard lock(mutex_);
    if (cohort_.has_value() && cohort_->phase == phase_e::active) {
      cohort_->phase = phase_e::retained_paused;
    }
  }

  void manager::terminate() {
    std::lock_guard lock(mutex_);
    provisional_.reset();
    cohort_.reset();
    ++map_revision_;
  }

  cancellation_result_t manager::cancel(
    const lease_token_t &token,
    const override_map_t &current_map) {
    std::lock_guard lock(mutex_);
    cancellation_result_t result {
      .status = cancellation_status_e::stale,
      .map_revision = map_revision_,
      .replacement_map = std::nullopt,
    };
    if (!cohort_.has_value() || cohort_->generation != token.generation) {
      return result;
    }

    const auto participant = cohort_->participants.find(token.participant);
    if (participant == cohort_->participants.end()) {
      return result;
    }
    if (cohort_->phase == phase_e::active) {
      result.status = cancellation_status_e::committed;
      result.map_revision = cohort_->map_revision;
      return result;
    }
    if (!participant->second) {
      return result;
    }

    participant->second = false;
    const auto pending = std::count_if(
      cohort_->participants.cbegin(),
      cohort_->participants.cend(),
      [](const auto &entry) { return entry.second; });
    if (pending != 0) {
      result.status = cancellation_status_e::deferred;
      result.map_revision = cohort_->map_revision;
      return result;
    }

    result.status = cancellation_status_e::rolled_back;
    result.map_revision = ++map_revision_;
    result.replacement_map = conditionally_rollback(current_map, cohort_->deltas);
    if (cohort_->prior_owner.has_value()) {
      // The conditional rollback may preserve an unrelated value changed by
      // another manager transaction. The paused owner remains the source of
      // the HDR target, so restore its metadata around the actual resulting
      // map rather than requiring byte-for-byte equality with its old map.
      auto restored_owner = restore_owner(std::move(*cohort_->prior_owner));
      restored_owner.candidate_map = *result.replacement_map;
      cohort_ = std::move(restored_owner);
      cohort_->map_revision = result.map_revision;
    } else {
      cohort_.reset();
    }
    return result;
  }

  std::optional<state_t> manager::state() const {
    std::lock_guard lock(mutex_);
    if (provisional_.has_value()) {
      return state_t {
        .phase = phase_e::provisional,
        .generation = provisional_->transaction.generation,
        .map_revision = map_revision_,
        .pending_participants = 0,
        .owner_token = std::nullopt,
        .resolved_overrides = provisional_->prior_map,
        .prior_owner_overrides = provisional_->prior_owner
          ? std::optional<override_map_t>(provisional_->prior_owner->candidate_map)
          : std::nullopt,
        .inherited_from_prior_owner = provisional_->prior_owner.has_value(),
      };
    }
    if (!cohort_.has_value()) {
      return std::nullopt;
    }

    const auto pending = std::count_if(
      cohort_->participants.cbegin(),
      cohort_->participants.cend(),
      [](const auto &entry) { return entry.second; });
    return state_t {
      .phase = cohort_->phase,
      .generation = cohort_->generation,
      .map_revision = cohort_->map_revision,
      .pending_participants = static_cast<std::size_t>(pending),
      .owner_token = cohort_->owner_token,
      .resolved_overrides = cohort_->candidate_map,
      .prior_owner_overrides = cohort_->prior_owner
        ? std::optional<override_map_t>(cohort_->prior_owner->candidate_map)
        : std::nullopt,
      .inherited_from_prior_owner = cohort_->prior_owner.has_value(),
    };
  }

  void manager::invalidate_external() {
    std::lock_guard lock(mutex_);
    provisional_.reset();
    cohort_.reset();
    ++map_revision_;
  }

  bool manager::accepts_candidate(const lease_token_t &token) const {
    std::lock_guard lock(mutex_);
    if (!cohort_.has_value() || cohort_->phase != phase_e::awaiting_stream ||
        cohort_->generation != token.generation) {
      return false;
    }

    const auto participant = cohort_->participants.find(token.participant);
    return participant != cohort_->participants.end() && participant->second;
  }

  std::optional<override_map_t> manager::current_owner_overrides() const {
    std::lock_guard lock(mutex_);
    if (!cohort_.has_value() ||
        (cohort_->phase != phase_e::active &&
         cohort_->phase != phase_e::retained_paused)) {
      return std::nullopt;
    }
    return cohort_->candidate_map;
  }

  manager &global_manager() {
    static manager instance;
    return instance;
  }

}  // namespace hdr_runtime_owner
