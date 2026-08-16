#include "tools/vibepollo_audio_hotfix_deployer_core.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <limits>
#include <span>

namespace vibepollo::audio_hotfix {
namespace {

constexpr std::array<std::uint8_t, 8> receipt_magic {'V', 'A', 'H', 'F', 'R', 'E', 'C', '1'};
constexpr std::array<std::uint8_t, 8> scm_magic {'V', 'A', 'H', 'F', 'S', 'C', 'M', '1'};
constexpr std::uint32_t format_version = 1;
constexpr std::size_t receipt_digest_bytes = 32;
constexpr std::size_t transaction_id_bytes = 36;

class writer_t {
 public:
  explicit writer_t(const std::size_t maximum): maximum_(maximum) {}

  bool append_u8(const std::uint8_t value) {
    return append(std::span<const std::uint8_t>(&value, 1));
  }

  bool append_u32(const std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes {};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
    }
    return append(bytes);
  }

  bool append_u64(const std::uint64_t value) {
    std::array<std::uint8_t, 8> bytes {};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
    }
    return append(bytes);
  }

  template<std::size_t count>
  bool append_array(const std::array<std::uint8_t, count> &bytes) {
    return append(bytes);
  }

  bool append_string(const std::string &text) {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(text.size())) &&
           append(std::as_bytes(std::span(text)));
  }

  bool append_blob(const std::vector<std::uint8_t> &blob) {
    if (blob.size() > std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    return append_u32(static_cast<std::uint32_t>(blob.size())) && append(blob);
  }

  bool append(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > maximum_ - std::min(maximum_, bytes_.size())) {
      return false;
    }
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    return true;
  }

  bool append(std::span<const std::byte> bytes) {
    return append({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
  }

  [[nodiscard]] std::vector<std::uint8_t> take() {
    return std::move(bytes_);
  }

 private:
  std::size_t maximum_;
  std::vector<std::uint8_t> bytes_;
};

class reader_t {
 public:
  explicit reader_t(const std::span<const std::uint8_t> bytes): bytes_(bytes) {}

  bool read_u8(std::uint8_t &value) {
    if (remaining() < 1) {
      return false;
    }
    value = bytes_[position_++];
    return true;
  }

  bool read_u32(std::uint32_t &value) {
    if (remaining() < 4) {
      return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      value |= static_cast<std::uint32_t>(bytes_[position_++]) << (i * 8U);
    }
    return true;
  }

  bool read_u64(std::uint64_t &value) {
    if (remaining() < 8) {
      return false;
    }
    value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      value |= static_cast<std::uint64_t>(bytes_[position_++]) << (i * 8U);
    }
    return true;
  }

  template<std::size_t count>
  bool read_array(std::array<std::uint8_t, count> &value) {
    if (remaining() < count) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), count, value.begin());
    position_ += count;
    return true;
  }

  bool read_fixed_string(const std::size_t count, std::string &value) {
    if (remaining() < count) {
      return false;
    }
    value.assign(
      reinterpret_cast<const char *>(bytes_.data() + position_),
      reinterpret_cast<const char *>(bytes_.data() + position_ + count));
    position_ += count;
    return true;
  }

  bool read_blob(const std::size_t maximum, std::vector<std::uint8_t> &value) {
    std::uint32_t count = 0;
    if (!read_u32(count) || count > maximum || remaining() < count) {
      return false;
    }
    value.assign(
      bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
      bytes_.begin() + static_cast<std::ptrdiff_t>(position_ + count));
    position_ += count;
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t position_ {};
};

