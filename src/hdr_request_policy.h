#pragma once

#include <optional>
#include <string_view>

namespace hdr::request_policy {

enum class override_e {
  automatic,
  force_on,
  force_off,
};

struct inputs_t {
  bool client_hdr_requested = false;
  bool prefer_sdr_10bit = false;
  bool force_sdr = false;
  override_e request_override = override_e::automatic;
};

struct result_t {
  bool enable_hdr = false;
  bool prefer_sdr_10bit = false;
  bool force_sdr = false;

  bool effective_hdr_requested() const {
    return enable_hdr && !prefer_sdr_10bit && !force_sdr;
  }
};

std::optional<override_e> parse_override(std::string_view value);
result_t resolve(const inputs_t &inputs);

}  // namespace hdr::request_policy
