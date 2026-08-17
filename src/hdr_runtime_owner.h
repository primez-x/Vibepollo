/**
 * @file src/hdr_runtime_owner.h
 * @brief Thread-safe ownership and conditional rollback for runtime overrides.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace hdr_runtime_owner {

  // This is the same shape used by config::runtime_config_overrides_snapshot().
  using override_map_t = std::unordered_map<std::string, std::string>;

  struct candidate_transaction_t {
    std::uint64_t generation = 0;
  };

  /**
   * A token identifies one pending participant in one candidate generation.
   * The first token returned by apply_candidate is the owner token; joined
   * participants use the same generation with a distinct participant id.
   */
  struct lease_token_t {
    std::uint64_t generation = 0;
    std::uint64_t participant = 0;

    friend bool operator==(const lease_token_t &, const lease_token_t &) = default;
  };

  enum class phase_e {
    provisional,
    awaiting_stream,
    active,
    retained_paused,
  };

  enum class commit_status_e {
    committed,
    already_committed,
    stale,
  };

  enum class cancellation_status_e {
    rolled_back,
    deferred,
    committed,
    stale,
  };

  struct lease_t {
    lease_token_t token;
    std::uint64_t map_revision = 0;
    override_map_t candidate_map;
  };

  struct cancellation_result_t {
    cancellation_status_e status = cancellation_status_e::stale;
    std::uint64_t map_revision = 0;
    std::optional<override_map_t> replacement_map;
  };

  struct state_t {
    phase_e phase = phase_e::provisional;
    std::uint64_t generation = 0;
    std::uint64_t map_revision = 0;
    std::size_t pending_participants = 0;
    std::optional<lease_token_t> owner_token;
    // The complete map selected by this owner.  Keeping it in the manager
    // makes active-session inheritance and paused-owner restoration observable
    // without consulting a mutable config singleton.
    override_map_t resolved_overrides;
    std::optional<override_map_t> prior_owner_overrides;
    bool inherited_from_prior_owner = false;
  };

  /**
   * Owns the metadata for one process-wide runtime-override candidate.
   *
   * The manager never calls a global config setter. Callers must snapshot and
   * publish the complete map while holding the existing lifecycle gate, then
   * pass the complete current snapshot to cancel(). The manager mutex protects
   * generations, tokens, deltas, and phases; callers remain responsible for
   * serializing the external map snapshot/publish pair with that lifecycle
   * gate. Keys and values are compared exactly; no case or whitespace
   * normalization is applied.
   */
  class manager final {
  public:
    manager() = default;
    manager(const manager &) = delete;
    manager &operator=(const manager &) = delete;

    /** Start a generation from a complete prior map. */
    candidate_transaction_t begin_candidate(const override_map_t &prior_map);

    /**
     * Record the complete candidate map and create its first awaiting-stream
     * participant. A transaction from an older generation is rejected.
     */
    std::optional<lease_t> apply_candidate(
      const candidate_transaction_t &transaction,
      const override_map_t &candidate_map);

    /** Update the current awaiting-stream candidate before publication. */
    bool update_candidate(
      const lease_token_t &token,
      const override_map_t &candidate_map);

    /** Join the current awaiting-stream cohort with a new participant token. */
    std::optional<lease_token_t> add_participant(const lease_token_t &token);

    /** Publish the candidate owner when any cohort participant starts a stream. */
    commit_status_e commit(const lease_token_t &token);

    /** Retain an active owner while its application is paused. */
    void retain_paused();

    /** Clear all ownership metadata during application/process termination. */
    void terminate();

    /**
     * Cancel one participant. Rollback is returned only after the final
     * awaiting participant is cancelled. The returned map is a caller-owned
     * replacement and must be published under the lifecycle gate.
     */
    cancellation_result_t cancel(
      const lease_token_t &token,
      const override_map_t &current_map);

    /** Return a copy of the current ownership state, if any. */
    std::optional<state_t> state() const;

    /** Invalidate a lease because an unrelated runtime-map writer won. */
    void invalidate_external();

    /** Test whether this participant may publish its pending candidate. */
    bool accepts_candidate(const lease_token_t &token) const;

    /** Return the complete map owned by the active/retained cohort. */
    std::optional<override_map_t> current_owner_overrides() const;

  private:
    struct key_delta_t {
      std::optional<std::string> prior;
      std::optional<std::string> candidate;
    };

    struct owner_state_t {
      std::uint64_t generation = 0;
      lease_token_t owner_token;
      std::uint64_t map_revision = 0;
      override_map_t candidate_map;
      std::unordered_map<std::string, key_delta_t> deltas;
      std::unordered_map<std::uint64_t, bool> participants;
      phase_e phase = phase_e::retained_paused;
    };

    struct cohort_t {
      std::uint64_t generation = 0;
      lease_token_t owner_token;
      std::uint64_t map_revision = 0;
      override_map_t prior_map;
      override_map_t candidate_map;
      std::unordered_map<std::string, key_delta_t> deltas;
      std::unordered_map<std::uint64_t, bool> participants;
      phase_e phase = phase_e::awaiting_stream;
      std::optional<owner_state_t> prior_owner;
    };

    struct provisional_t {
      candidate_transaction_t transaction;
      override_map_t prior_map;
      std::optional<owner_state_t> prior_owner;
    };

    static std::optional<std::string> value_for(
      const override_map_t &map,
      const std::string &key);

    static std::unordered_map<std::string, key_delta_t> build_deltas(
      const override_map_t &prior_map,
      const override_map_t &candidate_map);

    static override_map_t conditionally_rollback(
      const override_map_t &current_map,
      const std::unordered_map<std::string, key_delta_t> &deltas);

    static owner_state_t snapshot_owner(const cohort_t &cohort);
    static cohort_t restore_owner(owner_state_t owner);

    mutable std::mutex mutex_;
    std::uint64_t next_generation_ = 1;
    std::uint64_t next_participant_ = 1;
    std::uint64_t map_revision_ = 0;
    std::optional<provisional_t> provisional_;
    std::optional<cohort_t> cohort_;
  };

  // Process-wide owner used by launch/resume and the asynchronous stream
  // publication callbacks. The manager itself remains the only authority for
  // generation and per-key rollback metadata; callers still publish maps
  // under the existing stream lifecycle gate.
  manager &global_manager();

  // All production runtime-map publication goes through these functions. An
  // external publication invalidates any provisional/active HDR lease so a
  // stale startup callback cannot restore over the newer map. Candidate
  // publication is accepted only for the current awaiting-stream participant.
  override_map_t runtime_overrides_snapshot();
  void publish_external_runtime_overrides(const override_map_t &overrides);
  // Publish a manager-produced rollback without invalidating the restored
  // paused owner. Callers must already hold stream_lifecycle_mutex().
  void publish_rollback_runtime_overrides(const override_map_t &overrides);
  bool publish_candidate_runtime_overrides(
    const lease_token_t &token,
    const override_map_t &overrides
  );
  void clear_runtime_overrides();

}  // namespace hdr_runtime_owner