constexpr std::array<std::uint32_t, 64> sha256_constants {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
  0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
  0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
  0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
  0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
  0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

void sha256_transform(std::array<std::uint32_t, 8> &state, const std::uint8_t *block) noexcept {
  std::array<std::uint32_t, 64> words {};
  for (std::size_t i = 0; i < 16; ++i) {
    words[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (std::size_t i = 16; i < words.size(); ++i) {
    const auto s0 = std::rotr(words[i - 15], 7) ^ std::rotr(words[i - 15], 18) ^ (words[i - 15] >> 3U);
    const auto s1 = std::rotr(words[i - 2], 17) ^ std::rotr(words[i - 2], 19) ^ (words[i - 2] >> 10U);
    words[i] = words[i - 16] + s0 + words[i - 7] + s1;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];
  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto big_s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
    const auto choice = (e & f) ^ (~e & g);
    const auto temporary1 = h + big_s1 + choice + sha256_constants[i] + words[i];
    const auto big_s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary2 = big_s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

bool add_checked(std::uint64_t &total, const std::uint64_t value) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) {
    return false;
  }
  total += value;
  return true;
}

bool trusted(const artifact_observation_t &artifact) noexcept {
  return artifact.content == artifact_content_t::absent ||
         (artifact.trusted_acl && artifact.same_volume && artifact.no_reparse_points &&
          artifact.single_link && artifact.no_extra_streams && artifact.identity_matches_receipt);
}

bool append_optional(writer_t &writer, const optional_blob_t &field) {
  if (field.availability != field_availability_t::unsupported &&
      field.availability != field_availability_t::present) {
    return false;
  }
  if (field.availability == field_availability_t::unsupported && !field.value.empty()) {
    return false;
  }
  return writer.append_u8(static_cast<std::uint8_t>(field.availability)) &&
         writer.append_blob(field.value);
}

bool all_zero(const hash256_t &hash) noexcept {
  return std::all_of(hash.begin(), hash.end(), [](const auto value) { return value == 0; });
}

bool empty_identity(const file_identity_t &identity) noexcept {
  return identity.volume_serial == 0 &&
         std::all_of(identity.file_id.begin(), identity.file_id.end(), [](const auto value) { return value == 0; });
}

bool receipt_semantics_valid(const receipt_t &receipt) noexcept {
  if (receipt.original_main_size == 0 || receipt.service_executable_size == 0 ||
      receipt.candidate_main_size == 0 || receipt.candidate_helper_size == 0 ||
      all_zero(receipt.expected_original_hash) || all_zero(receipt.candidate_main_hash) ||
      all_zero(receipt.candidate_helper_hash) || all_zero(receipt.service_executable_hash) ||
      empty_identity(receipt.original_main_identity) || empty_identity(receipt.original_main_snapshot_identity) ||
      empty_identity(receipt.service_executable_identity) ||
      empty_identity(receipt.candidate_main_identity) || empty_identity(receipt.candidate_helper_identity) ||
      receipt.original_main_identity.volume_serial != receipt.service_executable_identity.volume_serial ||
      receipt.original_main_identity.volume_serial != receipt.original_main_snapshot_identity.volume_serial ||
      receipt.original_main_identity.volume_serial != receipt.candidate_main_identity.volume_serial ||
      receipt.original_main_identity.volume_serial != receipt.candidate_helper_identity.volume_serial) {
    return false;
  }
  if (receipt.helper_originally_present) {
    return receipt.original_helper_size != 0 && !all_zero(receipt.original_helper_hash) &&
           !empty_identity(receipt.original_helper_identity) &&
           receipt.original_helper_identity.volume_serial == receipt.original_main_identity.volume_serial;
  }
  return receipt.original_helper_size == 0 && all_zero(receipt.original_helper_hash) &&
         empty_identity(receipt.original_helper_identity);
}

}  // namespace

hash256_t sha256(const std::vector<std::uint8_t> &bytes) noexcept {
  std::array<std::uint32_t, 8> state {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::size_t offset = 0;
  while (bytes.size() - offset >= 64) {
    sha256_transform(state, bytes.data() + offset);
    offset += 64;
  }

  std::array<std::uint8_t, 128> tail {};
  const auto tail_bytes = bytes.size() - offset;
  if (tail_bytes != 0) {
    std::copy_n(bytes.data() + offset, tail_bytes, tail.data());
  }
  tail[tail_bytes] = 0x80U;
  const auto padded_bytes = tail_bytes < 56 ? 64U : 128U;
  const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8ULL;
  for (std::size_t i = 0; i < 8; ++i) {
    tail[padded_bytes - 1 - i] = static_cast<std::uint8_t>((bit_length >> (i * 8U)) & 0xffU);
  }
  sha256_transform(state, tail.data());
  if (padded_bytes == 128U) {
    sha256_transform(state, tail.data() + 64);
  }

  hash256_t digest {};
  for (std::size_t i = 0; i < state.size(); ++i) {
    digest[i * 4] = static_cast<std::uint8_t>((state[i] >> 24U) & 0xffU);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((state[i] >> 16U) & 0xffU);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((state[i] >> 8U) & 0xffU);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(state[i] & 0xffU);
  }
  return digest;
}

std::string hash_to_hex(const hash256_t &hash) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  text.resize(hash.size() * 2U);
  for (std::size_t i = 0; i < hash.size(); ++i) {
    text[i * 2] = digits[hash[i] >> 4U];
    text[i * 2 + 1] = digits[hash[i] & 0x0fU];
  }
  return text;
}

std::optional<hash256_t> hash_from_hex(const std::string &text) noexcept {
  if (text.size() != 64) {
    return std::nullopt;
  }
  const auto nibble = [](const char value) -> std::optional<std::uint8_t> {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
      return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    return std::nullopt;
  };
  hash256_t value {};
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto high = nibble(text[i * 2]);
    const auto low = nibble(text[i * 2 + 1]);
    if (!high || !low) {
      return std::nullopt;
    }
    value[i] = static_cast<std::uint8_t>((*high << 4U) | *low);
  }
  return value;
}

