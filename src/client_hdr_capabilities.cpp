/**
 * @file src/client_hdr_capabilities.cpp
 * @brief Bounded host parsing for v1 client HDR display capabilities.
 */

#include "client_hdr_capabilities.h"

#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {
  using json = nlohmann::json;

  int base64url_value(const char value) {
    if (value >= 'A' && value <= 'Z') {
      return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
      return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
      return value - '0' + 52;
    }
    if (value == '-') {
      return 62;
    }
    if (value == '_') {
      return 63;
    }
    return -1;
  }

  std::optional<std::string> decode_base64url(const std::string_view encoded) {
    if (encoded.size() > client_hdr::max_encoded_value_bytes) {
      return std::nullopt;
    }

    std::size_t padding = 0;
    while (padding < encoded.size() && encoded[encoded.size() - padding - 1] == '=') {
      ++padding;
    }
    if (padding > 2) {
      return std::nullopt;
    }

    const auto unpadded_size = encoded.size() - padding;
    const auto remainder = unpadded_size % 4;
    if (remainder == 1) {
      return std::nullopt;
    }
    if (padding != 0 && padding != (4 - remainder) % 4) {
      return std::nullopt;
    }

    for (std::size_t index = 0; index < unpadded_size; ++index) {
      if (base64url_value(encoded[index]) < 0) {
        return std::nullopt;
      }
    }

    std::string decoded;
    decoded.reserve((unpadded_size / 4) * 3 + (remainder == 0 ? 0 : remainder - 1));
    std::uint32_t accumulator = 0;
    unsigned accumulated_bits = 0;
    for (std::size_t index = 0; index < unpadded_size; ++index) {
      accumulator = (accumulator << 6u) | static_cast<std::uint32_t>(base64url_value(encoded[index]));
      accumulated_bits += 6;
      while (accumulated_bits >= 8) {
        accumulated_bits -= 8;
        decoded.push_back(static_cast<char>((accumulator >> accumulated_bits) & 0xffu));
        if (decoded.size() > client_hdr::max_decoded_json_bytes) {
          return std::nullopt;
        }
        if (accumulated_bits == 0) {
          accumulator = 0;
        } else {
          accumulator &= (1u << accumulated_bits) - 1u;
        }
      }
    }

    if (accumulated_bits != 0 && (accumulator & ((1u << accumulated_bits) - 1u)) != 0) {
      return std::nullopt;
    }
    return decoded;
  }

  enum class source_object_e {
    other,
    calibrated,
    display_reported,
  };

  struct object_state_t {
    source_object_e kind {source_object_e::other};
    bool source_seen = false;
  };

  class source_duplicate_detector final : public nlohmann::json_sax<json> {
  public:
    bool duplicate_source = false;

    bool null() override {
      pending_key_.clear();
      return true;
    }

    bool boolean(bool) override {
      pending_key_.clear();
      return true;
    }

    bool number_integer(number_integer_t) override {
      pending_key_.clear();
      return true;
    }

    bool number_unsigned(number_unsigned_t) override {
      pending_key_.clear();
      return true;
    }

    bool number_float(number_float_t, const string_t &) override {
      pending_key_.clear();
      return true;
    }

    bool string(string_t &) override {
      pending_key_.clear();
      return true;
    }

    bool binary(binary_t &) override {
      pending_key_.clear();
      return true;
    }

    bool start_object(std::size_t) override {
      source_object_e kind = source_object_e::other;
      if (pending_key_ == "calibrated") {
        kind = source_object_e::calibrated;
      } else if (pending_key_ == "edid") {
        kind = source_object_e::display_reported;
      }
      object_stack_.push_back({kind, false});
      pending_key_.clear();
      return true;
    }

    bool key(string_t &value) override {
      if (!object_stack_.empty() && value == "source") {
        auto &object = object_stack_.back();
        if (object.kind == source_object_e::calibrated || object.kind == source_object_e::display_reported) {
          if (object.source_seen) {
            duplicate_source = true;
          }
          object.source_seen = true;
        }
      }
      pending_key_ = value;
      return true;
    }

    bool end_object() override {
      if (!object_stack_.empty()) {
        object_stack_.pop_back();
      }
      pending_key_.clear();
      return true;
    }

    bool start_array(std::size_t) override {
      pending_key_.clear();
      return true;
    }

    bool end_array() override {
      pending_key_.clear();
      return true;
    }

    bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
      return false;
    }

  private:
    std::vector<object_state_t> object_stack_;
    std::string pending_key_;
  };

  enum class source_parse_status_e {
    absent,
    invalid,
    unsupported,
    valid,
  };

  struct source_parse_result_t {
    source_parse_status_e status {source_parse_status_e::absent};
    std::optional<client_hdr::peak_t> peak;
  };

  std::optional<std::uint32_t> normalize_peak(const json &value) {
    if (!value.is_number()) {
      return std::nullopt;
    }

    const auto raw = value.get<double>();
    if (!std::isfinite(raw) || raw < client_hdr::minimum_peak_luminance_nits ||
        raw > client_hdr::maximum_peak_luminance_nits) {
      return std::nullopt;
    }

    const auto rounded = std::round(raw);
    if (!std::isfinite(rounded) || rounded < client_hdr::minimum_peak_luminance_nits ||
        rounded > client_hdr::maximum_peak_luminance_nits) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(rounded);
  }

  source_parse_result_t parse_source(
    const json &root,
    const std::string_view member,
    const std::string_view expected_source,
    const bool duplicate_source
  ) {
    const auto iterator = root.find(member);
    if (iterator == root.end()) {
      return {};
    }
    if (duplicate_source || !iterator->is_object()) {
      return {source_parse_status_e::invalid, std::nullopt};
    }

    const auto source_iterator = iterator->find("source");
    const auto peak_iterator = iterator->find("peak_luminance_nits");
    if (source_iterator == iterator->end() || peak_iterator == iterator->end() || !source_iterator->is_string()) {
      return {source_parse_status_e::invalid, std::nullopt};
    }
    if (source_iterator->get<std::string>() != expected_source) {
      return {source_parse_status_e::unsupported, std::nullopt};
    }

    const auto peak = normalize_peak(*peak_iterator);
    if (!peak) {
      return {source_parse_status_e::invalid, std::nullopt};
    }
    return {
      source_parse_status_e::valid,
      client_hdr::peak_t {*peak, std::string(expected_source)},
    };
  }

  client_hdr::parse_result_t invalid_result() {
    return {client_hdr::parse_status_e::invalid, std::nullopt};
  }

  client_hdr::parse_result_t unsupported_result() {
    return {client_hdr::parse_status_e::unsupported, std::nullopt};
  }
}  // namespace

