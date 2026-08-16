/**
 * @file tests/unit/test_audio.cpp
 * @brief Deterministic tests for audio stream, sink, and capture policy.
 */
#include "../tests_common.h"

#include <src/audio_policy.h>

#include <atomic>
#include <deque>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace audio::policy;

TEST(AudioStreamPolicy, SelectsChannelAndQualityVariant) {
  EXPECT_EQ(stream_index(2, false), 0);
  EXPECT_EQ(stream_index(2, true), 1);
  EXPECT_EQ(stream_index(6, false), 2);
  EXPECT_EQ(stream_index(6, true), 3);
  EXPECT_EQ(stream_index(8, false), 4);
  EXPECT_EQ(stream_index(8, true), 5);
  EXPECT_EQ(stream_index(3, true), 0);
}

TEST(AudioStreamPolicy, AppliesCustomSurroundLayoutWithoutMutatingDefault) {
  const stream_layout_t base {6, 4, 2, {0, 1, 4, 5, 2, 3, 0, 0}};
  const stream_layout_t custom {6, 6, 0, {0, 1, 4, 5, 2, 3, 0, 0}};

  EXPECT_EQ(apply_custom_layout(base, std::nullopt).streams, 4);
  const auto selected = apply_custom_layout(base, custom);
  EXPECT_EQ(selected.streams, 6);
  EXPECT_EQ(selected.coupled_streams, 0);
  EXPECT_EQ(base.streams, 4);
}

TEST(AudioSinkPolicy, PreservesPriorityAndEmptyFallbacks) {
  const sink_catalog_t sinks {
    "host",
    "virtual-stereo",
    "virtual-51",
    "virtual-71"
  };

  EXPECT_EQ(select_sink(sinks, "configured", 2, true), "configured");
  EXPECT_EQ(select_sink(sinks, "configured", 6, false), "virtual-51");
  EXPECT_EQ(select_sink(sinks, "", 8, true), "host");

  const sink_catalog_t no_virtual {"", std::nullopt, std::nullopt, std::nullopt};
  EXPECT_TRUE(select_sink(no_virtual, "", 2, false).empty());
}

TEST(AudioEndpointRestorePolicy, ExcludesEverySteamRenderAdapterFromEligibleNonSteamSelection) {
  const std::vector<render_endpoint_t> endpoints {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
    {"hdmi", "NVIDIA High Definition Audio", true},
  };

  EXPECT_TRUE(is_steam_streaming_render_adapter("Steam Streaming Speakers"));
  EXPECT_TRUE(is_steam_streaming_render_adapter("Steam Streaming Microphone"));
  EXPECT_FALSE(is_steam_streaming_render_adapter("Steam Streaming Speakers 2"));
  EXPECT_FALSE(is_steam_streaming_render_adapter("Realtek USB Audio"));

  EXPECT_EQ(
    select_eligible_non_steam_render_endpoint(endpoints, {"steam-microphone", "realtek"}),
    std::optional<std::string> {"realtek"}
  );
}

TEST(AudioEndpointRestorePolicy, PrefersActiveConfiguredNonSteamEndpointThenSafeFallback) {
  const std::vector<render_endpoint_t> endpoints {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
    {"inactive-configured", "USB Audio", false},
    {"hdmi", "NVIDIA High Definition Audio", true},
  };

  EXPECT_EQ(
    select_eligible_non_steam_render_endpoint(endpoints, {"realtek", "hdmi"}),
    std::optional<std::string> {"realtek"}
  );
  EXPECT_EQ(
    select_eligible_non_steam_render_endpoint(endpoints, {"inactive-configured", "hdmi"}),
    std::optional<std::string> {"hdmi"}
  );

  const std::vector<render_endpoint_t> only_steam {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"unknown-adapter", "", true},
  };
  EXPECT_EQ(select_eligible_non_steam_render_endpoint(only_steam, {}), std::nullopt);
}

TEST(AudioEndpointRestorePolicy, RepairsSplitSteamRolesWithoutClaimingUserChangedRole) {
  const auto owned_roles = steam_owned_role_mask(
    {"steam-speakers", "steam-microphone", "user-headset"},
    {"steam-speakers", "steam-microphone"}
  );

  EXPECT_EQ(owned_roles, (std::vector<bool> {true, true, false}));
}

TEST(AudioEndpointRestorePolicy, BindsQueuedRoleToOneSnapshotAndRejectsLaterNonSteamDefault) {
  const std::vector<std::string> steam_ids {"steam-speakers", "steam-microphone"};
  const auto initial = steam_owned_roles_from_snapshot(
    {"steam-speakers", "user-headset", "steam-microphone"},
    steam_ids
  );

  ASSERT_EQ(initial.size(), 2u);
  EXPECT_EQ(initial[0].role_index, 0u);
  EXPECT_EQ(initial[0].expected_current_id, "steam-speakers");
  EXPECT_EQ(initial[1].role_index, 2u);
  EXPECT_EQ(initial[1].expected_current_id, "steam-microphone");
  EXPECT_FALSE(role_restore_still_owned(initial[0], "realtek"));
  EXPECT_FALSE(role_restore_still_owned(initial[0], "steam-microphone"));

  const auto after_user_change = steam_owned_roles_from_snapshot(
    {"realtek", "user-headset", "steam-microphone"},
    steam_ids
  );
  ASSERT_EQ(after_user_change.size(), 1u);
  EXPECT_EQ(after_user_change[0].role_index, 2u);
}

TEST(AudioEndpointRestorePolicy, SanitizesSteamRoleCaptureWithNonSteamFallbackOrFailsClosed) {
  const std::vector<std::string> steam_ids {"steam-speakers", "steam-microphone"};
  const auto sanitized = sanitize_captured_role_ids(
    {"steam-speakers", "steam-microphone", "user-headset"},
    steam_ids,
    "hdmi"
  );
  ASSERT_TRUE(sanitized);
  EXPECT_EQ(*sanitized, (std::vector<std::string> {"hdmi", "hdmi", "user-headset"}));

  EXPECT_EQ(
    sanitize_captured_role_ids({"steam-speakers", "steam-microphone", ""}, steam_ids, ""),
    std::nullopt
  );

  const auto non_steam_console = sanitize_captured_role_ids(
    {"realtek", "steam-microphone", "user-headset"},
    steam_ids,
    ""
  );
  ASSERT_TRUE(non_steam_console);
  EXPECT_EQ(*non_steam_console, (std::vector<std::string> {"realtek", "", "user-headset"}));
}

TEST(AudioEndpointRestorePolicy, IncompleteCatalogBlocksSelectionAndExactFallbackEligibility) {
  const std::vector<render_endpoint_t> endpoints {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
  };
  const auto incomplete = build_render_endpoint_catalog(false, endpoints);
  EXPECT_FALSE(incomplete.complete);
  EXPECT_EQ(select_eligible_non_steam_render_endpoint(incomplete, {"realtek"}), std::nullopt);
  EXPECT_FALSE(is_eligible_non_steam_fallback(incomplete, "realtek"));

  const auto complete = build_render_endpoint_catalog(true, endpoints);
  EXPECT_TRUE(is_eligible_non_steam_fallback(complete, "realtek"));
  EXPECT_FALSE(is_eligible_non_steam_fallback(complete, "steam-speakers"));
  EXPECT_FALSE(is_eligible_non_steam_fallback(complete, "steam-microphone"));
  EXPECT_FALSE(is_eligible_non_steam_fallback(complete, "unknown"));

  const auto steam_only = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
  });
  EXPECT_TRUE(steam_only.complete);
  EXPECT_EQ(select_eligible_non_steam_render_endpoint(steam_only, {}), std::nullopt);
}

TEST(AudioEndpointRestorePolicy, InstallRecoveryPlansBothSteamRenderRolesOnly) {
  const auto catalog = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
    {"communications-headset", "USB Headset", true},
  });
  const auto restores = plan_steam_role_restores(
    catalog,
    {"steam-speakers", "steam-microphone", "user-headset"},
    {"realtek", "realtek", "communications-headset"}
  );

  ASSERT_EQ(restores.size(), 2u);
  EXPECT_EQ(restores[0].role_index, 0u);
  EXPECT_EQ(restores[0].expected_current_id, "steam-speakers");
  EXPECT_EQ(restores[0].target_id, "realtek");
  EXPECT_EQ(restores[1].role_index, 1u);
  EXPECT_EQ(restores[1].expected_current_id, "steam-microphone");
  EXPECT_EQ(restores[1].target_id, "realtek");

  const auto reenumerated_catalog = build_render_endpoint_catalog(true, {
    {"steam-speakers-new", "Steam Streaming Speakers", true},
    {"steam-microphone-new", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
  });
  const auto missing_old_steam_microphone = plan_steam_role_restores(
    reenumerated_catalog,
    {"steam-speakers-new", "steam-microphone-new", "user-headset"},
    {"old-steam-microphone", "old-steam-microphone", "missing-physical"}
  );
  EXPECT_TRUE(missing_old_steam_microphone.empty());

  const auto incomplete_catalog = build_render_endpoint_catalog(false, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
  });
  EXPECT_TRUE(plan_steam_role_restores(
    incomplete_catalog,
    {"steam-speakers"},
    {"realtek"}
  ).empty());
}

TEST(AudioEndpointRestorePolicy, UserNonSteamChoiceBeforeFallbackWriteRetiresPreferredRestore) {
  const auto catalog = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
  });

  const auto transition = plan_fallback_role_transition(
    catalog,
    "steam-speakers",
    "realtek",
    "realtek"
  );
  EXPECT_EQ(transition.action, fallback_role_action_e::adopt_and_retire);
  EXPECT_TRUE(transition.next_expected_id.empty());
  EXPECT_FALSE(transition.keep_restore);
}

TEST(AudioEndpointRestorePolicy, DifferentSteamEndpointNeverRebindsFallbackOwnership) {
  const auto catalog = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
    {"realtek", "Realtek USB Audio", true},
  });

  const auto changed_endpoint = plan_fallback_role_transition(
    catalog,
    "steam-speakers",
    "steam-microphone",
    "realtek"
  );
  EXPECT_EQ(changed_endpoint.action, fallback_role_action_e::adopt_and_retire);
  EXPECT_TRUE(changed_endpoint.next_expected_id.empty());
  EXPECT_FALSE(changed_endpoint.keep_restore);

  const auto exact_guard = plan_fallback_role_transition(
    catalog,
    "steam-speakers",
    "steam-speakers",
    "realtek"
  );
  EXPECT_EQ(exact_guard.action, fallback_role_action_e::assign_selected_fallback);
  EXPECT_EQ(exact_guard.next_expected_id, "steam-speakers");
  EXPECT_TRUE(exact_guard.keep_restore);
}