bool valid_transaction_id(const std::string &transaction_id) noexcept {
  if (transaction_id.size() != transaction_id_bytes) {
    return false;
  }
  constexpr std::array hyphens {8U, 13U, 18U, 23U};
  for (std::size_t i = 0; i < transaction_id.size(); ++i) {
    if (std::find(hyphens.begin(), hyphens.end(), i) != hyphens.end()) {
      if (transaction_id[i] != '-') {
        return false;
      }
      continue;
    }
    if (!((transaction_id[i] >= '0' && transaction_id[i] <= '9') ||
          (transaction_id[i] >= 'a' && transaction_id[i] <= 'f'))) {
      return false;
    }
  }
  return true;
}

result_t<std::vector<std::uint8_t>> serialize_receipt(const receipt_t &receipt) {
  if (!valid_transaction_id(receipt.transaction_id)) {
    return result_t<std::vector<std::uint8_t>>::failure("invalid transaction id");
  }
  if (receipt.scm_baseline.size() > k_max_scm_baseline_bytes) {
    return result_t<std::vector<std::uint8_t>>::failure("SCM baseline exceeds limit");
  }
  if (sha256(receipt.scm_baseline) != receipt.baseline_digest) {
    return result_t<std::vector<std::uint8_t>>::failure("SCM baseline digest mismatch");
  }
  if (!receipt_semantics_valid(receipt)) {
    return result_t<std::vector<std::uint8_t>>::failure("receipt semantic fields are inconsistent");
  }

  writer_t writer(k_max_receipt_bytes - receipt_digest_bytes);
  if (!writer.append_array(receipt_magic) || !writer.append_u32(format_version) ||
      !writer.append_u32(0) ||
      !writer.append(std::as_bytes(std::span(receipt.transaction_id))) ||
      !writer.append_array(receipt.expected_original_hash) ||
      !writer.append_array(receipt.candidate_main_hash) ||
      !writer.append_array(receipt.candidate_helper_hash) ||
      !writer.append_array(receipt.original_helper_hash) ||
      !writer.append_array(receipt.service_executable_hash) ||
      !writer.append_array(receipt.baseline_digest) ||
      !writer.append_u64(receipt.original_main_identity.volume_serial) ||
      !writer.append_array(receipt.original_main_identity.file_id) ||
      !writer.append_u64(receipt.original_main_snapshot_identity.volume_serial) ||
      !writer.append_array(receipt.original_main_snapshot_identity.file_id) ||
      !writer.append_u64(receipt.original_helper_identity.volume_serial) ||
      !writer.append_array(receipt.original_helper_identity.file_id) ||
      !writer.append_u64(receipt.service_executable_identity.volume_serial) ||
      !writer.append_array(receipt.service_executable_identity.file_id) ||
      !writer.append_u64(receipt.candidate_main_identity.volume_serial) ||
      !writer.append_array(receipt.candidate_main_identity.file_id) ||
      !writer.append_u64(receipt.candidate_helper_identity.volume_serial) ||
      !writer.append_array(receipt.candidate_helper_identity.file_id) ||
      !writer.append_u64(receipt.original_main_size) ||
      !writer.append_u64(receipt.original_helper_size) ||
      !writer.append_u64(receipt.service_executable_size) ||
      !writer.append_u64(receipt.candidate_main_size) ||
      !writer.append_u64(receipt.candidate_helper_size) ||
      !writer.append_u8(receipt.helper_originally_present ? 1U : 0U)) {
    return result_t<std::vector<std::uint8_t>>::failure("receipt exceeds limit");
  }
  for (std::size_t i = 0; i < 7; ++i) {
    if (!writer.append_u8(0)) {
      return result_t<std::vector<std::uint8_t>>::failure("receipt exceeds limit");
    }
  }
  if (!writer.append_u64(receipt.created_filetime) || !writer.append_blob(receipt.scm_baseline)) {
    return result_t<std::vector<std::uint8_t>>::failure("receipt exceeds limit");
  }

  auto bytes = writer.take();
  const auto total_size = bytes.size() + receipt_digest_bytes;
  if (total_size > std::numeric_limits<std::uint32_t>::max()) {
    return result_t<std::vector<std::uint8_t>>::failure("receipt size overflow");
  }
  const auto size = static_cast<std::uint32_t>(total_size);
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[12 + i] = static_cast<std::uint8_t>((size >> (i * 8U)) & 0xffU);
  }
  const auto digest = sha256(bytes);
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  return result_t<std::vector<std::uint8_t>>::success(std::move(bytes));
}

