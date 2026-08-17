#include "http_request_view.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace {

constexpr std::size_t kMaxRawCapabilityValueBytes = 12288;
constexpr std::string_view kCapabilityKey = "clientDisplayCapabilities";

int hex_value(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::optional<std::string> percent_decode_strict(std::string_view raw) {
  std::string decoded;
  decoded.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (raw[i] != '%') {
      decoded.push_back(raw[i]);
      continue;
    }
    if (i + 2 >= raw.size()) return std::nullopt;
    const int high = hex_value(raw[i + 1]);
    const int low = hex_value(raw[i + 2]);
    if (high < 0 || low < 0) return std::nullopt;
    decoded.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }
  return decoded;
}

std::string percent_decode_for_query(std::string_view raw) {
  std::string decoded;
  decoded.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (raw[i] == '%' && i + 2 < raw.size()) {
      const int high = hex_value(raw[i + 1]);
      const int low = hex_value(raw[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>((high << 4) | low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(raw[i]);
  }
  return decoded;
}

// Used only to classify a possibly malformed capability key. Invalid escape
// triplets are discarded so a key such as
// `clientDisplay%ZZCapabilities` is still removed from every downstream
// parser/logger, while an unrelated suffix remains unrelated.
std::string percent_decode_for_key_classification(std::string_view raw) {
  std::string decoded;
  decoded.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (raw[i] != '%') {
      decoded.push_back(raw[i]);
      continue;
    }
    if (i + 2 >= raw.size()) {
      continue;
    }
    const int high = hex_value(raw[i + 1]);
    const int low = hex_value(raw[i + 2]);
    if (high >= 0 && low >= 0) {
      decoded.push_back(static_cast<char>((high << 4) | low));
    }
    i += 2;
  }
  return decoded;
}

bool equal_case_insensitive(std::string_view left, std::string_view right) {
  if (left.size() != right.size()) return false;
  return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
  });
}

}  // namespace

http::client_hdr::request_query_view http::client_hdr::sanitize_request_query(
  const std::string_view raw_query
) {
  request_query_view result;
  std::size_t offset = 0;
  std::size_t capability_count = 0;
  bool invalid_capability = false;

  while (offset <= raw_query.size()) {
    const std::size_t ampersand = raw_query.find('&', offset);
    const std::size_t end = ampersand == std::string_view::npos ? raw_query.size() : ampersand;
    const std::string_view segment = raw_query.substr(offset, end - offset);
    const std::size_t equals = segment.find('=');
    const std::string_view raw_key = equals == std::string_view::npos ? segment : segment.substr(0, equals);
    const std::string_view raw_value = equals == std::string_view::npos ? std::string_view {} : segment.substr(equals + 1);

    const auto decoded_key = percent_decode_strict(raw_key);
    const auto classified_key = percent_decode_for_key_classification(raw_key);
    const bool is_capability =
      (decoded_key.has_value() && equal_case_insensitive(*decoded_key, kCapabilityKey)) ||
      (!decoded_key.has_value() &&
       equal_case_insensitive(classified_key, kCapabilityKey));
    if (is_capability) {
      ++capability_count;
      if (capability_count > 1) {
        invalid_capability = true;
      }
      // Reject the raw transport value before percent-decoding or allocating
      // a decoded copy.  This is the request-boundary allocation guard; the
      // decoded base64 and JSON limits are enforced by the capability parser.
      if (raw_value.size() > kMaxRawCapabilityValueBytes ||
          !decoded_key.has_value()) {
        invalid_capability = true;
      } else if (capability_count == 1) {
        const auto decoded_value = percent_decode_strict(raw_value);
        if (!decoded_value.has_value() || decoded_value->empty() ||
            decoded_value->size() > http::client_hdr::kMaxDecodedBase64ValueBytes) {
          invalid_capability = true;
        } else {
          result.encoded_value = std::move(*decoded_value);
        }
      }
    } else {
      if (!result.sanitized_query.empty()) result.sanitized_query.push_back('&');
      result.sanitized_query.append(segment);
      result.parameters.emplace_back(
        percent_decode_for_query(raw_key),
        percent_decode_for_query(raw_value)
      );
    }

    if (ampersand == std::string_view::npos) break;
    offset = ampersand + 1;
  }

  if (capability_count == 0) {
    result.status = capability_status::absent;
    return result;
  }
  if (invalid_capability) {
    result.encoded_value.reset();
    result.status = capability_status::invalid;
    return result;
  }
  result.status = capability_status::valid;
  return result;
}