TEST(AudioEndpointRestorePolicy, FallbackOwnershipCommitsOnlyAfterSuccessfulMatchingReadback) {
  const auto committed = complete_fallback_role_transition(
    "steam-speakers",
    "realtek",
    true,
    std::optional<std::string> {"realtek"}
  );
  EXPECT_EQ(committed.action, fallback_commit_action_e::commit_fallback);
  EXPECT_EQ(committed.next_expected_id, "realtek");
  EXPECT_TRUE(committed.keep_restore);

  const auto delayed_policy_propagation = complete_fallback_role_transition(
    "steam-speakers",
    "realtek",
    true,
    std::optional<std::string> {"steam-speakers"}
  );
  EXPECT_EQ(delayed_policy_propagation.action, fallback_commit_action_e::retry_committed);
  EXPECT_EQ(delayed_policy_propagation.next_expected_id, "steam-speakers");
  EXPECT_TRUE(delayed_policy_propagation.keep_restore);

  const auto raced_user_choice = complete_fallback_role_transition(
    "steam-speakers",
    "realtek",
    true,
    std::optional<std::string> {"hdmi"}
  );
  EXPECT_EQ(raced_user_choice.action, fallback_commit_action_e::release_external);
  EXPECT_TRUE(raced_user_choice.next_expected_id.empty());
  EXPECT_FALSE(raced_user_choice.keep_restore);

  const auto uncertain = complete_fallback_role_transition(
    "steam-speakers",
    "realtek",
    true,
    std::nullopt
  );
  EXPECT_EQ(uncertain.action, fallback_commit_action_e::retry_committed);
  EXPECT_EQ(uncertain.next_expected_id, "steam-speakers");
  EXPECT_TRUE(uncertain.keep_restore);
}

TEST(AudioEndpointRestorePolicy, SupersededWriteRepairsOnlyItsExactSideEffectForTheActiveEpoch) {
  EXPECT_EQ(
    superseded_assignment_repair_target("steam-speakers", "steam-speakers", "realtek"),
    std::optional<std::string> {"realtek"}
  );
  EXPECT_EQ(
    superseded_assignment_repair_target("steam-speakers", "user-headset", "realtek"),
    std::nullopt
  );
  EXPECT_EQ(
    superseded_assignment_repair_target("steam-speakers", "realtek", "realtek"),
    std::nullopt
  );
  EXPECT_EQ(
    superseded_assignment_repair_target("steam-speakers", "steam-speakers", ""),
    std::nullopt
  );
}