result_t<receipt_t> parse_receipt(const std::vector<std::uint8_t> &bytes) {
  constexpr std::size_t minimum_size =
    8 + 4 + 4 + transaction_id_bytes + 6 * 32 + 6 * (8 + 16) + 6 * 8 + 8 + 4 + 32;
  if (bytes.size() < minimum_size || bytes.size() > k_max_receipt_bytes) {
    return result_t<receipt_t>::failure("receipt length is invalid");
  }
  const std::vector<std::uint8_t> payload(bytes.begin(), bytes.end() - receipt_digest_bytes);
  hash256_t stored_digest {};
  std::copy_n(bytes.end() - receipt_digest_bytes, receipt_digest_bytes, stored_digest.begin());
  if (sha256(payload) != stored_digest) {
    return result_t<receipt_t>::failure("receipt digest mismatch");
  }

  reader_t reader(payload);
  std::array<std::uint8_t, 8> magic {};
  std::uint32_t version = 0;
  std::uint32_t encoded_size = 0;
  receipt_t receipt;
  if (!reader.read_array(magic) || magic != receipt_magic ||
      !reader.read_u32(version) || version != format_version ||
      !reader.read_u32(encoded_size) || encoded_size != bytes.size() ||
      !reader.read_fixed_string(transaction_id_bytes, receipt.transaction_id) ||
      !valid_transaction_id(receipt.transaction_id) ||
      !reader.read_array(receipt.expected_original_hash) ||
      !reader.read_array(receipt.candidate_main_hash) ||
      !reader.read_array(receipt.candidate_helper_hash) ||
      !reader.read_array(receipt.original_helper_hash) ||
      !reader.read_array(receipt.service_executable_hash) ||
      !reader.read_array(receipt.baseline_digest) ||
      !reader.read_u64(receipt.original_main_identity.volume_serial) ||
      !reader.read_array(receipt.original_main_identity.file_id) ||
      !reader.read_u64(receipt.original_main_snapshot_identity.volume_serial) ||
      !reader.read_array(receipt.original_main_snapshot_identity.file_id) ||
      !reader.read_u64(receipt.original_helper_identity.volume_serial) ||
      !reader.read_array(receipt.original_helper_identity.file_id) ||
      !reader.read_u64(receipt.service_executable_identity.volume_serial) ||
      !reader.read_array(receipt.service_executable_identity.file_id) ||
      !reader.read_u64(receipt.candidate_main_identity.volume_serial) ||
      !reader.read_array(receipt.candidate_main_identity.file_id) ||
      !reader.read_u64(receipt.candidate_helper_identity.volume_serial) ||
      !reader.read_array(receipt.candidate_helper_identity.file_id) ||
      !reader.read_u64(receipt.original_main_size) ||
      !reader.read_u64(receipt.original_helper_size) ||
      !reader.read_u64(receipt.service_executable_size) ||
      !reader.read_u64(receipt.candidate_main_size) ||
      !reader.read_u64(receipt.candidate_helper_size)) {
    return result_t<receipt_t>::failure("receipt header is invalid");
  }
  std::uint8_t helper_present = 0;
  if (!reader.read_u8(helper_present) || helper_present > 1) {
    return result_t<receipt_t>::failure("receipt flags are invalid");
  }
  receipt.helper_originally_present = helper_present == 1;
  for (std::size_t i = 0; i < 7; ++i) {
    std::uint8_t reserved = 0;
    if (!reader.read_u8(reserved) || reserved != 0) {
      return result_t<receipt_t>::failure("receipt reserved bytes are nonzero");
    }
  }
  if (!reader.read_u64(receipt.created_filetime) ||
      !reader.read_blob(k_max_scm_baseline_bytes, receipt.scm_baseline) ||
      reader.remaining() != 0 || sha256(receipt.scm_baseline) != receipt.baseline_digest) {
    return result_t<receipt_t>::failure("receipt body is invalid");
  }
  if (!receipt_semantics_valid(receipt)) {
    return result_t<receipt_t>::failure("receipt semantic fields are inconsistent");
  }
  return result_t<receipt_t>::success(std::move(receipt));
}