namespace client_hdr {
  parse_result_t parse(const std::string_view encoded_value) {
    if (encoded_value.empty()) {
      return {parse_status_e::absent, std::nullopt};
    }

    try {
      const auto decoded = decode_base64url(encoded_value);
      if (!decoded) {
        return invalid_result();
      }

      source_duplicate_detector detector;
      if (!json::sax_parse(*decoded, &detector) || detector.duplicate_source) {
        return invalid_result();
      }

      const auto root = json::parse(*decoded, nullptr, false);
      if (root.is_discarded() || !root.is_object()) {
        return invalid_result();
      }

      const auto version_iterator = root.find("version");
      const auto platform_iterator = root.find("platform");
      if (version_iterator == root.end() || platform_iterator == root.end()) {
        return invalid_result();
      }
      if ((!version_iterator->is_number_integer() && !version_iterator->is_number_unsigned()) ||
          !platform_iterator->is_string()) {
        return invalid_result();
      }

      const bool version_one =
        (version_iterator->is_number_integer() && version_iterator->get<json::number_integer_t>() == 1) ||
        (version_iterator->is_number_unsigned() && version_iterator->get<json::number_unsigned_t>() == 1);
      if (!version_one) {
        return unsupported_result();
      }
      if (platform_iterator->get<std::string>() != "windows-desktop") {
        return unsupported_result();
      }

      const auto calibrated = parse_source(
        root,
        "calibrated",
        calibrated_source,
        false
      );
      const auto display_reported = parse_source(
        root,
        "edid",
        display_reported_source,
        false
      );

      if (calibrated.status == source_parse_status_e::invalid ||
          display_reported.status == source_parse_status_e::invalid) {
        if (!calibrated.peak && !display_reported.peak) {
          return invalid_result();
        }
      }
      if (calibrated.status == source_parse_status_e::unsupported ||
          display_reported.status == source_parse_status_e::unsupported) {
        if (!calibrated.peak && !display_reported.peak) {
          return unsupported_result();
        }
      }

      capabilities_t result;
      result.calibrated = calibrated.peak;
      result.display_reported = display_reported.peak;
      return {parse_status_e::valid, std::move(result)};
    } catch (...) {
      return invalid_result();
    }
  }
}  // namespace client_hdr