TEST(AudioEndpointRestorePolicy, SupersededWriteKeepsWatchingOldOrActiveUntilStaleTargetAppears) {
  const std::vector<std::string> stale_targets {"stale-a"};
  const std::vector<std::string> tolerated_ids {"old", "active-b"};

  EXPECT_EQ(
    observe_superseded_write(
      stale_targets,
      tolerated_ids,
      "old",
      "active-b",
      true,
      false
    ).action,
    superseded_write_observation_action_e::keep_observing
  );
  EXPECT_EQ(
    observe_superseded_write(
      stale_targets,
      tolerated_ids,
      "active-b",
      "active-b",
      true,
      false
    ).action,
    superseded_write_observation_action_e::keep_observing
  );
  const auto delayed_stale = observe_superseded_write(
    stale_targets,
    tolerated_ids,
    "stale-a",
    "active-b",
    true,
    false
  );
  EXPECT_EQ(delayed_stale.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(delayed_stale.target_id, "active-b");
}

TEST(AudioEndpointRestorePolicy, FailedSupersededCallStillRepairsItsExactDelayedSideEffect) {
  const auto delayed_side_effect = observe_superseded_write(
    {"failed-write-a"},
    {"old", "active-b"},
    "failed-write-a",
    "active-b",
    true,
    false
  );
  EXPECT_EQ(delayed_side_effect.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(delayed_side_effect.target_id, "active-b");
}

TEST(AudioEndpointRestorePolicy, SupersededWritePreservesExternalThirdIdAcrossLaterPropagation) {
  const auto external = observe_superseded_write(
    {"stale-a"},
    {"old", "active-b"},
    "user-c",
    "active-b",
    true,
    false
  );
  EXPECT_EQ(external.action, superseded_write_observation_action_e::adopt_external);
  EXPECT_EQ(external.target_id, "user-c");

  const auto delayed_stale = observe_superseded_write(
    {"stale-a"},
    {"old", "active-b", "user-c"},
    "stale-a",
    external.target_id,
    true,
    false
  );
  EXPECT_EQ(delayed_stale.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(delayed_stale.target_id, "user-c");
}

TEST(AudioEndpointRestorePolicy, ExternalAdoptionRetiresHistoricalToleratedIdsByPhase) {
  const auto after_user_c = advance_superseded_write_ledger_after_external_adoption(
    {"stale-a"},
    "active-b",
    "user-c"
  );
  EXPECT_EQ(
    after_user_c.stale_target_ids,
    (std::vector<std::string> {"stale-a", "active-b"})
  );
  EXPECT_EQ(after_user_c.tolerated_live_ids, (std::vector<std::string> {"user-c"}));

  const auto user_reselected_old = observe_superseded_write(
    after_user_c.stale_target_ids,
    after_user_c.tolerated_live_ids,
    "old-o",
    "user-c",
    true,
    false
  );
  ASSERT_EQ(
    user_reselected_old.action,
    superseded_write_observation_action_e::adopt_external
  );
  EXPECT_EQ(user_reselected_old.target_id, "old-o");

  const auto after_user_old = advance_superseded_write_ledger_after_external_adoption(
    after_user_c.stale_target_ids,
    "user-c",
    user_reselected_old.target_id
  );
  const auto delayed_stale = observe_superseded_write(
    after_user_old.stale_target_ids,
    after_user_old.tolerated_live_ids,
    "stale-a",
    user_reselected_old.target_id,
    true,
    false
  );
  EXPECT_EQ(delayed_stale.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(delayed_stale.target_id, "old-o");
}

TEST(AudioEndpointRestorePolicy, SupersededWriteObservationIsRoleIndependent) {
  const auto console = observe_superseded_write(
    {"console-a"},
    {"console-b"},
    "console-a",
    "console-b",
    true,
    false
  );
  const auto communications = observe_superseded_write(
    {"communications-a"},
    {"communications-b"},
    "communications-b",
    "communications-b",
    true,
    false
  );
  EXPECT_EQ(console.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(console.target_id, "console-b");
  EXPECT_EQ(communications.action, superseded_write_observation_action_e::keep_observing);
  EXPECT_TRUE(communications.target_id.empty());
}

TEST(AudioEndpointRestorePolicy, SupersededWriteObservationCancelsWithoutEntitlement) {
  EXPECT_EQ(
    observe_superseded_write({"stale-a"}, {"active-b"}, "stale-a", "active-b", true, true).action,
    superseded_write_observation_action_e::cancel
  );
  EXPECT_EQ(
    observe_superseded_write({"stale-a"}, {"active-b"}, "stale-a", "active-b", false, false).action,
    superseded_write_observation_action_e::cancel
  );
}

TEST(AudioEndpointRestorePolicy, FailedCurrentWriteKeepsWatchingThroughExternalChoiceAndLateSideEffect) {
  EXPECT_TRUE(policy_write_requires_settling(false, true));
  EXPECT_TRUE(policy_write_requires_settling(false, false));
  EXPECT_TRUE(policy_write_requires_settling(true, false));
  EXPECT_FALSE(policy_write_requires_settling(true, true));

  const auto external = observe_superseded_write(
    {"failed-a"},
    {"old-b"},
    "user-u",
    "old-b",
    true,
    false
  );
  ASSERT_EQ(external.action, superseded_write_observation_action_e::adopt_external);
  EXPECT_EQ(external.target_id, "user-u");

  const auto late_failed_side_effect = observe_superseded_write(
    {"failed-a"},
    {"old-b", "user-u"},
    "failed-a",
    external.target_id,
    true,
    false
  );
  EXPECT_EQ(
    late_failed_side_effect.action,
    superseded_write_observation_action_e::repair_active
  );
  EXPECT_EQ(late_failed_side_effect.target_id, "user-u");
}

TEST(AudioEndpointRestorePolicy, SuccessfulUnconfirmedActivationRepairsOnlyItsLateOwnedTarget) {
  EXPECT_TRUE(activation_policy_write_requires_settling(true, true, false));
  EXPECT_FALSE(activation_policy_write_requires_settling(true, true, true));
  EXPECT_TRUE(activation_policy_write_requires_settling(false, true, false));
  EXPECT_TRUE(activation_policy_write_requires_settling(true, false, false));

  const auto immediate_old = observe_superseded_write(
    {"steam-audio"},
    {"old-speakers", "steam-audio"},
    "old-speakers",
    "steam-audio",
    true,
    false
  );
  EXPECT_EQ(
    immediate_old.action,
    superseded_write_observation_action_e::keep_observing
  );

  const auto after_teardown = observe_superseded_write(
    {"steam-audio"},
    {"old-speakers", "steam-audio"},
    "old-speakers",
    "old-speakers",
    true,
    false
  );
  EXPECT_EQ(
    after_teardown.action,
    superseded_write_observation_action_e::keep_observing
  );

  const auto late_steam = observe_superseded_write(
    {"steam-audio"},
    {"old-speakers", "steam-audio"},
    "steam-audio",
    "old-speakers",
    true,
    false
  );
  EXPECT_EQ(late_steam.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(late_steam.target_id, "old-speakers");
}

TEST(AudioEndpointRestorePolicy, ActivationReceiptPreservesUserChoiceAndSiblingRole) {
  const auto external = observe_superseded_write(
    {"steam-console"},
    {"old-console", "steam-console"},
    "user-headset",
    "old-console",
    true,
    false
  );
  ASSERT_EQ(external.action, superseded_write_observation_action_e::adopt_external);
  EXPECT_EQ(external.target_id, "user-headset");

  const auto phase = advance_superseded_write_ledger_after_external_adoption(
    {"steam-console"},
    "old-console",
    external.target_id
  );
  const auto late_steam = observe_superseded_write(
    phase.stale_target_ids,
    phase.tolerated_live_ids,
    "steam-console",
    external.target_id,
    true,
    false
  );
  EXPECT_EQ(late_steam.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(late_steam.target_id, "user-headset");

  const auto sibling = observe_superseded_write(
    {"steam-communications"},
    {"old-communications", "steam-communications"},
    "old-communications",
    "old-communications",
    true,
    false
  );
  EXPECT_EQ(sibling.action, superseded_write_observation_action_e::keep_observing);
}

TEST(AudioEndpointRestorePolicy, PendingCurrentEpochTargetIsToleratedUntilExactCommit) {
  EXPECT_TRUE(policy_write_receipt_is_pending(
    7,
    7,
    11,
    11,
    false,
    "steam-expected",
    "steam-expected"
  ));
  EXPECT_FALSE(policy_write_receipt_is_pending(
    7,
    7,
    11,
    11,
    false,
    "steam-expected",
    "user-headset"
  ));
  EXPECT_FALSE(policy_write_receipt_is_pending(
    7,
    7,
    11,
    11,
    true,
    "steam-expected",
    "steam-expected"
  ));
  EXPECT_FALSE(policy_write_receipt_is_pending(
    6,
    7,
    11,
    11,
    false,
    "steam-expected",
    "steam-expected"
  ));
  EXPECT_FALSE(policy_write_receipt_is_pending(
    7,
    7,
    10,
    11,
    false,
    "steam-expected",
    "steam-expected"
  ));

  const auto pending_realtek = observe_superseded_write(
    {"older-steam-write"},
    {"steam-expected", "realtek-pending"},
    "realtek-pending",
    "steam-expected",
    true,
    false
  );
  EXPECT_EQ(
    pending_realtek.action,
    superseded_write_observation_action_e::keep_observing
  );
  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-expected",
      "realtek-pending",
      true,
      std::optional<std::string> {"realtek-pending"}
    ).action,
    fallback_commit_action_e::commit_fallback
  );
}

TEST(AudioEndpointRestorePolicy, QueuedGenerationReceivesAFreshObserverWindow) {
  const auto predecessor_at_deadline = advance_superseded_write_observer(
    true,
    true,
    true,
    true,
    true,
    49,
    50
  );
  EXPECT_EQ(
    predecessor_at_deadline.action,
    superseded_write_observer_progress_action_e::handoff_generation
  );
  EXPECT_EQ(predecessor_at_deadline.stable_observations, 0);

  const auto queued_generation_first_sample = advance_superseded_write_observer(
    false,
    false,
    true,
    true,
    true,
    0,
    50
  );
  EXPECT_EQ(
    queued_generation_first_sample.action,
    superseded_write_observer_progress_action_e::continue_observing
  );
  EXPECT_EQ(queued_generation_first_sample.stable_observations, 1);
}

TEST(AudioEndpointRestorePolicy, RepairGenerationHandoffsWithoutExtendingCausalDeadline) {
  const auto repair_registered_at_predecessor_deadline =
    advance_superseded_write_observer(
      true,
      true,
      true,
      true,
      true,
      49,
      50
    );
  EXPECT_EQ(
    repair_registered_at_predecessor_deadline.action,
    superseded_write_observer_progress_action_e::handoff_generation
  );
  EXPECT_EQ(repair_registered_at_predecessor_deadline.stable_observations, 0);
  EXPECT_EQ(
    causal_repair_deadline_after_generation(10'000, 20'000, true),
    10'000u
  );
}

TEST(AudioEndpointRestorePolicy, AssignmentHandoffRebindsObserverBeforeOldRetirement) {
  const policy_observer_owner_key_t old_owner {17, 41, 5, "steam-speakers"};
  const policy_observer_owner_key_t new_owner {17, 42, 6, "realtek"};
  const policy_observer_owner_key_t revised_owner {17, 42, 7, "realtek"};
  EXPECT_TRUE(policy_observer_owner_changed(old_owner, new_owner));
  EXPECT_TRUE(policy_observer_owner_changed(new_owner, revised_owner));
  EXPECT_FALSE(policy_observer_owner_changed(new_owner, new_owner));

  const auto paused_at_old_deadline = advance_superseded_write_observer(
    policy_observer_owner_changed(old_owner, new_owner),
    true,
    true,
    true,
    true,
    49,
    50
  );
  EXPECT_EQ(
    paused_at_old_deadline.action,
    superseded_write_observer_progress_action_e::handoff_generation
  );

  const auto late_steam = observe_superseded_write(
    {"steam-speakers"},
    {"realtek"},
    "steam-speakers",
    "realtek",
    true,
    false
  );
  EXPECT_EQ(late_steam.action, superseded_write_observation_action_e::repair_active);
  EXPECT_EQ(late_steam.target_id, "realtek");
}

TEST(AudioEndpointRestorePolicy, CausalRepairIsSingleOutstandingAndCannotExtendItsDeadline) {
  causal_repair_state_t repair {false, 0, 0, 10'000};
  auto first = plan_causal_repair(repair, 100, true, 3);
  ASSERT_EQ(first.action, causal_repair_action_e::issue);
  EXPECT_TRUE(first.state.outstanding);
  EXPECT_EQ(first.state.attempts, 1u);

  repair = complete_causal_repair(first.state, 110, true, false, 1'000);
  EXPECT_TRUE(repair.outstanding);
  for (std::uint64_t now = 111; now < repair.deadline_tick; now += 777) {
    const auto repeated = plan_causal_repair(repair, now, true, 3);
    EXPECT_EQ(repeated.action, causal_repair_action_e::wait);
    EXPECT_EQ(repeated.state.attempts, 1u);
    EXPECT_EQ(repeated.state.deadline_tick, 10'000u);
  }

  auto failed_once = complete_causal_repair(first.state, 110, false, false, 1'000);
  EXPECT_EQ(
    plan_causal_repair(failed_once, 1'110, true, 1).action,
    causal_repair_action_e::retire
  );

  EXPECT_EQ(
    causal_repair_deadline_after_generation(10'000, 20'000, true),
    10'000u
  );
  EXPECT_EQ(
    causal_repair_deadline_after_generation(10'000, 20'000, false),
    20'000u
  );
  const auto delayed_convergence = observe_superseded_write(
    {"steam-speakers"},
    {"realtek"},
    "realtek",
    "realtek",
    true,
    false
  );
  EXPECT_EQ(
    delayed_convergence.action,
    superseded_write_observation_action_e::keep_observing
  );
}

TEST(AudioEndpointRestorePolicy, FullReceiptLedgerRejectsWhenEveryEntryIsUnresolved) {
  const auto all_unresolved = decide_policy_receipt_capacity(
    64,
    64,
    std::vector<bool>(64, false)
  );
  EXPECT_EQ(
    all_unresolved.action,
    policy_receipt_capacity_action_e::reject_unresolved
  );
  EXPECT_EQ(all_unresolved.reclamation_index, std::nullopt);

  const auto one_settled = decide_policy_receipt_capacity(
    64,
    64,
    {false, false, true, false}
  );
  EXPECT_EQ(
    one_settled.action,
    policy_receipt_capacity_action_e::reclaim_settled
  );
  EXPECT_EQ(one_settled.reclamation_index, std::optional<std::size_t> {2});

  const auto below_capacity = decide_policy_receipt_capacity(63, 64, {});
  EXPECT_EQ(below_capacity.action, policy_receipt_capacity_action_e::append);
  EXPECT_EQ(below_capacity.reclamation_index, std::nullopt);
}

TEST(AudioEndpointRestorePolicy, FixedRoleLanesLetSiblingsProgressPastBlockedConsole) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  EXPECT_EQ(lanes.worker_capacity(), 3u);

  std::promise<void> console_started;
  auto console_started_signal = console_started.get_future();
  std::promise<void> release_console;
  auto release_console_signal = release_console.get_future().share();
  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token stop_token) {
      console_started.set_value();
      while (!stop_token.stop_requested() &&
             release_console_signal.wait_for(10ms) != std::future_status::ready) {
      }
    },
    []() {},
  }));
  ASSERT_EQ(console_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> multimedia_done;
  std::promise<void> communications_done;
  auto multimedia_signal = multimedia_done.get_future();
  auto communications_signal = communications_done.get_future();
  ASSERT_TRUE(lanes.submit_foreground(1, {
    [&](std::stop_token) { multimedia_done.set_value(); },
    []() {},
  }));
  ASSERT_TRUE(lanes.submit_foreground(2, {
    [&](std::stop_token) { communications_done.set_value(); },
    []() {},
  }));
  EXPECT_EQ(multimedia_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(communications_signal.wait_for(1s), std::future_status::ready);
  release_console.set_value();
}

TEST(AudioEndpointRestorePolicy, CrossRoleSynchronousLaneCallsFailClosedWithoutMutualWait) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> console_entered;
  std::promise<void> multimedia_entered;
  auto console_entered_signal = console_entered.get_future().share();
  auto multimedia_entered_signal = multimedia_entered.get_future().share();
  std::promise<int> console_result;
  std::promise<int> multimedia_result;
  auto console_result_signal = console_result.get_future();
  auto multimedia_result_signal = multimedia_result.get_future();

  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token) {
      console_entered.set_value();
      multimedia_entered_signal.wait();
      console_result.set_value(lanes.invoke_foreground<int>(
        1,
        [](std::stop_token) { return 7; },
        -17,
        -23
      ));
    },
    []() {},
  }));
  ASSERT_TRUE(lanes.submit_foreground(1, {
    [&](std::stop_token) {
      multimedia_entered.set_value();
      console_entered_signal.wait();
      multimedia_result.set_value(lanes.invoke_foreground<int>(
        0,
        [](std::stop_token) { return 11; },
        -17,
        -23
      ));
    },
    []() {},
  }));

  const auto console_ready = console_result_signal.wait_for(250ms);
  const auto multimedia_ready = multimedia_result_signal.wait_for(250ms);
  EXPECT_EQ(console_ready, std::future_status::ready);
  EXPECT_EQ(multimedia_ready, std::future_status::ready);
  lanes.shutdown();
  EXPECT_EQ(console_result_signal.get(), -17);
  EXPECT_EQ(multimedia_result_signal.get(), -17);
}

TEST(AudioEndpointRestorePolicy, FixedRoleLaneQueueIsCapacityOneLatestIntentWins) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> running_started;
  auto running_started_signal = running_started.get_future();
  std::promise<void> release_running;
  auto release_running_signal = release_running.get_future().share();
  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token stop_token) {
      running_started.set_value();
      while (!stop_token.stop_requested() &&
             release_running_signal.wait_for(10ms) != std::future_status::ready) {
      }
    },
    []() {},
  }));
  ASSERT_EQ(running_started_signal.wait_for(1s), std::future_status::ready);

  std::atomic_int superseded {0};
  std::atomic_int executed {-1};
  std::promise<void> latest_done;
  auto latest_done_signal = latest_done.get_future();
  for (int intent = 0; intent < 50; ++intent) {
    ASSERT_TRUE(lanes.submit_foreground(0, {
      [&, intent](std::stop_token) {
        executed.store(intent, std::memory_order_release);
        if (intent == 49) {
          latest_done.set_value();
        }
      },
      [&]() { superseded.fetch_add(1, std::memory_order_relaxed); },
    }));
  }
  EXPECT_EQ(lanes.worker_capacity(), 3u);
  release_running.set_value();
  ASSERT_EQ(latest_done_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(executed.load(std::memory_order_acquire), 49);
  EXPECT_EQ(superseded.load(std::memory_order_relaxed), 49);
}

