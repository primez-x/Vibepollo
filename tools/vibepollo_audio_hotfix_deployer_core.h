#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vibepollo::audio_hotfix {

using hash256_t = std::array<std::uint8_t, 32>;

constexpr std::size_t k_max_scm_baseline_bytes = 64U * 1024U;
constexpr std::size_t k_max_receipt_bytes = 128U * 1024U;

template<class value_t>
struct result_t {
  value_t value {};
  std::string error;

  [[nodiscard]] bool ok() const noexcept {
    return error.empty();
  }

  static result_t success(value_t value) {
    return {.value = std::move(value), .error = {}};
  }

  static result_t failure(std::string error) {
    return {.value = {}, .error = std::move(error)};
  }
};

struct file_identity_t {
  std::uint64_t volume_serial {};
  std::array<std::uint8_t, 16> file_id {};

  bool operator==(const file_identity_t &) const = default;
};

struct receipt_t {
  std::string transaction_id;
  hash256_t expected_original_hash {};
  hash256_t candidate_main_hash {};
  hash256_t candidate_helper_hash {};
  hash256_t original_helper_hash {};
  hash256_t service_executable_hash {};
  hash256_t baseline_digest {};
  file_identity_t original_main_identity;
  file_identity_t original_main_snapshot_identity;
  file_identity_t original_helper_identity;
  file_identity_t service_executable_identity;
  file_identity_t candidate_main_identity;
  file_identity_t candidate_helper_identity;
  std::uint64_t original_main_size {};
  std::uint64_t original_helper_size {};
  std::uint64_t service_executable_size {};
  std::uint64_t candidate_main_size {};
  std::uint64_t candidate_helper_size {};
  bool helper_originally_present {};
  std::uint64_t created_filetime {};
  std::vector<std::uint8_t> scm_baseline;

  bool operator==(const receipt_t &) const = default;
};

enum class field_availability_t : std::uint8_t {
  unsupported = 0,
  present = 1,
};

struct optional_blob_t {
  field_availability_t availability {field_availability_t::unsupported};
  std::vector<std::uint8_t> value;

  bool operator==(const optional_blob_t &) const = default;
};

struct scm_semantic_baseline_t {
  std::string account;
  std::string binary_path;
  std::uint32_t service_type {};
  std::uint32_t start_type {};
  std::uint32_t error_control {};
  std::vector<std::string> dependencies;
  optional_blob_t failure_actions;
  optional_blob_t failure_actions_flag;
  optional_blob_t delayed_start;
  optional_blob_t sid_type;
  optional_blob_t required_privileges;
  optional_blob_t preshutdown_timeout;
  optional_blob_t triggers;
  optional_blob_t preferred_node;
  optional_blob_t managed_account;
  optional_blob_t launch_protection;
  std::vector<std::uint8_t> service_security_owner_dacl;
  std::vector<std::uint8_t> registry_security_owner_dacl;

  bool operator==(const scm_semantic_baseline_t &) const = default;
};

struct space_inputs_t {
  std::uint64_t original_main_bytes {};
  std::uint64_t original_helper_bytes {};
  std::uint64_t candidate_main_bytes {};
  std::uint64_t candidate_helper_bytes {};
  std::uint64_t receipt_bytes {};
  std::uint64_t safety_margin_bytes {};
};

enum class artifact_content_t : std::uint8_t {
  absent,
  original,
  candidate,
  other,
};

struct artifact_observation_t {
  artifact_content_t content {artifact_content_t::absent};
  bool trusted_acl {};
  bool same_volume {};
  bool no_reparse_points {};
  bool single_link {};
  bool no_extra_streams {};
  bool identity_matches_receipt {};
};

enum class receipt_presence_t : std::uint8_t {
  absent,
  valid,
  invalid,
};

enum class service_health_t : std::uint8_t {
  stopped,
  healthy_original,
  healthy_candidate,
  running_unverified,
};

struct physical_observation_t {
  receipt_presence_t receipt {receipt_presence_t::absent};
  bool namespace_exact {};
  bool scm_baseline_matches {};
  artifact_observation_t main_target;
  artifact_observation_t helper_target;
  artifact_observation_t original_snapshot;
  artifact_observation_t helper_snapshot;
  artifact_observation_t live_backup;
  artifact_observation_t failed_main;
  artifact_observation_t failed_helper;
  service_health_t service_health {service_health_t::stopped};
};

enum class physical_state_t : std::uint8_t {
  unknown,
  pristine_original_stopped,
  pristine_original_healthy,
  receipt_ready_original_running,
  receipt_ready_original_stopped,
  deployed_stopped,
  deployed_healthy,
  rollback_required,
  rolled_back_stopped,
  rolled_back_healthy,
  unsafe,
};

[[nodiscard]] hash256_t sha256(const std::vector<std::uint8_t> &bytes) noexcept;
[[nodiscard]] std::string hash_to_hex(const hash256_t &hash);
[[nodiscard]] std::optional<hash256_t> hash_from_hex(const std::string &text) noexcept;
[[nodiscard]] bool valid_transaction_id(const std::string &transaction_id) noexcept;

[[nodiscard]] result_t<std::vector<std::uint8_t>> serialize_receipt(const receipt_t &receipt);
[[nodiscard]] result_t<receipt_t> parse_receipt(const std::vector<std::uint8_t> &bytes);
[[nodiscard]] result_t<std::vector<std::uint8_t>> serialize_scm_baseline(
  const scm_semantic_baseline_t &baseline
);
[[nodiscard]] result_t<hash256_t> digest_scm_baseline(const scm_semantic_baseline_t &baseline);
[[nodiscard]] std::optional<std::uint64_t> required_free_space(const space_inputs_t &input) noexcept;
[[nodiscard]] physical_state_t classify_physical_state(const physical_observation_t &observation) noexcept;

}  // namespace vibepollo::audio_hotfix