result_t<std::vector<std::uint8_t>> serialize_scm_baseline(const scm_semantic_baseline_t &baseline) {
  writer_t writer(k_max_scm_baseline_bytes);
  if (!writer.append_array(scm_magic) || !writer.append_u32(format_version) ||
      !writer.append_string(baseline.account) || !writer.append_string(baseline.binary_path) ||
      !writer.append_u32(baseline.service_type) || !writer.append_u32(baseline.start_type) ||
      !writer.append_u32(baseline.error_control) ||
      baseline.dependencies.size() > std::numeric_limits<std::uint32_t>::max() ||
      !writer.append_u32(static_cast<std::uint32_t>(baseline.dependencies.size()))) {
    return result_t<std::vector<std::uint8_t>>::failure("SCM baseline exceeds limit");
  }
  for (const auto &dependency : baseline.dependencies) {
    if (!writer.append_string(dependency)) {
      return result_t<std::vector<std::uint8_t>>::failure("SCM dependency exceeds limit");
    }
  }
  const std::array fields {
    &baseline.failure_actions,
    &baseline.failure_actions_flag,
    &baseline.delayed_start,
    &baseline.sid_type,
    &baseline.required_privileges,
    &baseline.preshutdown_timeout,
    &baseline.triggers,
    &baseline.preferred_node,
    &baseline.managed_account,
    &baseline.launch_protection,
  };
  for (const auto *field : fields) {
    if (!append_optional(writer, *field)) {
      return result_t<std::vector<std::uint8_t>>::failure("SCM optional field is invalid or too large");
    }
  }
  if (!writer.append_blob(baseline.service_security_owner_dacl) ||
      !writer.append_blob(baseline.registry_security_owner_dacl)) {
    return result_t<std::vector<std::uint8_t>>::failure("SCM security descriptor exceeds limit");
  }
  return result_t<std::vector<std::uint8_t>>::success(writer.take());
}

