/**
 * @file tests/unit/test_hdr_runtime_owner.cpp
 * @brief Conditional runtime override ownership and rollback contracts.
 */
#include "../tests_common.h"

#include <src/hdr_runtime_owner.h>

#include <optional>
#include <initializer_list>
#include <string>
#include <utility>

namespace {
  using hdr_runtime_owner::cancellation_status_e;
  using hdr_runtime_owner::commit_status_e;
  using hdr_runtime_owner::manager;
  using hdr_runtime_owner::override_map_t;

  override_map_t map(std::initializer_list<std::pair<std::string, std::string>> entries) {
    override_map_t result;
    for (const auto &entry : entries) {
      result.emplace(entry);
    }
    return result;
  }

  TEST(HdrRuntimeOwner, EmptyPriorMapRollsBackHdrAndUnrelatedCandidateKeys) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
      {"unrelated_runtime_key", "candidate"},
    }));

    ASSERT_TRUE(lease.has_value());
    const auto result = owner.cancel(lease->token, lease->candidate_map);

    EXPECT_EQ(result.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(result.replacement_map.has_value());
    EXPECT_TRUE(result.replacement_map->empty());
  }

  TEST(HdrRuntimeOwner, NonemptyPriorMapRestoresEveryChangedKey) {
    manager owner;
    const auto prior = map({
      {"rtx_hdr_peak_brightness", "600"},
      {"unrelated_runtime_key", "prior"},
      {"preserved_runtime_key", "same"},
      {"removed_by_candidate", "restore-me"},
    });
    const auto transaction = owner.begin_candidate(prior);
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1400"},
      {"unrelated_runtime_key", "candidate"},
      {"preserved_runtime_key", "same"},
      {"candidate_only_key", "new"},
    }));

    ASSERT_TRUE(lease.has_value());
    const auto result = owner.cancel(lease->token, lease->candidate_map);

    EXPECT_EQ(result.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(result.replacement_map.has_value());
    EXPECT_EQ(*result.replacement_map, prior);
  }

  TEST(HdrRuntimeOwner, UnrelatedManagerEditSurvivesRollback) {
    manager owner;
    const auto prior = map({
      {"rtx_hdr_peak_brightness", "600"},
      {"unrelated_runtime_key", "prior"},
    });
    const auto transaction = owner.begin_candidate(prior);
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1400"},
      {"unrelated_runtime_key", "candidate"},
    }));

    ASSERT_TRUE(lease.has_value());
    auto current = lease->candidate_map;
    current["unrelated_runtime_key"] = "manager-edit";
    const auto result = owner.cancel(lease->token, current);

    EXPECT_EQ(result.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(result.replacement_map.has_value());
    EXPECT_EQ(result.replacement_map->at("rtx_hdr_peak_brightness"), "600");
    EXPECT_EQ(result.replacement_map->at("unrelated_runtime_key"), "manager-edit");
  }

  TEST(HdrRuntimeOwner, RemovedCandidateOwnedKeyIsNotRecreated) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));

    ASSERT_TRUE(lease.has_value());
    auto current = lease->candidate_map;
    current.erase("rtx_hdr_peak_brightness");
    const auto result = owner.cancel(lease->token, current);

    EXPECT_EQ(result.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(result.replacement_map.has_value());
    EXPECT_TRUE(result.replacement_map->empty());
  }

  TEST(HdrRuntimeOwner, CommitPreventsRollback) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));

    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(owner.commit(lease->token), commit_status_e::committed);

    const auto result = owner.cancel(lease->token, lease->candidate_map);

    EXPECT_EQ(result.status, cancellation_status_e::committed);
    EXPECT_FALSE(result.replacement_map.has_value());
  }

  TEST(HdrRuntimeOwner, AwaitingCandidateCanBeUpdatedBeforeCommit) {
    manager owner;
    const auto transaction = owner.begin_candidate(map({
      {"rtx_hdr_peak_brightness", "600"},
    }));
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));
    ASSERT_TRUE(lease.has_value());

    EXPECT_TRUE(owner.update_candidate(lease->token, map({
      {"rtx_hdr_peak_brightness", "1400"},
      {"new_runtime_key", "candidate"},
    })));

    const auto state = owner.state();
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->resolved_overrides.at("rtx_hdr_peak_brightness"), "1400");
    EXPECT_EQ(state->resolved_overrides.at("new_runtime_key"), "candidate");

    const auto rollback = owner.cancel(lease->token, state->resolved_overrides);
    ASSERT_EQ(rollback.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(rollback.replacement_map.has_value());
    EXPECT_EQ(*rollback.replacement_map, map({
      {"rtx_hdr_peak_brightness", "600"},
    }));
  }

  TEST(HdrRuntimeOwner, ActiveOwnerBecomesRetainedPausedWithoutRollback) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));

    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(owner.commit(lease->token), commit_status_e::committed);
    owner.retain_paused();

    const auto state = owner.state();
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->phase, hdr_runtime_owner::phase_e::retained_paused);
    EXPECT_FALSE(owner.accepts_candidate(lease->token));

    const auto result = owner.cancel(lease->token, lease->candidate_map);
    EXPECT_EQ(result.status, cancellation_status_e::stale);
    EXPECT_FALSE(result.replacement_map.has_value());
  }

  TEST(HdrRuntimeOwner, TerminationClearsRetainedOwnerMetadata) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));

    ASSERT_TRUE(lease.has_value());
    EXPECT_EQ(owner.commit(lease->token), commit_status_e::committed);
    owner.retain_paused();
    owner.terminate();

    EXPECT_FALSE(owner.state().has_value());
    EXPECT_FALSE(owner.accepts_candidate(lease->token));
  }

  TEST(HdrRuntimeOwner, FailedReplacementRestoresRetainedOwnerAndMap) {
    manager owner;
    const auto first_transaction = owner.begin_candidate(override_map_t {});
    const auto first_lease = owner.apply_candidate(first_transaction, map({
      {"rtx_hdr_peak_brightness", "600"},
    }));
    ASSERT_TRUE(first_lease.has_value());
    EXPECT_EQ(owner.commit(first_lease->token), commit_status_e::committed);
    owner.retain_paused();

    const auto replacement_transaction = owner.begin_candidate(first_lease->candidate_map);
    const auto replacement = owner.apply_candidate(replacement_transaction, map({
      {"rtx_hdr_peak_brightness", "1400"},
    }));
    ASSERT_TRUE(replacement.has_value());

    const auto rollback = owner.cancel(replacement->token, replacement->candidate_map);
    ASSERT_EQ(rollback.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(rollback.replacement_map.has_value());
    EXPECT_EQ(rollback.replacement_map->at("rtx_hdr_peak_brightness"), "600");
    const auto restored = owner.state();
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->phase, hdr_runtime_owner::phase_e::retained_paused);
    ASSERT_TRUE(owner.current_owner_overrides().has_value());
    EXPECT_EQ(owner.current_owner_overrides()->at("rtx_hdr_peak_brightness"), "600");
  }

  TEST(HdrRuntimeOwner, MultipleParticipantsDeferRollbackUntilFinalCancellation) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));
    ASSERT_TRUE(lease.has_value());
    const auto participant = owner.add_participant(lease->token);
    ASSERT_TRUE(participant.has_value());

    const auto first = owner.cancel(lease->token, lease->candidate_map);
    EXPECT_EQ(first.status, cancellation_status_e::deferred);
    EXPECT_FALSE(first.replacement_map.has_value());

    const auto final = owner.cancel(*participant, lease->candidate_map);
    EXPECT_EQ(final.status, cancellation_status_e::rolled_back);
    ASSERT_TRUE(final.replacement_map.has_value());
    EXPECT_TRUE(final.replacement_map->empty());
  }

  TEST(HdrRuntimeOwner, CohortCommitPreventsEveryParticipantCancellationRollback) {
    manager owner;
    const auto transaction = owner.begin_candidate(override_map_t {});
    const auto lease = owner.apply_candidate(transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));
    ASSERT_TRUE(lease.has_value());
    const auto participant = owner.add_participant(lease->token);
    ASSERT_TRUE(participant.has_value());

    EXPECT_EQ(owner.commit(*participant), commit_status_e::committed);

    const auto first = owner.cancel(lease->token, lease->candidate_map);
    const auto second = owner.cancel(*participant, lease->candidate_map);
    EXPECT_EQ(first.status, cancellation_status_e::committed);
    EXPECT_EQ(second.status, cancellation_status_e::committed);
    EXPECT_FALSE(first.replacement_map.has_value());
    EXPECT_FALSE(second.replacement_map.has_value());
  }

  TEST(HdrRuntimeOwner, DuplicateAndStaleTokenOperationsAreHarmless) {
    manager owner;
    const auto first_transaction = owner.begin_candidate(override_map_t {});
    const auto first_lease = owner.apply_candidate(first_transaction, map({
      {"rtx_hdr_peak_brightness", "1000"},
    }));
    ASSERT_TRUE(first_lease.has_value());

    const auto first_cancel = owner.cancel(first_lease->token, first_lease->candidate_map);
    ASSERT_EQ(first_cancel.status, cancellation_status_e::rolled_back);
    const auto duplicate_cancel = owner.cancel(first_lease->token, first_lease->candidate_map);
    EXPECT_EQ(duplicate_cancel.status, cancellation_status_e::stale);
    EXPECT_FALSE(duplicate_cancel.replacement_map.has_value());
    EXPECT_EQ(owner.commit(first_lease->token), commit_status_e::stale);
    EXPECT_FALSE(owner.add_participant(first_lease->token).has_value());

    const auto second_transaction = owner.begin_candidate(override_map_t {});
    const auto second_lease = owner.apply_candidate(second_transaction, map({
      {"rtx_hdr_peak_brightness", "1200"},
    }));
    ASSERT_TRUE(second_lease.has_value());
    const auto stale_generation = owner.cancel(first_lease->token, second_lease->candidate_map);
    EXPECT_EQ(stale_generation.status, cancellation_status_e::stale);
    EXPECT_FALSE(stale_generation.replacement_map.has_value());
  }
}  // namespace
