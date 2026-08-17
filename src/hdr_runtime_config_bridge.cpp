/**
 * @file src/hdr_runtime_config_bridge.cpp
 * @brief Production bridge between runtime HDR ownership and config storage.
 */
#include "hdr_runtime_owner.h"
#include "config.h"

namespace hdr_runtime_owner {

  override_map_t runtime_overrides_snapshot() {
    return config::runtime_config_overrides_snapshot();
  }

  void publish_external_runtime_overrides(const override_map_t &overrides) {
    global_manager().invalidate_external();
    config::set_runtime_config_overrides(overrides);
  }

  void publish_rollback_runtime_overrides(const override_map_t &overrides) {
    config::set_runtime_config_overrides(overrides);
  }

  bool publish_candidate_runtime_overrides(
    const lease_token_t &token,
    const override_map_t &overrides
  ) {
    if (!global_manager().accepts_candidate(token)) {
      return false;
    }
    config::set_runtime_config_overrides(overrides);
    return true;
  }

  void clear_runtime_overrides() {
    global_manager().terminate();
    config::clear_runtime_config_overrides();
  }

}  // namespace hdr_runtime_owner