TEST(AudioEndpointRestorePolicy, FixedRoleLaneShutdownJoinsBeforeStateDestruction) {
  using namespace std::chrono_literals;
  std::atomic_int callbacks {0};
  std::promise<void> foreground_started;
  auto foreground_started_signal = foreground_started.get_future();
  std::promise<void> foreground_stopped;
  auto foreground_stopped_signal = foreground_stopped.get_future();
  {
    fixed_role_lane_coordinator_t lanes;
    ASSERT_TRUE(lanes.submit_foreground(0, {
      [&](std::stop_token stop_token) {
        foreground_started.set_value();
        while (!stop_token.stop_requested()) {
          std::this_thread::sleep_for(1ms);
        }
        callbacks.fetch_add(1, std::memory_order_relaxed);
        foreground_stopped.set_value();
      },
      []() {},
    }));
    ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);
  }
  ASSERT_EQ(foreground_stopped_signal.wait_for(1s), std::future_status::ready);
  const auto callbacks_after_destruction = callbacks.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(25ms);
  EXPECT_EQ(callbacks.load(std::memory_order_relaxed), callbacks_after_destruction);
}

TEST(AudioEndpointRestorePolicy, FixedRoleLaneThrownForegroundCompletesSynchronousCaller) {
  fixed_role_lane_coordinator_t lanes;
  const auto result = lanes.invoke_foreground<int>(
    0,
    [](std::stop_token) -> int {
      throw std::runtime_error("injected foreground failure");
    },
    -17,
    -23
  );
  EXPECT_EQ(result, -17);
  lanes.shutdown();
}

TEST(AudioEndpointRestorePolicy, OlderObserverStepCannotClearReplacementGeneration) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> predecessor_started;
  auto predecessor_started_signal = predecessor_started.get_future();
  std::promise<void> release_predecessor;
  auto release_predecessor_signal = release_predecessor.get_future().share();
  ASSERT_TRUE(lanes.set_observer_step(0, [&](std::stop_token stop_token) {
    predecessor_started.set_value();
    while (!stop_token.stop_requested() &&
           release_predecessor_signal.wait_for(10ms) != std::future_status::ready) {
    }
    return false;
  }));
  ASSERT_EQ(predecessor_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> replacement_ran;
  auto replacement_signal = replacement_ran.get_future();
  ASSERT_TRUE(lanes.set_observer_step(0, [&](std::stop_token) {
    replacement_ran.set_value();
    return false;
  }));
  release_predecessor.set_value();
  EXPECT_EQ(replacement_signal.wait_for(1s), std::future_status::ready);
}

TEST(AudioEndpointRestorePolicy, RestoreSlotRejectsOlderAndKeepsEqualInstallIdempotent) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> foreground_started;
  auto foreground_started_signal = foreground_started.get_future();
  std::promise<void> release_foreground;
  auto release_foreground_signal = release_foreground.get_future().share();
  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token stop_token) {
      foreground_started.set_value();
      while (!stop_token.stop_requested() &&
             release_foreground_signal.wait_for(1ms) != std::future_status::ready) {
      }
    },
    []() {},
  }));
  ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> newest_ran;
  auto newest_signal = newest_ran.get_future();
  EXPECT_EQ(
    lanes.set_restore_step(0, {7, 12}, [&](std::stop_token) {
      newest_ran.set_value();
      return false;
    }),
    restore_slot_install_result_e::installed
  );
  EXPECT_EQ(
    lanes.set_restore_step(0, {7, 12}, [&](std::stop_token) {
      ADD_FAILURE() << "equal-key install replaced the original callback";
      return false;
    }),
    restore_slot_install_result_e::unchanged_equal
  );
  EXPECT_EQ(
    lanes.set_restore_step(0, {7, 11}, [&](std::stop_token) {
      ADD_FAILURE() << "older assignment callback executed";
      return false;
    }),
    restore_slot_install_result_e::rejected_older
  );
  EXPECT_EQ(
    lanes.set_restore_step(0, {6, 99}, [&](std::stop_token) {
      ADD_FAILURE() << "older runtime callback executed";
      return false;
    }),
    restore_slot_install_result_e::rejected_older
  );

  release_foreground.set_value();
  EXPECT_EQ(newest_signal.wait_for(1s), std::future_status::ready);
}

TEST(AudioEndpointRestorePolicy, RestoreSlotOldClearAndCompletionCannotRemoveNewerKey) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> old_started;
  auto old_started_signal = old_started.get_future();
  std::promise<void> release_old;
  auto release_old_signal = release_old.get_future().share();
  ASSERT_EQ(
    lanes.set_restore_step(0, {3, 9}, [&](std::stop_token stop_token) {
      old_started.set_value();
      while (!stop_token.stop_requested() &&
             release_old_signal.wait_for(1ms) != std::future_status::ready) {
      }
      return false;
    }),
    restore_slot_install_result_e::installed
  );
  ASSERT_EQ(old_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> newest_ran;
  auto newest_signal = newest_ran.get_future();
  std::promise<void> release_newest;
  auto release_newest_signal = release_newest.get_future().share();
  EXPECT_EQ(
    lanes.set_restore_step(0, {3, 10}, [&](std::stop_token stop_token) {
      newest_ran.set_value();
      while (!stop_token.stop_requested() &&
             release_newest_signal.wait_for(1ms) != std::future_status::ready) {
      }
      return false;
    }),
    restore_slot_install_result_e::installed
  );
  EXPECT_FALSE(lanes.clear_restore_step(0, {3, 9}));
  release_old.set_value();
  EXPECT_EQ(newest_signal.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(lanes.clear_restore_step(0, {3, 10}));
  release_newest.set_value();
}

TEST(AudioEndpointRestorePolicy, RestoreAssignmentFloorRejectsStaleReplayOnlyForSameRuntime) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> foreground_started;
  auto foreground_started_signal = foreground_started.get_future();
  std::promise<void> release_foreground;
  auto release_foreground_signal = release_foreground.get_future().share();
  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token stop_token) {
      foreground_started.set_value();
      while (!stop_token.stop_requested() &&
             release_foreground_signal.wait_for(1ms) != std::future_status::ready) {
      }
    },
    []() {},
  }));
  ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);

  EXPECT_EQ(
    lanes.set_restore_step(0, {5, 7}, [](std::stop_token) { return false; }),
    restore_slot_install_result_e::installed
  );
  std::promise<void> other_runtime_ran;
  auto other_runtime_signal = other_runtime_ran.get_future();
  EXPECT_EQ(
    lanes.set_restore_step(1, {4, 99}, [&](std::stop_token) {
      other_runtime_ran.set_value();
      return false;
    }),
    restore_slot_install_result_e::installed
  );
  EXPECT_EQ(lanes.clear_restore_steps_before({5, 8}), 1u);
  EXPECT_EQ(
    lanes.set_restore_step(0, {5, 7}, [&](std::stop_token) {
      ADD_FAILURE() << "stale replay crossed the assignment floor";
      return false;
    }),
    restore_slot_install_result_e::rejected_older
  );
  std::promise<void> current_assignment_ran;
  auto current_assignment_signal = current_assignment_ran.get_future();
  EXPECT_EQ(
    lanes.set_restore_step(0, {5, 8}, [&](std::stop_token) {
      current_assignment_ran.set_value();
      return false;
    }),
    restore_slot_install_result_e::installed
  );

  release_foreground.set_value();
  EXPECT_EQ(current_assignment_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(other_runtime_signal.wait_for(1s), std::future_status::ready);
}

