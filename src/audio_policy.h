/**
 * @file src/audio_policy.h
 * @brief Platform-neutral audio stream and capture decisions.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace audio::policy {
  struct stream_layout_t {
    int channels;
    int streams;
    int coupled_streams;
    std::array<unsigned char, 8> mapping;
  };

  int stream_index(int channels, bool high_quality);
  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom);

  struct sink_catalog_t {
    std::string host;
    std::optional<std::string> stereo;
    std::optional<std::string> surround51;
    std::optional<std::string> surround71;
  };

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled);

  struct render_endpoint_t {
    std::string id;
    std::string adapter_name;
    bool active;
  };

  struct render_endpoint_catalog_t {
    bool complete;
    std::vector<render_endpoint_t> endpoints;
    std::vector<std::string> steam_endpoint_ids;
    std::vector<std::string> eligible_non_steam_endpoint_ids;
  };

  struct owned_role_snapshot_t {
    std::size_t role_index;
    std::string expected_current_id;
  };

  struct role_restore_plan_t {
    std::size_t role_index;
    std::string expected_current_id;
    std::string target_id;
  };

  enum class fallback_role_action_e {
    assign_selected_fallback,
    keep_steam_for_retry,
    adopt_and_retire,
  };

  struct fallback_role_transition_t {
    fallback_role_action_e action;
    std::string next_expected_id;
    bool keep_restore;
  };

  enum class fallback_commit_action_e {
    commit_fallback,
    release_external,
    retry_committed,
  };

  struct fallback_commit_transition_t {
    fallback_commit_action_e action;
    std::string next_expected_id;
    bool keep_restore;
  };

  enum class fallback_catalog_action_e {
    attempt,
    poll_after_backoff,
    wait_for_arrival,
  };

  enum class pending_role_ownership_action_e {
    poll_catalog,
    confirm_steam_owned,
    release_external,
  };

  enum class default_endpoint_observation_e {
    unavailable,
    matches_expected,
    different_nonempty,
  };

  enum class restore_external_observation_action_e {
    commit_pending_receipt,
    retry_stale_receipt,
    adopt_external,
  };

  enum class worker_bootstrap_action_e {
    run,
    retry_after_backoff,
    stop,
  };

  enum class worker_role_write_action_e {
    use_policy_status,
    release_external,
    stop_worker,
  };

  enum class superseded_write_observation_action_e {
    keep_observing,
    repair_active,
    adopt_external,
    cancel,
  };

  struct superseded_write_observation_transition_t {
    superseded_write_observation_action_e action;
    std::string target_id;
  };

  enum class superseded_write_observer_progress_action_e {
    continue_observing,
    handoff_generation,
    retire_generation,
  };

  struct superseded_write_observer_progress_t {
    superseded_write_observer_progress_action_e action;
    int stable_observations;
  };

  struct superseded_write_ledger_phase_t {
    std::vector<std::string> stale_target_ids;
    std::vector<std::string> tolerated_live_ids;
  };

  struct policy_observer_owner_key_t {
    std::uint64_t generation;
    std::uint64_t assignment_epoch;
    std::uint64_t role_revision;
    std::string desired_id;
  };

  enum class causal_repair_action_e {
    issue,
    wait,
    retire,
  };

  struct causal_repair_state_t {
    bool outstanding;
    unsigned attempts;
    std::uint64_t next_attempt_tick;
    std::uint64_t deadline_tick;
  };

  struct causal_repair_transition_t {
    causal_repair_action_e action;
    causal_repair_state_t state;
  };

  struct causal_repair_chain_candidate_t {
    std::uint64_t causal_receipt_id;
    std::string stale_target_id;
    causal_repair_state_t state;
  };

  struct causal_repair_chain_selection_t {
    std::optional<std::uint64_t> causal_receipt_id;
    causal_repair_transition_t transition;
  };

  enum class policy_receipt_capacity_action_e {
    append,
    reclaim_settled,
    reject_unresolved,
  };

  enum class policy_write_execution_action_e {
    erase_unissued,
    retain_hazard,
  };

  struct policy_pre_read_proof_t {
    bool process_reaped;
    bool execution_completed;
    bool pre_read_stage;
    bool set_was_not_issued;
    bool read_failed;
    bool readback_empty;
  };

  struct policy_receipt_capacity_decision_t {
    policy_receipt_capacity_action_e action;
    std::optional<std::size_t> reclamation_index;
  };

  enum class role_lane_background_selection_e {
    none,
    observer,
    restore,
  };

  struct role_lane_background_transition_t {
    role_lane_background_selection_e selection;
    bool next_prefer_observer;
  };

  struct restore_slot_key_t {
    std::uint64_t runtime_generation = 0;
    std::uint64_t assignment_epoch = 0;

    friend bool operator==(const restore_slot_key_t &, const restore_slot_key_t &) =
      default;
  };

  enum class restore_slot_install_result_e {
    installed,
    unchanged_equal,
    rejected_older,
    unavailable,
  };

  role_lane_background_transition_t select_role_lane_background(
    bool observer_ready,
    bool restore_ready,
    bool prefer_observer
  );

  enum class granted_restore_phase_action_e {
    run,
    cancel,
  };

  granted_restore_phase_action_e granted_restore_phase_action(
    bool stop_requested,
    bool worker_active
  );

  std::optional<std::uint64_t> advance_policy_assignment_epoch(
    std::uint64_t current_epoch,
    const std::function<bool()> &rotate_runtime
  );

  class fixed_role_lane_coordinator_t {
  public:
    static constexpr std::size_t role_count = 3;
    // A foreground helper invocation is itself hard-bounded. Once background
    // work is ready, admit at most one such invocation before servicing it.
    static constexpr std::size_t max_consecutive_foreground_before_background = 1;

    struct foreground_work_t {
      std::function<void(std::stop_token)> run;
      std::function<void()> supersede;
    };
    using background_step_t = std::function<bool(std::stop_token)>;
    using worker_factory_t = std::function<std::jthread(
      std::size_t,
      std::function<void(std::stop_token)>
    )>;

    fixed_role_lane_coordinator_t();
    explicit fixed_role_lane_coordinator_t(worker_factory_t worker_factory);
    ~fixed_role_lane_coordinator_t();
    fixed_role_lane_coordinator_t(const fixed_role_lane_coordinator_t &) = delete;
    fixed_role_lane_coordinator_t &operator=(const fixed_role_lane_coordinator_t &) = delete;

    bool submit_foreground(std::size_t role_index, foreground_work_t work);
    std::optional<std::size_t> current_lane_index() const noexcept;

    template<class Result, class Callback>
    Result invoke_foreground(
      std::size_t role_index,
      Callback &&callback,
      const Result &internal_failure_result,
      const Result &superseded_result
    ) {
      try {
        if (current_lane_index().has_value()) {
          return internal_failure_result;
        }
        auto completion = std::make_shared<std::promise<Result>>();
        auto completed = completion->get_future();
        auto fulfill = [completion](Result result) noexcept {
          try {
            completion->set_value(std::move(result));
          } catch (...) {
          }
        };
        Result run_failure {internal_failure_result};
        Result supersede_failure {superseded_result};
        const auto accepted = submit_foreground(role_index, {
          [callback = std::forward<Callback>(callback),
           fulfill,
           run_failure = std::move(run_failure)](
            std::stop_token stop_token
          ) mutable {
            try {
              fulfill(callback(stop_token));
            } catch (...) {
              fulfill(std::move(run_failure));
            }
          },
          [fulfill,
           supersede_failure = std::move(supersede_failure)]() mutable {
            fulfill(std::move(supersede_failure));
          },
        });
        if (!accepted) {
          return superseded_result;
        }
        return completed.get();
      } catch (...) {
        return internal_failure_result;
      }
    }

    bool set_observer_step(std::size_t role_index, background_step_t step);
    bool ensure_observer_step(std::size_t role_index, background_step_t step);
    restore_slot_install_result_e set_restore_step(
      std::size_t role_index,
      restore_slot_key_t key,
      background_step_t step
    );
    void clear_observer_step(std::size_t role_index);
    bool clear_restore_step(std::size_t role_index, restore_slot_key_t key);
    std::size_t clear_restore_steps_before(restore_slot_key_t threshold);
    void suspend_dispatch() noexcept;
    void resume_dispatch() noexcept;
    void shutdown() noexcept;
    std::size_t worker_capacity() const noexcept;
    bool is_current_lane(std::size_t role_index) const noexcept;
    bool has_pending_foreground(std::size_t role_index) const noexcept;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  class fixed_role_lane_runtime_t {
  public:
    using coordinator_factory_t =
      std::function<std::shared_ptr<fixed_role_lane_coordinator_t>()>;
    using runtime_created_callback_t = std::function<void(
      const std::shared_ptr<fixed_role_lane_coordinator_t> &,
      std::uint64_t
    )>;

    fixed_role_lane_runtime_t();
    explicit fixed_role_lane_runtime_t(
      coordinator_factory_t coordinator_factory,
      runtime_created_callback_t runtime_created_callback = {}
    );
    ~fixed_role_lane_runtime_t();
    fixed_role_lane_runtime_t(const fixed_role_lane_runtime_t &) = delete;
    fixed_role_lane_runtime_t &operator=(const fixed_role_lane_runtime_t &) = delete;

    bool activate();
    bool release() noexcept;
    std::shared_ptr<fixed_role_lane_coordinator_t> get(
      bool *created = nullptr,
      std::uint64_t *runtime_generation = nullptr
    );
    bool rotate(std::uint64_t *runtime_generation = nullptr);
    std::uint64_t generation() const noexcept;
    bool quiescing() const noexcept;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  bool is_steam_streaming_render_adapter(std::string_view adapter_name);
  render_endpoint_catalog_t build_render_endpoint_catalog(
    bool discovery_complete,
    const std::vector<render_endpoint_t> &endpoints
  );
  bool is_eligible_non_steam_fallback(
    const render_endpoint_catalog_t &catalog,
    const std::string &device_id
  );
  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids
  );
  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids,
    const std::vector<std::string> &excluded_ids
  );
  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const std::vector<render_endpoint_t> &endpoints,
    const std::vector<std::string> &preferred_ids
  );
  std::vector<bool> steam_owned_role_mask(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids
  );
  std::vector<owned_role_snapshot_t> steam_owned_roles_from_snapshot(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids
  );
  bool role_restore_still_owned(
    const owned_role_snapshot_t &owned_role,
    const std::string &current_id
  );
  std::optional<std::vector<std::string>> sanitize_captured_role_ids(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids,
    const std::string &eligible_non_steam_replacement_id
  );
  std::vector<role_restore_plan_t> plan_steam_role_restores(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &target_role_ids
  );
  fallback_role_transition_t plan_fallback_role_transition(
    const render_endpoint_catalog_t &catalog,
    const std::string &expected_steam_id,
    const std::string &current_id,
    const std::string &selected_fallback_id
  );
  fallback_commit_transition_t complete_fallback_role_transition(
    const std::string &committed_expected_id,
    const std::string &inflight_target_id,
    bool set_succeeded,
    const std::optional<std::string> &readback_id
  );
  std::optional<std::string> superseded_assignment_repair_target(
    const std::string &superseded_side_effect_target_id,
    const std::string &live_id,
    const std::string &active_epoch_owned_target_id
  );
  superseded_write_observation_transition_t observe_superseded_write(
    const std::vector<std::string> &stale_target_ids,
    const std::vector<std::string> &tolerated_live_ids,
    const std::string &live_id,
    const std::string &active_epoch_owned_target_id,
    bool active_epoch_entitled,
    bool cancelled
  );
  bool policy_write_requires_settling(
    bool set_succeeded,
    bool assignment_still_current
  );
  bool activation_policy_write_requires_settling(
    bool set_succeeded,
    bool assignment_still_current,
    bool exact_target_readback_confirmed
  );
  bool policy_write_receipt_is_pending(
    std::uint64_t receipt_assignment_epoch,
    std::uint64_t active_assignment_epoch,
    std::uint64_t receipt_role_revision,
    std::uint64_t active_role_revision,
    bool repair_hazard,
    const std::string &issuer_active_id,
    const std::string &active_owned_id
  );
  bool policy_observer_owner_changed(
    const policy_observer_owner_key_t &observed,
    const policy_observer_owner_key_t &current
  );
  causal_repair_transition_t plan_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick,
    bool stale_target_is_live,
    unsigned max_attempts
  );
  causal_repair_chain_selection_t select_causal_repair_chain(
    const std::vector<causal_repair_chain_candidate_t> &chains,
    const std::string &stale_target_id,
    std::uint64_t now_tick,
    unsigned max_attempts
  );
  causal_repair_state_t complete_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick,
    bool set_succeeded,
    bool exact_target_readback,
    std::uint64_t retry_backoff_ticks
  );
  causal_repair_state_t retract_unissued_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick
  );
  std::uint64_t causal_repair_deadline_after_generation(
    std::uint64_t original_deadline_tick,
    std::uint64_t proposed_deadline_tick,
    bool repair_generation
  );
  policy_receipt_capacity_decision_t decide_policy_receipt_capacity(
    std::size_t receipt_count,
    std::size_t receipt_limit,
    const std::vector<bool> &settled_receipts
  );
  policy_write_execution_action_e classify_policy_write_execution(
    bool may_have_executed,
    bool precondition_proved_no_write,
    bool pre_read_proved_no_write
  );
  bool authenticated_policy_pre_read_proves_no_write(
    const policy_pre_read_proof_t &proof
  );
  superseded_write_observer_progress_t advance_superseded_write_observer(
    bool generation_changed,
    bool deadline_expired,
    bool read_succeeded,
    bool observation_state_unchanged,
    bool kept_observing,
    int stable_observations,
    int required_stable_observations
  );
  superseded_write_ledger_phase_t advance_superseded_write_ledger_after_external_adoption(
    const std::vector<std::string> &stale_target_ids,
    const std::string &previous_active_target_id,
    const std::string &external_id
  );
  pending_role_ownership_action_e classify_pending_role_ownership(
    const render_endpoint_catalog_t &catalog,
    const std::string &expected_current_id,
    const std::optional<std::string> &live_id
  );
  default_endpoint_observation_e classify_default_endpoint_observation(
    const std::string &expected_current_id,
    const std::optional<std::string> &live_id
  );
  restore_external_observation_action_e classify_restore_external_observation(
    const std::string &live_id,
    const std::vector<std::string> &pending_target_ids,
    const std::vector<std::string> &stale_target_ids
  );
  bool should_attempt_role_fallback(
    bool fallback_requested,
    bool has_ambiguous_inflight_write
  );
  worker_bootstrap_action_e worker_bootstrap_action(
    bool assignment_active,
    bool initialization_succeeded
  );
  worker_role_write_action_e worker_role_write_action(
    bool worker_active,
    bool role_assignment_still_current
  );
  fallback_catalog_action_e fallback_catalog_action(const render_endpoint_catalog_t &catalog);
  bool should_queue_post_install_unknown_role(
    bool current_read_known,
    const std::string &old_id
  );

  enum class sample_status_e {
    ok,
    timeout,
    reinitialize,
    interrupted,
    error,
  };

  enum class sample_action_e {
    emit,
    retry,
    reacquire,
    stop,
  };

  sample_action_e sample_action(sample_status_e status);

  class sample_source_t {
  public:
    virtual ~sample_source_t() = default;
    virtual sample_status_e sample() = 0;
    virtual bool reacquire() = 0;
  };

  struct capture_summary_t {
    std::size_t emitted = 0;
    std::size_t timeouts = 0;
    std::size_t reacquisitions = 0;
    bool stopped = false;
  };

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit);
}  // namespace audio::policy