result_t<hash256_t> digest_scm_baseline(const scm_semantic_baseline_t &baseline) {
  auto encoded = serialize_scm_baseline(baseline);
  if (!encoded.ok()) {
    return result_t<hash256_t>::failure(std::move(encoded.error));
  }
  return result_t<hash256_t>::success(sha256(encoded.value));
}

std::optional<std::uint64_t> required_free_space(const space_inputs_t &input) noexcept {
  std::uint64_t total = 0;
  const std::array values {
    input.candidate_main_bytes,
    input.candidate_helper_bytes,
    input.original_main_bytes,
    input.original_helper_bytes,
    input.original_main_bytes,
    input.candidate_main_bytes,
    input.candidate_helper_bytes,
    input.receipt_bytes,
    input.safety_margin_bytes,
  };
  for (const auto value : values) {
    if (!add_checked(total, value)) {
      return std::nullopt;
    }
  }
  return total;
}

physical_state_t classify_physical_state(const physical_observation_t &observation) noexcept {
  const std::array artifacts {
    observation.main_target,
    observation.helper_target,
    observation.original_snapshot,
    observation.helper_snapshot,
    observation.live_backup,
    observation.failed_main,
    observation.failed_helper,
  };
  if (!observation.namespace_exact || !observation.scm_baseline_matches ||
      observation.receipt == receipt_presence_t::invalid ||
      !std::all_of(artifacts.begin(), artifacts.end(), trusted)) {
    return physical_state_t::unsafe;
  }

  if (observation.receipt == receipt_presence_t::absent) {
    const auto no_transaction_artifacts = observation.original_snapshot.content == artifact_content_t::absent &&
                                          observation.helper_snapshot.content == artifact_content_t::absent &&
                                          observation.live_backup.content == artifact_content_t::absent &&
                                          observation.failed_main.content == artifact_content_t::absent &&
                                          observation.failed_helper.content == artifact_content_t::absent;
    if (!no_transaction_artifacts || observation.main_target.content != artifact_content_t::original) {
      return physical_state_t::unsafe;
    }
    return observation.service_health == service_health_t::healthy_original ?
             physical_state_t::pristine_original_healthy : physical_state_t::pristine_original_stopped;
  }

  if (observation.original_snapshot.content != artifact_content_t::original) {
    return physical_state_t::unsafe;
  }
  const auto main = observation.main_target.content;
  const auto helper = observation.helper_target.content;
  const auto backup = observation.live_backup.content;
  if (main == artifact_content_t::original &&
      (helper == artifact_content_t::absent || helper == artifact_content_t::original) &&
      (backup == artifact_content_t::absent || backup == artifact_content_t::original)) {
    if (observation.failed_main.content == artifact_content_t::candidate ||
        observation.failed_helper.content == artifact_content_t::candidate) {
      return observation.service_health == service_health_t::healthy_original ?
               physical_state_t::rolled_back_healthy : physical_state_t::rolled_back_stopped;
    }
    return observation.service_health == service_health_t::healthy_original ?
             physical_state_t::receipt_ready_original_running : physical_state_t::receipt_ready_original_stopped;
  }
  if (main == artifact_content_t::candidate && helper == artifact_content_t::candidate &&
      backup == artifact_content_t::original) {
    if (observation.service_health == service_health_t::healthy_candidate) {
      return physical_state_t::deployed_healthy;
    }
    if (observation.service_health == service_health_t::stopped) {
      return physical_state_t::deployed_stopped;
    }
    return physical_state_t::rollback_required;
  }
  if (main == artifact_content_t::other || helper == artifact_content_t::other ||
      backup == artifact_content_t::other) {
    return physical_state_t::unsafe;
  }
  return physical_state_t::rollback_required;
}

}  // namespace vibepollo::audio_hotfix