TEST(AudioEndpointRestorePolicy, DelayedObserverPreemptsNextRestorePollingIteration) {
  using namespace std::chrono_literals;
  const auto first_turn = select_role_lane_background(true, true, true);
  ASSERT_EQ(first_turn.selection, role_lane_background_selection_e::observer);
  EXPECT_FALSE(first_turn.next_prefer_observer);
  const auto second_turn = select_role_lane_background(
    true,
    true,
    first_turn.next_prefer_observer
  );
  EXPECT_EQ(second_turn.selection, role_lane_background_selection_e::restore);
  EXPECT_TRUE(second_turn.next_prefer_observer);

  fixed_role_lane_coordinator_t lanes;
  std::promise<void> first_restore_started;
  auto first_restore_started_signal = first_restore_started.get_future();
  std::promise<void> release_first_restore;
  auto release_first_restore_signal = release_first_restore.get_future().share();
  std::promise<void> observer_ran;
  auto observer_ran_signal = observer_ran.get_future();
  std::promise<void> second_restore_ran;
  auto second_restore_ran_signal = second_restore_ran.get_future();
  std::atomic_bool stale_target_repaired {false};
  std::atomic_int restore_steps {0};
  ASSERT_EQ(lanes.set_restore_step(0, {1, 1}, [&](std::stop_token stop_token) {
    const auto step = restore_steps.fetch_add(1, std::memory_order_relaxed);
    if (step == 0) {
      first_restore_started.set_value();
      while (!stop_token.stop_requested() &&
             release_first_restore_signal.wait_for(1ms) != std::future_status::ready) {
      }
      return true;
    }
    EXPECT_TRUE(stale_target_repaired.load(std::memory_order_acquire));
    second_restore_ran.set_value();
    return false;
  }), restore_slot_install_result_e::installed);
  ASSERT_EQ(first_restore_started_signal.wait_for(1s), std::future_status::ready);
  ASSERT_TRUE(lanes.ensure_observer_step(0, [&](std::stop_token) {
    stale_target_repaired.store(true, std::memory_order_release);
    observer_ran.set_value();
    return false;
  }));
  release_first_restore.set_value();
  EXPECT_EQ(observer_ran_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(second_restore_ran_signal.wait_for(1s), std::future_status::ready);
}

TEST(AudioEndpointRestorePolicy, ObserverSchedulingRestartsAfterQueuedRuntimeShutdown) {
  using namespace std::chrono_literals;
  std::atomic_int observations {0};
  {
    fixed_role_lane_coordinator_t lanes;
    std::promise<void> foreground_started;
    auto foreground_started_signal = foreground_started.get_future();
    ASSERT_TRUE(lanes.submit_foreground(0, {
      [&](std::stop_token stop_token) {
        foreground_started.set_value();
        while (!stop_token.stop_requested()) {
          std::this_thread::sleep_for(1ms);
        }
      },
      []() {},
    }));
    ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);
    ASSERT_TRUE(lanes.ensure_observer_step(0, [&](std::stop_token) {
      observations.fetch_add(1, std::memory_order_relaxed);
      return false;
    }));
    lanes.shutdown();
  }
  EXPECT_EQ(observations.load(std::memory_order_relaxed), 0);

  std::promise<void> restarted_observer_ran;
  auto restarted_observer_ran_signal = restarted_observer_ran.get_future();
  {
    fixed_role_lane_coordinator_t restarted_lanes;
    ASSERT_TRUE(restarted_lanes.ensure_observer_step(0, [&](std::stop_token) {
      observations.fetch_add(1, std::memory_order_relaxed);
      restarted_observer_ran.set_value();
      return false;
    }));
    EXPECT_EQ(
      restarted_observer_ran_signal.wait_for(1s),
      std::future_status::ready
    );
  }
  EXPECT_EQ(observations.load(std::memory_order_relaxed), 1);
}

TEST(AudioEndpointRestorePolicy, ThrownBackgroundStepIsRetriedFailClosed) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::atomic_int attempts {0};
  std::promise<void> retried;
  auto retried_signal = retried.get_future();
  ASSERT_TRUE(lanes.ensure_observer_step(0, [&](std::stop_token) {
    if (attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
      throw std::runtime_error("injected observer failure");
    }
    retried.set_value();
    return false;
  }));
  EXPECT_EQ(retried_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(attempts.load(std::memory_order_relaxed), 2);
}

TEST(AudioEndpointRestorePolicy, PartialLaneConstructionStopsAndJoinsStartedWorkers) {
  using namespace std::chrono_literals;
  std::atomic_int stopped_workers {0};
  auto construction = std::async(std::launch::async, [&]() {
    EXPECT_THROW(
      fixed_role_lane_coordinator_t([&](
        std::size_t index,
        std::function<void(std::stop_token)> entry
      ) {
        if (index == 1) {
          throw std::runtime_error("injected jthread construction failure");
        }
        return std::jthread([
          entry = std::move(entry),
          &stopped_workers
        ](std::stop_token stop_token) mutable {
          entry(stop_token);
          stopped_workers.fetch_add(1, std::memory_order_release);
        });
      }),
      std::runtime_error
    );
  });
  EXPECT_EQ(construction.wait_for(1s), std::future_status::ready);
  construction.get();
  EXPECT_EQ(stopped_workers.load(std::memory_order_acquire), 1);
}

TEST(AudioEndpointRestorePolicy, RuntimeActivationWaitsForPriorGenerationToJoin) {
  using namespace std::chrono_literals;
  fixed_role_lane_runtime_t runtime;
  EXPECT_TRUE(runtime.activate());
  auto predecessor = runtime.get();
  ASSERT_TRUE(predecessor);
  const auto predecessor_generation = runtime.generation();

  std::promise<void> predecessor_started;
  auto predecessor_started_signal = predecessor_started.get_future();
  std::promise<void> release_predecessor;
  auto release_predecessor_signal = release_predecessor.get_future().share();
  ASSERT_TRUE(predecessor->submit_foreground(0, {
    [&](std::stop_token) {
      predecessor_started.set_value();
      release_predecessor_signal.wait();
    },
    []() {},
  }));
  ASSERT_EQ(predecessor_started_signal.wait_for(1s), std::future_status::ready);

  auto last_release = std::async(std::launch::async, [&]() {
    return runtime.release();
  });
  while (!runtime.quiescing()) {
    std::this_thread::yield();
  }
  auto next_activation = std::async(std::launch::async, [&]() {
    return runtime.activate();
  });
  EXPECT_EQ(next_activation.wait_for(50ms), std::future_status::timeout);
  EXPECT_FALSE(runtime.get());

  release_predecessor.set_value();
  ASSERT_EQ(last_release.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(last_release.get());
  ASSERT_EQ(next_activation.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(next_activation.get());
  auto successor = runtime.get();
  ASSERT_TRUE(successor);
  EXPECT_NE(successor, predecessor);
  EXPECT_GT(runtime.generation(), predecessor_generation);
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, RuntimeCreationReplaysRetainedWorkAfterThrowAndNull) {
  using namespace std::chrono_literals;
  std::atomic_int factory_attempts {0};
  std::atomic_int replay_count {0};
  std::promise<void> observer_ran;
  std::promise<void> restore_ran;
  auto observer_signal = observer_ran.get_future();
  auto restore_signal = restore_ran.get_future();
  fixed_role_lane_runtime_t *runtime_view = nullptr;

  fixed_role_lane_runtime_t runtime(
    [&]() -> std::shared_ptr<fixed_role_lane_coordinator_t> {
      const auto attempt = factory_attempts.fetch_add(1, std::memory_order_relaxed);
      if (attempt == 0) {
        throw std::runtime_error("injected coordinator factory failure");
      }
      if (attempt == 1) {
        return {};
      }
      return std::make_shared<fixed_role_lane_coordinator_t>();
    },
    [&](const std::shared_ptr<fixed_role_lane_coordinator_t> &lanes,
        std::uint64_t generation) {
      EXPECT_EQ(runtime_view->generation() + 1, generation);
      replay_count.fetch_add(1, std::memory_order_relaxed);
      ASSERT_TRUE(lanes->ensure_observer_step(0, [&](std::stop_token) {
        observer_ran.set_value();
        return false;
      }));
      ASSERT_EQ(lanes->set_restore_step(0, {generation, 1}, [&](std::stop_token) {
        restore_ran.set_value();
        return false;
      }), restore_slot_install_result_e::installed);
    }
  );
  runtime_view = &runtime;

  EXPECT_TRUE(runtime.activate());
  bool created = true;
  EXPECT_FALSE(runtime.get(&created));
  EXPECT_FALSE(created);
  created = true;
  EXPECT_FALSE(runtime.get(&created));
  EXPECT_FALSE(created);
  const auto lanes = runtime.get(&created);
  ASSERT_TRUE(lanes);
  EXPECT_TRUE(created);
  EXPECT_EQ(observer_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(restore_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(factory_attempts.load(std::memory_order_relaxed), 3);
  EXPECT_EQ(replay_count.load(std::memory_order_relaxed), 1);

  created = true;
  EXPECT_EQ(runtime.get(&created), lanes);
  EXPECT_FALSE(created);
  EXPECT_EQ(replay_count.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, RuntimeCreationIsSingleFlightUntilReplayCompletes) {
  using namespace std::chrono_literals;
  std::promise<void> replay_started;
  auto replay_started_signal = replay_started.get_future();
  std::promise<void> release_replay;
  auto release_replay_signal = release_replay.get_future().share();
  std::atomic_int callback_count {0};
  fixed_role_lane_runtime_t *runtime_view = nullptr;
  fixed_role_lane_runtime_t runtime(
    []() { return std::make_shared<fixed_role_lane_coordinator_t>(); },
    [&](const std::shared_ptr<fixed_role_lane_coordinator_t> &,
        std::uint64_t generation) {
      EXPECT_EQ(generation, 1u);
      EXPECT_EQ(runtime_view->generation(), 0u);
      callback_count.fetch_add(1, std::memory_order_relaxed);
      replay_started.set_value();
      release_replay_signal.wait();
    }
  );
  runtime_view = &runtime;
  EXPECT_TRUE(runtime.activate());

  std::uint64_t first_generation = 0;
  auto first_get = std::async(std::launch::async, [&]() {
    return runtime.get(nullptr, &first_generation);
  });
  ASSERT_EQ(replay_started_signal.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(runtime.generation(), 0u);

  std::uint64_t second_generation = 0;
  auto second_get = std::async(std::launch::async, [&]() {
    return runtime.get(nullptr, &second_generation);
  });
  EXPECT_EQ(second_get.wait_for(50ms), std::future_status::timeout);

  release_replay.set_value();
  ASSERT_EQ(first_get.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(second_get.wait_for(1s), std::future_status::ready);
  const auto first = first_get.get();
  const auto second = second_get.get();
  ASSERT_TRUE(first);
  EXPECT_EQ(second, first);
  EXPECT_EQ(first_generation, 1u);
  EXPECT_EQ(second_generation, 1u);
  EXPECT_EQ(runtime.generation(), 1u);
  EXPECT_EQ(callback_count.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, PausedStaleReplayCannotReplaceNewAssignmentSlot) {
  using namespace std::chrono_literals;
  std::promise<void> replay_started;
  auto replay_started_signal = replay_started.get_future();
  std::promise<void> release_replay;
  auto release_replay_signal = release_replay.get_future().share();
  std::promise<void> foreground_started;
  auto foreground_started_signal = foreground_started.get_future();
  std::promise<void> release_foreground;
  auto release_foreground_signal = release_foreground.get_future().share();
  std::promise<void> new_assignment_ran;
  auto new_assignment_signal = new_assignment_ran.get_future();
  std::atomic_bool stale_assignment_ran {false};

  fixed_role_lane_runtime_t runtime(
    []() { return std::make_shared<fixed_role_lane_coordinator_t>(); },
    [&](const std::shared_ptr<fixed_role_lane_coordinator_t> &lanes,
        std::uint64_t generation) {
      EXPECT_TRUE(lanes->submit_foreground(0, {
        [&](std::stop_token stop_token) {
          foreground_started.set_value();
          while (!stop_token.stop_requested() &&
                 release_foreground_signal.wait_for(1ms) !=
                   std::future_status::ready) {
          }
        },
        []() {},
      }));
      EXPECT_EQ(
        lanes->set_restore_step(0, {generation, 4}, [&](std::stop_token) {
          stale_assignment_ran.store(true, std::memory_order_release);
          return false;
        }),
        restore_slot_install_result_e::installed
      );
      replay_started.set_value();
      release_replay_signal.wait();
    }
  );
  EXPECT_TRUE(runtime.activate());

  auto creation = std::async(std::launch::async, [&]() {
    return runtime.get();
  });
  ASSERT_EQ(replay_started_signal.wait_for(1s), std::future_status::ready);
  auto newer_assignment = std::async(std::launch::async, [&]() {
    std::uint64_t generation = 0;
    const auto lanes = runtime.get(nullptr, &generation);
    if (!lanes) {
      return false;
    }
    lanes->clear_restore_steps_before({generation, 5});
    return lanes->set_restore_step(0, {generation, 5}, [&](std::stop_token) {
      new_assignment_ran.set_value();
      return false;
    }) == restore_slot_install_result_e::installed;
  });
  EXPECT_EQ(newer_assignment.wait_for(50ms), std::future_status::timeout);

  release_replay.set_value();
  ASSERT_EQ(creation.wait_for(1s), std::future_status::ready);
  ASSERT_TRUE(creation.get());
  ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);
  ASSERT_EQ(newer_assignment.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(newer_assignment.get());
  EXPECT_FALSE(stale_assignment_ran.load(std::memory_order_acquire));

  release_foreground.set_value();
  EXPECT_EQ(new_assignment_signal.wait_for(1s), std::future_status::ready);
  EXPECT_FALSE(stale_assignment_ran.load(std::memory_order_acquire));
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, RuntimeReplayExceptionDiscardsAndRetries) {
  std::atomic_int callback_attempts {0};
  std::vector<std::weak_ptr<fixed_role_lane_coordinator_t>> candidates;
  fixed_role_lane_runtime_t runtime(
    [&]() {
      auto candidate = std::make_shared<fixed_role_lane_coordinator_t>();
      candidates.emplace_back(candidate);
      return candidate;
    },
    [&](const std::shared_ptr<fixed_role_lane_coordinator_t> &lanes,
        std::uint64_t generation) {
      if (callback_attempts.fetch_add(1, std::memory_order_relaxed) == 0) {
        EXPECT_EQ(generation, 1u);
        EXPECT_EQ(
          lanes->set_restore_step(
            0,
            {generation, 1},
            [](std::stop_token) { return false; }
          ),
          restore_slot_install_result_e::installed
        );
        throw std::runtime_error("injected replay failure");
      }
      EXPECT_EQ(generation, 1u);
    }
  );
  EXPECT_TRUE(runtime.activate());

  bool created = true;
  EXPECT_FALSE(runtime.get(&created));
  EXPECT_FALSE(created);
  EXPECT_EQ(runtime.generation(), 0u);
  ASSERT_EQ(candidates.size(), 1u);
  EXPECT_TRUE(candidates.front().expired());

  const auto successor = runtime.get(&created);
  ASSERT_TRUE(successor);
  EXPECT_TRUE(created);
  EXPECT_EQ(runtime.generation(), 1u);
  EXPECT_EQ(callback_attempts.load(std::memory_order_relaxed), 2);
  ASSERT_EQ(candidates.size(), 2u);
  EXPECT_FALSE(candidates.back().expired());
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, RuntimeRotationJoinsBeforePublishingSuccessor) {
  using namespace std::chrono_literals;
  fixed_role_lane_runtime_t runtime;
  EXPECT_TRUE(runtime.activate());
  std::uint64_t predecessor_generation = 0;
  const auto predecessor = runtime.get(nullptr, &predecessor_generation);
  ASSERT_TRUE(predecessor);

  std::promise<void> predecessor_started;
  auto predecessor_started_signal = predecessor_started.get_future();
  std::promise<void> release_predecessor;
  auto release_predecessor_signal = release_predecessor.get_future().share();
  ASSERT_TRUE(predecessor->submit_foreground(0, {
    [&](std::stop_token) {
      predecessor_started.set_value();
      release_predecessor_signal.wait();
    },
    []() {},
  }));
  ASSERT_EQ(predecessor_started_signal.wait_for(1s), std::future_status::ready);

  std::uint64_t successor_generation = 0;
  auto rotation = std::async(std::launch::async, [&]() {
    return runtime.rotate(&successor_generation);
  });
  while (!runtime.quiescing()) {
    std::this_thread::yield();
  }
  EXPECT_EQ(rotation.wait_for(50ms), std::future_status::timeout);
  EXPECT_FALSE(runtime.get());

  release_predecessor.set_value();
  ASSERT_EQ(rotation.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(rotation.get());
  const auto successor = runtime.get();
  ASSERT_TRUE(successor);
  EXPECT_NE(successor, predecessor);
  EXPECT_GT(successor_generation, predecessor_generation);
  EXPECT_EQ(successor_generation, runtime.generation());
  EXPECT_TRUE(runtime.release());
}

TEST(AudioEndpointRestorePolicy, AssignmentEpochExhaustionRequiresRuntimeRotation) {
  int rotations = 0;
  const auto ordinary = advance_policy_assignment_epoch(41, [&]() {
    ++rotations;
    return true;
  });
  ASSERT_EQ(ordinary, std::optional<std::uint64_t> {42});
  EXPECT_EQ(rotations, 0);

  const auto rejected = advance_policy_assignment_epoch(
    std::numeric_limits<std::uint64_t>::max(),
    [&]() {
      ++rotations;
      return false;
    }
  );
  EXPECT_FALSE(rejected);
  EXPECT_EQ(rotations, 1);

  const auto wrapped = advance_policy_assignment_epoch(
    std::numeric_limits<std::uint64_t>::max(),
    [&]() {
      ++rotations;
      return true;
    }
  );
  ASSERT_EQ(wrapped, std::optional<std::uint64_t> {1});
  EXPECT_EQ(rotations, 2);
}

TEST(AudioEndpointRestorePolicy, RestoreBackgroundYieldsToBoundedForegroundRead) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::promise<void> restore_started;
  auto restore_started_signal = restore_started.get_future();
  std::atomic_int restore_steps {0};
  ASSERT_EQ(lanes.set_restore_step(0, {1, 1}, [&](std::stop_token stop_token) {
    const auto step = restore_steps.fetch_add(1, std::memory_order_relaxed);
    if (step == 0) {
      restore_started.set_value();
      while (!stop_token.stop_requested() && !lanes.has_pending_foreground(0)) {
        std::this_thread::sleep_for(1ms);
      }
      return true;
    }
    return false;
  }), restore_slot_install_result_e::installed);
  ASSERT_EQ(restore_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> read_done;
  auto read_done_signal = read_done.get_future();
  ASSERT_TRUE(lanes.submit_foreground(0, {
    [&](std::stop_token) { read_done.set_value(); },
    []() {},
  }));
  EXPECT_EQ(read_done_signal.wait_for(1s), std::future_status::ready);
}

TEST(AudioEndpointRestorePolicy, SustainedForegroundLoadCannotStarveObserverOrRestore) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::atomic_bool keep_producing {true};
  std::atomic_int foreground_runs {0};
  std::promise<void> foreground_started;
  auto foreground_started_signal = foreground_started.get_future();

  auto submit_next = std::make_shared<std::function<void()>>();
  std::weak_ptr<std::function<void()>> weak_submit_next = submit_next;
  *submit_next = [&, weak_submit_next]() {
    ASSERT_TRUE(lanes.submit_foreground(0, {
      [&, weak_submit_next](std::stop_token stop_token) {
        const auto run = foreground_runs.fetch_add(1, std::memory_order_relaxed);
        if (!stop_token.stop_requested() &&
            keep_producing.load(std::memory_order_acquire)) {
          if (const auto next = weak_submit_next.lock()) {
            (*next)();
          }
        }
        if (run == 0) {
          foreground_started.set_value();
        }
        std::this_thread::sleep_for(1ms);
      },
      []() {},
    }));
  };
  (*submit_next)();
  ASSERT_EQ(foreground_started_signal.wait_for(1s), std::future_status::ready);

  std::promise<void> observer_ran;
  std::promise<void> restore_ran;
  auto observer_signal = observer_ran.get_future();
  auto restore_signal = restore_ran.get_future();
  std::atomic_int observer_foreground_count {-1};
  std::atomic_int restore_foreground_count {-1};
  ASSERT_TRUE(lanes.set_observer_step(0, [&](std::stop_token) {
    observer_foreground_count.store(
      foreground_runs.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    observer_ran.set_value();
    return false;
  }));
  ASSERT_EQ(lanes.set_restore_step(0, {1, 1}, [&](std::stop_token) {
    restore_foreground_count.store(
      foreground_runs.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    restore_ran.set_value();
    return false;
  }), restore_slot_install_result_e::installed);

  constexpr auto progress_bound = 250ms;
  EXPECT_EQ(observer_signal.wait_for(progress_bound), std::future_status::ready);
  EXPECT_EQ(restore_signal.wait_for(progress_bound), std::future_status::ready);
  EXPECT_GT(foreground_runs.load(std::memory_order_relaxed), 1);
  const auto background_gap = observer_foreground_count.load(
                                std::memory_order_relaxed
                              ) -
                              restore_foreground_count.load(
                                std::memory_order_relaxed
                              );
  EXPECT_LE(background_gap < 0 ? -background_gap : background_gap, 1);

  keep_producing.store(false, std::memory_order_release);
  submit_next.reset();
  lanes.shutdown();
}

TEST(AudioEndpointRestorePolicy, GrantedRestoreTurnRunsOnePhaseBeforeYielding) {
  EXPECT_EQ(
    granted_restore_phase_action(false, true),
    granted_restore_phase_action_e::run
  );
  EXPECT_EQ(
    granted_restore_phase_action(true, true),
    granted_restore_phase_action_e::cancel
  );
  EXPECT_EQ(
    granted_restore_phase_action(false, false),
    granted_restore_phase_action_e::cancel
  );
}

TEST(AudioEndpointRestorePolicy, SustainedSlowForegroundAllowsRepeatedBackgroundProgress) {
  using namespace std::chrono_literals;
  fixed_role_lane_coordinator_t lanes;
  std::atomic_bool keep_producing {true};
  std::atomic_int foreground_runs {0};
  auto submit_next = std::make_shared<std::function<void()>>();
  std::weak_ptr<std::function<void()>> weak_submit_next = submit_next;
  *submit_next = [&, weak_submit_next]() {
    if (!keep_producing.load(std::memory_order_acquire)) {
      return;
    }
    EXPECT_TRUE(lanes.submit_foreground(0, {
      [&, weak_submit_next](std::stop_token stop_token) {
        foreground_runs.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(3ms);
        if (!stop_token.stop_requested() &&
            keep_producing.load(std::memory_order_acquire)) {
          if (const auto next = weak_submit_next.lock()) {
            (*next)();
          }
        }
      },
      []() {},
    }));
  };
  (*submit_next)();

  constexpr int required_steps = 8;
  std::atomic_int observer_steps {0};
  std::atomic_int restore_steps {0};
  std::promise<void> observer_completed;
  std::promise<void> restore_completed;
  auto observer_signal = observer_completed.get_future();
  auto restore_signal = restore_completed.get_future();
  ASSERT_TRUE(lanes.set_observer_step(0, [&](std::stop_token) {
    std::this_thread::sleep_for(2ms);
    const auto step = observer_steps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (step == required_steps) {
      observer_completed.set_value();
    }
    return step < required_steps;
  }));
  ASSERT_EQ(
    lanes.set_restore_step(0, {1, 1}, [&](std::stop_token) {
      std::this_thread::sleep_for(2ms);
      const auto step = restore_steps.fetch_add(1, std::memory_order_relaxed) + 1;
      if (step == required_steps) {
        restore_completed.set_value();
      }
      return step < required_steps;
    }),
    restore_slot_install_result_e::installed
  );

  // The production causal observer window is ten seconds; both background
  // classes must repeatedly advance under load with substantial margin.
  EXPECT_EQ(observer_signal.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(restore_signal.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(observer_steps.load(std::memory_order_relaxed), required_steps);
  EXPECT_EQ(restore_steps.load(std::memory_order_relaxed), required_steps);
  EXPECT_GT(foreground_runs.load(std::memory_order_relaxed), required_steps);

  keep_producing.store(false, std::memory_order_release);
  submit_next.reset();
  lanes.shutdown();
}

TEST(AudioEndpointRestorePolicy, FailedCausalRepairUsesBoundedBackoffAndAttemptLimit) {
  causal_repair_state_t repair {false, 0, 0, 10'000};
  auto first = plan_causal_repair(repair, 100, true, 2);
  ASSERT_EQ(first.action, causal_repair_action_e::issue);
  repair = complete_causal_repair(first.state, 110, false, false, 1'000);
  EXPECT_FALSE(repair.outstanding);
  EXPECT_EQ(repair.next_attempt_tick, 1'110u);
  EXPECT_EQ(plan_causal_repair(repair, 1'109, true, 2).action, causal_repair_action_e::wait);

  auto second = plan_causal_repair(repair, 1'110, true, 2);
  ASSERT_EQ(second.action, causal_repair_action_e::issue);
  EXPECT_EQ(second.state.attempts, 2u);
  second.state.outstanding = false;
  EXPECT_EQ(
    plan_causal_repair(second.state, 2'110, true, 2).action,
    causal_repair_action_e::retire
  );
}

TEST(AudioEndpointRestorePolicy, OverlappingSameTargetReceiptsKeepIndependentRepairWindows) {
  const std::vector<causal_repair_chain_candidate_t> chains {
    {41, "steam-speakers", {false, 1, 0, 400}},
    {42, "steam-speakers", {false, 0, 0, 1'400}},
  };
  const auto selected = select_causal_repair_chain(
    chains,
    "steam-speakers",
    500,
    1
  );
  ASSERT_EQ(selected.causal_receipt_id, std::optional<std::uint64_t> {42});
  EXPECT_EQ(selected.transition.action, causal_repair_action_e::issue);
  EXPECT_EQ(selected.transition.state.attempts, 1u);
  EXPECT_EQ(selected.transition.state.deadline_tick, 1'400u);
}

TEST(AudioEndpointRestorePolicy, RestoreExternalArbitrationRetainsStaleReceiptForObserverRepair) {
  EXPECT_EQ(
    classify_restore_external_observation(
      "steam-speakers",
      {},
      {"steam-speakers"}
    ),
    restore_external_observation_action_e::retry_stale_receipt
  );
  EXPECT_EQ(
    classify_restore_external_observation(
      "realtek",
      {"realtek"},
      {"steam-speakers"}
    ),
    restore_external_observation_action_e::commit_pending_receipt
  );
  EXPECT_EQ(
    classify_restore_external_observation(
      "user-hdmi",
      {"realtek"},
      {"steam-speakers"}
    ),
    restore_external_observation_action_e::adopt_external
  );
}

TEST(AudioEndpointRestorePolicy, ExpiredOutstandingChainCannotBlockNewSameTargetRoot) {
  causal_repair_state_t expired_outstanding {true, 1, 0, 400};
  const auto retired = plan_causal_repair(
    expired_outstanding,
    500,
    true,
    1
  );
  ASSERT_EQ(retired.action, causal_repair_action_e::retire);
  EXPECT_FALSE(retired.state.outstanding);

  const std::vector<causal_repair_chain_candidate_t> chains {
    {41, "steam-speakers", expired_outstanding},
    {42, "steam-speakers", {false, 0, 0, 1'400}},
  };
  const auto selected = select_causal_repair_chain(
    chains,
    "steam-speakers",
    500,
    1
  );
  ASSERT_EQ(selected.causal_receipt_id, std::optional<std::uint64_t> {42});
  EXPECT_EQ(selected.transition.action, causal_repair_action_e::issue);
}

TEST(AudioEndpointRestorePolicy, DefinitelyUnissuedWriteErasesReceiptWhileAmbiguityRetainsHazard) {
  EXPECT_EQ(
    classify_policy_write_execution(false, false, false),
    policy_write_execution_action_e::erase_unissued
  );
  EXPECT_EQ(
    classify_policy_write_execution(true, false, false),
    policy_write_execution_action_e::retain_hazard
  );
  EXPECT_EQ(
    classify_policy_write_execution(true, true, false),
    policy_write_execution_action_e::erase_unissued
  );
}

TEST(AudioEndpointRestorePolicy, AuthenticatedPreReadErasesButPostSetFailureRemainsHazard) {
  const policy_pre_read_proof_t authenticated {
    true,
    true,
    true,
    true,
    true,
    true,
  };
  EXPECT_TRUE(authenticated_policy_pre_read_proves_no_write(authenticated));
  auto ambiguous_resume = authenticated;
  ambiguous_resume.execution_completed = false;
  EXPECT_FALSE(
    authenticated_policy_pre_read_proves_no_write(ambiguous_resume)
  );
  auto unreaped = authenticated;
  unreaped.process_reaped = false;
  EXPECT_FALSE(authenticated_policy_pre_read_proves_no_write(unreaped));
  auto post_set = authenticated;
  post_set.pre_read_stage = false;
  EXPECT_FALSE(authenticated_policy_pre_read_proves_no_write(post_set));
  auto set_was_issued = authenticated;
  set_was_issued.set_was_not_issued = false;
  EXPECT_FALSE(
    authenticated_policy_pre_read_proves_no_write(set_was_issued)
  );
  auto read_did_not_fail = authenticated;
  read_did_not_fail.read_failed = false;
  EXPECT_FALSE(
    authenticated_policy_pre_read_proves_no_write(read_did_not_fail)
  );
  auto contradictory_readback = authenticated;
  contradictory_readback.readback_empty = false;
  EXPECT_FALSE(
    authenticated_policy_pre_read_proves_no_write(contradictory_readback)
  );

  EXPECT_EQ(
    classify_policy_write_execution(true, false, true),
    policy_write_execution_action_e::erase_unissued
  );
  EXPECT_EQ(
    classify_policy_write_execution(true, false, false),
    policy_write_execution_action_e::retain_hazard
  );
}

TEST(AudioEndpointRestorePolicy, DefinitelyUnissuedRepairReturnsItsCausalBudget) {
  const causal_repair_state_t claimed {true, 1, 900, 1'400};
  const auto retracted = retract_unissued_causal_repair(claimed, 500);
  EXPECT_FALSE(retracted.outstanding);
  EXPECT_EQ(retracted.attempts, 0u);
  EXPECT_EQ(retracted.next_attempt_tick, 500u);
  EXPECT_EQ(retracted.deadline_tick, 1'400u);
}

TEST(AudioEndpointRestorePolicy, InProgressPolicyCallNeverAdvancesStableRetirement) {
  const auto in_progress = advance_superseded_write_observer(
    false,
    false,
    true,
    true,
    false,
    49,
    50
  );
  EXPECT_EQ(
    in_progress.action,
    superseded_write_observer_progress_action_e::continue_observing
  );
  EXPECT_EQ(in_progress.stable_observations, 0);
}

TEST(AudioEndpointRestorePolicy, MissingDefaultReadNeverAdvancesStableRetirement) {
  const auto missing_read = advance_superseded_write_observer(
    false,
    false,
    false,
    true,
    true,
    49,
    50
  );
  EXPECT_EQ(
    missing_read.action,
    superseded_write_observer_progress_action_e::continue_observing
  );
  EXPECT_EQ(missing_read.stable_observations, 0);

  const auto same_generation_deadline = advance_superseded_write_observer(
    false,
    true,
    false,
    false,
    false,
    0,
    50
  );
  EXPECT_EQ(
    same_generation_deadline.action,
    superseded_write_observer_progress_action_e::retire_generation
  );
}

TEST(AudioEndpointRestorePolicy, FreshResetPollsIncompleteCatalogThenConfirmsExactSteamOwnership) {
  const auto incomplete = build_render_endpoint_catalog(false, {});
  EXPECT_EQ(
    classify_pending_role_ownership(incomplete, "steam-speakers", "steam-speakers"),
    pending_role_ownership_action_e::poll_catalog
  );

  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
  });
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "steam-speakers", "steam-speakers"),
    pending_role_ownership_action_e::confirm_steam_owned
  );
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "steam-speakers", ""),
    pending_role_ownership_action_e::poll_catalog
  );
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "steam-speakers", "user-headset"),
    pending_role_ownership_action_e::release_external
  );
}

TEST(AudioEndpointRestorePolicy, MissingDefaultReadRetainsRoleWhileSiblingExternalChoiceReleases) {
  EXPECT_EQ(
    classify_default_endpoint_observation("steam-console", std::nullopt),
    default_endpoint_observation_e::unavailable
  );
  EXPECT_EQ(
    classify_default_endpoint_observation(
      "steam-console",
      std::optional<std::string> {""}
    ),
    default_endpoint_observation_e::unavailable
  );
  EXPECT_EQ(
    classify_default_endpoint_observation(
      "steam-console",
      std::optional<std::string> {"steam-console"}
    ),
    default_endpoint_observation_e::matches_expected
  );
  EXPECT_EQ(
    classify_default_endpoint_observation(
      "steam-communications",
      std::optional<std::string> {"user-headset"}
    ),
    default_endpoint_observation_e::different_nonempty
  );

  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-console", "Steam Streaming Speakers", true},
    {"steam-communications", "Steam Streaming Speakers", true},
    {"user-headset", "USB Headset", true},
  });
  const std::vector<pending_role_ownership_action_e> sibling_actions {
    classify_pending_role_ownership(complete, "steam-console", std::nullopt),
    classify_pending_role_ownership(
      complete,
      "steam-communications",
      std::optional<std::string> {"user-headset"}
    ),
  };
  EXPECT_EQ(
    sibling_actions,
    (std::vector<pending_role_ownership_action_e> {
      pending_role_ownership_action_e::poll_catalog,
      pending_role_ownership_action_e::release_external,
    })
  );
  EXPECT_EQ(
    classify_pending_role_ownership(
      complete,
      "steam-console",
      std::optional<std::string> {"steam-console"}
    ),
    pending_role_ownership_action_e::confirm_steam_owned
  );
}

TEST(AudioEndpointRestorePolicy, UnavailableReadsNeverCommitAdoptOrReleaseRestoreOwnership) {
  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
  });
  EXPECT_EQ(
    classify_default_endpoint_observation("steam-speakers", std::nullopt),
    default_endpoint_observation_e::unavailable
  );
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "steam-speakers", std::nullopt),
    pending_role_ownership_action_e::poll_catalog
  );
  const auto unavailable_completion = complete_fallback_role_transition(
    "steam-speakers",
    "realtek",
    true,
    std::nullopt
  );
  EXPECT_EQ(unavailable_completion.action, fallback_commit_action_e::retry_committed);
  EXPECT_EQ(unavailable_completion.next_expected_id, "steam-speakers");
  EXPECT_TRUE(unavailable_completion.keep_restore);
}

TEST(AudioEndpointRestorePolicy, FailedRestoreHandoffPollsIncompleteCatalogWithoutArrivalSignal) {
  const auto incomplete = build_render_endpoint_catalog(false, {
    {"steam-speakers", "Steam Streaming Speakers", true},
  });
  EXPECT_EQ(fallback_catalog_action(incomplete), fallback_catalog_action_e::poll_after_backoff);

  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
  });
  EXPECT_EQ(fallback_catalog_action(complete), fallback_catalog_action_e::attempt);
  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-speakers",
      "realtek",
      true,
      std::optional<std::string> {"steam-speakers"}
    ).action,
    fallback_commit_action_e::retry_committed
  );
}

TEST(AudioEndpointRestorePolicy, PreferredFailureUsesAnotherFallbackWithoutAffectingSiblingRole) {
  const auto catalog = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
    {"hdmi", "NVIDIA High Definition Audio", true},
  });

  const auto failed_role_fallback = select_eligible_non_steam_render_endpoint(
    catalog,
    {},
    {"realtek"}
  );
  EXPECT_EQ(failed_role_fallback, std::optional<std::string> {"hdmi"});
  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-speakers",
      *failed_role_fallback,
      true,
      failed_role_fallback
    ).action,
    fallback_commit_action_e::commit_fallback
  );

  EXPECT_EQ(
    select_eligible_non_steam_render_endpoint(catalog, {}, {}),
    std::optional<std::string> {"realtek"}
  );
  EXPECT_TRUE(should_attempt_role_fallback(true, false));
  EXPECT_FALSE(should_attempt_role_fallback(false, false));
  EXPECT_FALSE(should_attempt_role_fallback(true, true));
}

TEST(AudioEndpointRestorePolicy, WorkerBootstrapRetriesInitializationWithoutDroppingRecoveryState) {
  EXPECT_EQ(
    worker_bootstrap_action(true, false),
    worker_bootstrap_action_e::retry_after_backoff
  );
  EXPECT_EQ(worker_bootstrap_action(true, true), worker_bootstrap_action_e::run);
  EXPECT_EQ(worker_bootstrap_action(false, false), worker_bootstrap_action_e::stop);
  EXPECT_EQ(worker_bootstrap_action(false, true), worker_bootstrap_action_e::stop);
}

TEST(AudioEndpointRestorePolicy, FailedWorkerWriteExternalReleaseRetiresOnlyAffectedRole) {
  EXPECT_TRUE(policy_write_requires_settling(false, true));
  const auto external_role_a = observe_superseded_write(
    {"failed-preferred-a"},
    {"steam-a"},
    "user-u",
    "steam-a",
    true,
    false
  );
  ASSERT_EQ(
    external_role_a.action,
    superseded_write_observation_action_e::adopt_external
  );
  EXPECT_EQ(external_role_a.target_id, "user-u");

  const std::vector<worker_role_write_action_e> two_role_results {
    worker_role_write_action(true, false),
    worker_role_write_action(true, true),
  };

  EXPECT_EQ(
    two_role_results,
    (std::vector<worker_role_write_action_e> {
      worker_role_write_action_e::release_external,
      worker_role_write_action_e::use_policy_status,
    })
  );
  EXPECT_EQ(
    worker_role_write_action(false, false),
    worker_role_write_action_e::stop_worker
  );
  EXPECT_EQ(
    worker_role_write_action(false, true),
    worker_role_write_action_e::stop_worker
  );
}

TEST(AudioEndpointRestorePolicy, PostInstallRecoveryUsesExactReadbackAndPreservesSiblingChoice) {
  const auto incomplete = build_render_endpoint_catalog(false, {});
  EXPECT_EQ(
    classify_pending_role_ownership(incomplete, "steam-speakers", "steam-speakers"),
    pending_role_ownership_action_e::poll_catalog
  );

  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
    {"user-headset", "USB Headset", true},
  });
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "", std::nullopt),
    pending_role_ownership_action_e::poll_catalog
  );
  EXPECT_TRUE(should_queue_post_install_unknown_role(false, ""));
  EXPECT_TRUE(should_queue_post_install_unknown_role(false, "old-realtek"));
  EXPECT_FALSE(should_queue_post_install_unknown_role(true, ""));
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "", "steam-speakers"),
    pending_role_ownership_action_e::confirm_steam_owned
  );
  EXPECT_EQ(
    classify_pending_role_ownership(complete, "", "user-headset"),
    pending_role_ownership_action_e::release_external
  );
  const auto restores = plan_steam_role_restores(
    complete,
    {"steam-speakers", "user-headset"},
    {"realtek", "communications-headset"}
  );
  ASSERT_EQ(restores.size(), 1u);
  EXPECT_EQ(restores.front().role_index, 0u);

  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-speakers",
      "realtek",
      false,
      std::optional<std::string> {"steam-speakers"}
    ).action,
    fallback_commit_action_e::retry_committed
  );
  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-speakers",
      "realtek",
      true,
      std::optional<std::string> {"steam-speakers"}
    ).action,
    fallback_commit_action_e::retry_committed
  );
  EXPECT_EQ(
    complete_fallback_role_transition(
      "steam-speakers",
      "realtek",
      true,
      std::optional<std::string> {"realtek"}
    ).action,
    fallback_commit_action_e::commit_fallback
  );
}

TEST(AudioEndpointRestorePolicy, IncompleteDiscoveryPollsThenAttemptsWithoutArrivalSignal) {
  const auto incomplete = build_render_endpoint_catalog(false, {
    {"steam-speakers", "Steam Streaming Speakers", true},
  });
  EXPECT_EQ(fallback_catalog_action(incomplete), fallback_catalog_action_e::poll_after_backoff);

  const auto complete = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"realtek", "Realtek USB Audio", true},
  });
  EXPECT_EQ(fallback_catalog_action(complete), fallback_catalog_action_e::attempt);

  const auto no_non_steam = build_render_endpoint_catalog(true, {
    {"steam-speakers", "Steam Streaming Speakers", true},
    {"steam-microphone", "Steam Streaming Microphone", true},
  });
  EXPECT_EQ(fallback_catalog_action(no_non_steam), fallback_catalog_action_e::wait_for_arrival);
}

namespace {
  class fake_source_t: public sample_source_t {
  public:
    std::deque<sample_status_e> statuses;
    std::deque<bool> reacquire_results;

    sample_status_e sample() override {
      if (statuses.empty()) {
        return sample_status_e::interrupted;
      }
      const auto status = statuses.front();
      statuses.pop_front();
      return status;
    }

    bool reacquire() override {
      if (reacquire_results.empty()) {
        return false;
      }
      const bool result = reacquire_results.front();
      reacquire_results.pop_front();
      return result;
    }
  };
}  // namespace

TEST(AudioCapturePolicy, RetainsSuccessTimeoutReinitializeAndStopLifecycle) {
  fake_source_t source;
  source.statuses = {
    sample_status_e::ok,
    sample_status_e::timeout,
    sample_status_e::reinitialize,
    sample_status_e::ok,
    sample_status_e::interrupted,
  };
  source.reacquire_results = {true};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 2u);
  EXPECT_EQ(summary.timeouts, 1u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}

TEST(AudioCapturePolicy, FailedReinitializeStopsWithoutEmitting) {
  fake_source_t source;
  source.statuses = {sample_status_e::reinitialize, sample_status_e::ok};
  source.reacquire_results = {false};

  const auto summary = drive_capture(source, 10);
  EXPECT_EQ(summary.emitted, 0u);
  EXPECT_EQ(summary.reacquisitions, 1u);
  EXPECT_TRUE(summary.stopped);
}
