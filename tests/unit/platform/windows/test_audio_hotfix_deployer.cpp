#include <gtest/gtest.h>

#include "tools/vibepollo_audio_hotfix_deployer_core.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

namespace hotfix = vibepollo::audio_hotfix;

namespace {

hotfix::hash256_t hash(const std::uint8_t fill) {
  hotfix::hash256_t value {};
  value.fill(fill);
  return value;
}

hotfix::receipt_t sample_receipt() {
  hotfix::receipt_t receipt;
  receipt.transaction_id = "01234567-89ab-cdef-0123-456789abcdef";
  receipt.expected_original_hash = hash(0x11);
  receipt.candidate_main_hash = hash(0x22);
  receipt.candidate_helper_hash = hash(0x33);
  receipt.original_helper_hash = hash(0x34);
  receipt.service_executable_hash = hash(0x35);
  receipt.original_main_identity.volume_serial = 0x0102030405060708ULL;
  receipt.original_main_identity.file_id.fill(0x55);
  receipt.original_main_snapshot_identity.volume_serial = receipt.original_main_identity.volume_serial;
  receipt.original_main_snapshot_identity.file_id.fill(0x5b);
  receipt.original_helper_identity.volume_serial = receipt.original_main_identity.volume_serial;
  receipt.original_helper_identity.file_id.fill(0x56);
  receipt.service_executable_identity.volume_serial = receipt.original_main_identity.volume_serial;
  receipt.service_executable_identity.file_id.fill(0x57);
  receipt.candidate_main_identity.volume_serial = receipt.original_main_identity.volume_serial;
  receipt.candidate_main_identity.file_id.fill(0x58);
  receipt.candidate_helper_identity.volume_serial = receipt.original_main_identity.volume_serial;
  receipt.candidate_helper_identity.file_id.fill(0x59);
  receipt.original_main_size = 48ULL * 1024ULL * 1024ULL;
  receipt.original_helper_size = 1ULL * 1024ULL * 1024ULL;
  receipt.service_executable_size = 2ULL * 1024ULL * 1024ULL;
  receipt.candidate_main_size = 52ULL * 1024ULL * 1024ULL;
  receipt.candidate_helper_size = 2ULL * 1024ULL * 1024ULL;
  receipt.helper_originally_present = true;
  receipt.created_filetime = 0x1122334455667788ULL;
  receipt.scm_baseline = {0x01, 0x02, 0x03, 0x04};
  receipt.baseline_digest = hotfix::sha256(receipt.scm_baseline);
  return receipt;
}

hotfix::artifact_observation_t artifact(
  const hotfix::artifact_content_t content,
  const bool trusted = true
) {
  return {
    .content = content,
    .trusted_acl = trusted,
    .same_volume = true,
    .no_reparse_points = true,
    .single_link = true,
    .no_extra_streams = true,
    .identity_matches_receipt = true,
  };
}

}  // namespace

TEST(AudioHotfixDeployerCore, Sha256MatchesPublishedVector) {
  const std::vector<std::uint8_t> abc {'a', 'b', 'c'};
  EXPECT_EQ(
    hotfix::hash_to_hex(hotfix::sha256(abc)),
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(AudioHotfixDeployerCore, HexHashParserIsStrictAndCanonical) {
  const auto expected = hash(0xab);
  const auto parsed = hotfix::hash_from_hex(hotfix::hash_to_hex(expected));
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, expected);
  EXPECT_FALSE(hotfix::hash_from_hex(std::string(63, '0')).has_value());
  EXPECT_FALSE(hotfix::hash_from_hex(std::string(64, 'G')).has_value());
}

TEST(AudioHotfixDeployerCore, ReceiptRoundTripsDeterministically) {
  const auto expected = sample_receipt();
  const auto first = hotfix::serialize_receipt(expected);
  ASSERT_TRUE(first.ok()) << first.error;
  const auto second = hotfix::serialize_receipt(expected);
  ASSERT_TRUE(second.ok()) << second.error;
  EXPECT_EQ(first.value, second.value);

  const auto parsed = hotfix::parse_receipt(first.value);
  ASSERT_TRUE(parsed.ok()) << parsed.error;
  EXPECT_EQ(parsed.value, expected);
}

TEST(AudioHotfixDeployerCore, ReceiptRejectsCorruptionTrailingDataAndUnsafeId) {
  auto serialized = hotfix::serialize_receipt(sample_receipt());
  ASSERT_TRUE(serialized.ok()) << serialized.error;
  serialized.value.at(40) ^= 0x01;
  EXPECT_FALSE(hotfix::parse_receipt(serialized.value).ok());

  serialized = hotfix::serialize_receipt(sample_receipt());
  ASSERT_TRUE(serialized.ok()) << serialized.error;
  serialized.value.push_back(0);
  EXPECT_FALSE(hotfix::parse_receipt(serialized.value).ok());

  auto unsafe = sample_receipt();
  unsafe.transaction_id = "..\\escape";
  EXPECT_FALSE(hotfix::serialize_receipt(unsafe).ok());
}

TEST(AudioHotfixDeployerCore, ReceiptEnforcesBoundedBaseline) {
  auto receipt = sample_receipt();
  receipt.scm_baseline.resize(hotfix::k_max_scm_baseline_bytes + 1U);
  EXPECT_FALSE(hotfix::serialize_receipt(receipt).ok());
}

TEST(AudioHotfixDeployerCore, ReceiptRejectsInconsistentOptionalHelperState) {
  auto receipt = sample_receipt();
  receipt.helper_originally_present = false;
  EXPECT_FALSE(hotfix::serialize_receipt(receipt).ok());

  receipt.original_helper_hash = {};
  receipt.original_helper_identity = {};
  receipt.original_helper_size = 0;
  EXPECT_TRUE(hotfix::serialize_receipt(receipt).ok());
}

TEST(AudioHotfixDeployerCore, RequiredSpaceCoversWorstCaseProductionArtifacts) {
  hotfix::space_inputs_t input {
    .original_main_bytes = 80ULL * 1024ULL * 1024ULL,
    .original_helper_bytes = 3ULL * 1024ULL * 1024ULL,
    .candidate_main_bytes = 90ULL * 1024ULL * 1024ULL,
    .candidate_helper_bytes = 4ULL * 1024ULL * 1024ULL,
    .receipt_bytes = 128ULL * 1024ULL,
    .safety_margin_bytes = 64ULL * 1024ULL * 1024ULL,
  };
  const auto required = hotfix::required_free_space(input);
  ASSERT_TRUE(required.has_value());
  // Candidate stages + immutable originals + the ReplaceFile live backup +
  // failed-candidate names + receipt + an explicit reserve.
  EXPECT_EQ(*required, 435290112ULL);
}

TEST(AudioHotfixDeployerCore, RequiredSpaceFailsClosedOnOverflow) {
  hotfix::space_inputs_t input {};
  input.original_main_bytes = std::numeric_limits<std::uint64_t>::max();
  input.candidate_main_bytes = 1;
  EXPECT_FALSE(hotfix::required_free_space(input).has_value());
}

TEST(AudioHotfixDeployerCore, ScmBaselineEncodingIsStableAndAvailabilityExplicit) {
  hotfix::scm_semantic_baseline_t baseline;
  baseline.account = "LocalSystem";
  baseline.binary_path = "C:\\Program Files\\Apollo\\sunshine.exe";
  baseline.start_type = 2;
  baseline.dependencies = {"Tcpip", "Afd"};
  baseline.failure_actions.availability = hotfix::field_availability_t::present;
  baseline.failure_actions.value = {1, 2, 3};
  baseline.triggers.availability = hotfix::field_availability_t::unsupported;
  baseline.service_security_owner_dacl = {4, 5, 6};
  baseline.registry_security_owner_dacl = {7, 8, 9};

  const auto encoded = hotfix::serialize_scm_baseline(baseline);
  ASSERT_TRUE(encoded.ok()) << encoded.error;
  const auto encoded_again = hotfix::serialize_scm_baseline(baseline);
  ASSERT_TRUE(encoded_again.ok()) << encoded_again.error;
  EXPECT_EQ(encoded.value, encoded_again.value);
  EXPECT_EQ(hotfix::sha256(encoded.value), hotfix::digest_scm_baseline(baseline).value);

  auto changed = baseline;
  changed.triggers.availability = hotfix::field_availability_t::present;
  changed.triggers.value.clear();
  EXPECT_NE(
    hotfix::digest_scm_baseline(baseline).value,
    hotfix::digest_scm_baseline(changed).value);
}

TEST(AudioHotfixDeployerCore, PhysicalClassifierRecognizesConvergedStates) {
  hotfix::physical_observation_t state;
  state.receipt = hotfix::receipt_presence_t::valid;
  state.scm_baseline_matches = true;
  state.namespace_exact = true;
  state.main_target = artifact(hotfix::artifact_content_t::candidate);
  state.helper_target = artifact(hotfix::artifact_content_t::candidate);
  state.original_snapshot = artifact(hotfix::artifact_content_t::original);
  state.live_backup = artifact(hotfix::artifact_content_t::original);
  state.service_health = hotfix::service_health_t::healthy_candidate;
  EXPECT_EQ(hotfix::classify_physical_state(state), hotfix::physical_state_t::deployed_healthy);

  state.main_target = artifact(hotfix::artifact_content_t::original);
  state.helper_target = artifact(hotfix::artifact_content_t::absent);
  state.live_backup = artifact(hotfix::artifact_content_t::absent);
  state.failed_main = artifact(hotfix::artifact_content_t::candidate);
  state.service_health = hotfix::service_health_t::healthy_original;
  EXPECT_EQ(hotfix::classify_physical_state(state), hotfix::physical_state_t::rolled_back_healthy);
}

TEST(AudioHotfixDeployerCore, PhysicalClassifierFailsClosedForEveryTrustViolation) {
  hotfix::physical_observation_t state;
  state.receipt = hotfix::receipt_presence_t::valid;
  state.scm_baseline_matches = true;
  state.namespace_exact = true;
  state.main_target = artifact(hotfix::artifact_content_t::candidate);
  state.helper_target = artifact(hotfix::artifact_content_t::candidate);
  state.original_snapshot = artifact(hotfix::artifact_content_t::original);
  state.live_backup = artifact(hotfix::artifact_content_t::original);
  state.service_health = hotfix::service_health_t::stopped;

  const std::array<void (*)(hotfix::physical_observation_t &), 9> corruptions {{
    [](auto &s) { s.namespace_exact = false; },
    [](auto &s) { s.scm_baseline_matches = false; },
    [](auto &s) { s.main_target.trusted_acl = false; },
    [](auto &s) { s.main_target.same_volume = false; },
    [](auto &s) { s.main_target.no_reparse_points = false; },
    [](auto &s) { s.main_target.single_link = false; },
    [](auto &s) { s.main_target.no_extra_streams = false; },
    [](auto &s) { s.main_target.identity_matches_receipt = false; },
    [](auto &s) { s.receipt = hotfix::receipt_presence_t::invalid; },
  }};

  for (const auto corrupt : corruptions) {
    auto current = state;
    corrupt(current);
    EXPECT_EQ(hotfix::classify_physical_state(current), hotfix::physical_state_t::unsafe);
  }
}

TEST(AudioHotfixDeployerCore, PhysicalClassifierIsExhaustiveAcrossContentCombinations) {
  constexpr std::array contents {
    hotfix::artifact_content_t::absent,
    hotfix::artifact_content_t::original,
    hotfix::artifact_content_t::candidate,
    hotfix::artifact_content_t::other,
  };
  for (const auto main : contents) {
    for (const auto helper : contents) {
      for (const auto backup : contents) {
        hotfix::physical_observation_t state;
        state.receipt = hotfix::receipt_presence_t::valid;
        state.scm_baseline_matches = true;
        state.namespace_exact = true;
        state.main_target = artifact(main);
        state.helper_target = artifact(helper);
        state.original_snapshot = artifact(hotfix::artifact_content_t::original);
        state.live_backup = artifact(backup);
        state.service_health = hotfix::service_health_t::stopped;
        EXPECT_NE(hotfix::classify_physical_state(state), hotfix::physical_state_t::unknown);
      }
    }
  }
}

#ifdef _WIN32
namespace {

#ifndef VIBEPOLLO_AUDIO_HOTFIX_TEST_CLI_PATH
#error "The public test CLI path must be provided by CMake"
#endif

constexpr std::uint64_t mib = 1024ULL * 1024ULL;
constexpr char fixture_original_hash[] = "3b6a07d0d404fab4e23b6d34bc6696a6a312dd92821332385e5af7c01c421351";

struct fake_scm_state_t {
  std::uint32_t magic {0x46435356U};
  std::uint32_t version {1};
  std::uint32_t running {};
  std::uint32_t semantic_epoch {1};
  std::uint32_t health_ok {1};
  std::uint32_t listeners_ok {1};
  std::uint32_t start_count {};
  std::uint32_t stop_count {};
  std::uint32_t sunshine_pid {};
  std::uint32_t last_stopped_sunshine_pid {};
  std::uint32_t next_sunshine_pid {1000};
  std::uint32_t linger_sunshine_on_stop {};
  std::uint32_t reuse_sunshine_pid_on_start {};

  bool operator==(const fake_scm_state_t &) const = default;
};

struct child_result_t {
  DWORD exit_code {};
  std::string output;
};

std::wstring widen(const std::string &text) {
  return std::wstring(text.begin(), text.end());
}

std::wstring quote(const std::wstring &value) {
  std::wstring result = L"\"";
  std::size_t backslashes = 0;
  for (const auto character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      result.append(backslashes * 2U + 1U, L'\\');
      result.push_back(character);
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2U, L'\\');
  result.push_back(L'\"');
  return result;
}

class local_handle_t {
 public:
  explicit local_handle_t(HANDLE value = INVALID_HANDLE_VALUE): value_(value) {}
  ~local_handle_t() {
    if (value_ && value_ != INVALID_HANDLE_VALUE) {
      CloseHandle(value_);
    }
  }
  local_handle_t(const local_handle_t &) = delete;
  local_handle_t &operator=(const local_handle_t &) = delete;
  local_handle_t(local_handle_t &&other) noexcept: value_(other.release()) {}
  local_handle_t &operator=(local_handle_t &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const { return value_; }
  void reset(HANDLE value = INVALID_HANDLE_VALUE) {
    if (value_ && value_ != INVALID_HANDLE_VALUE) {
      CloseHandle(value_);
    }
    value_ = value;
  }
  HANDLE release() {
    const auto result = value_;
    value_ = INVALID_HANDLE_VALUE;
    return result;
  }

 private:
  HANDLE value_;
};

class local_memory_t {
 public:
  explicit local_memory_t(HLOCAL value = nullptr): value_(value) {}
  ~local_memory_t() {
    if (value_) {
      LocalFree(value_);
    }
  }
  [[nodiscard]] HLOCAL get() const { return value_; }

 private:
  HLOCAL value_;
};

PSECURITY_DESCRIPTOR secure_descriptor() {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:SYD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
    return nullptr;
  }
  return descriptor;
}

void create_secure_directory(const std::filesystem::path &path) {
  local_memory_t descriptor(secure_descriptor());
  ASSERT_NE(descriptor.get(), nullptr);
  SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor.get(), FALSE};
  ASSERT_TRUE(CreateDirectoryW(path.c_str(), &attributes) || GetLastError() == ERROR_ALREADY_EXISTS);
}

void create_file(
  const std::filesystem::path &path,
  const std::uint64_t size,
  const std::uint8_t pattern,
  const bool sparse_zero,
  const bool secure
) {
  local_memory_t descriptor(secure ? secure_descriptor() : nullptr);
  SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor.get(), FALSE};
  local_handle_t file(CreateFileW(
    path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
    secure ? &attributes : nullptr, CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
  ASSERT_NE(file.get(), INVALID_HANDLE_VALUE) << path.string();
  if (sparse_zero) {
    LARGE_INTEGER end {};
    end.QuadPart = static_cast<LONGLONG>(size);
    ASSERT_TRUE(SetFilePointerEx(file.get(), end, nullptr, FILE_BEGIN));
    ASSERT_TRUE(SetEndOfFile(file.get()));
  } else {
    std::vector<std::uint8_t> block(static_cast<std::size_t>(mib), pattern);
    std::uint64_t remaining = size;
    while (remaining != 0) {
      const auto request = static_cast<DWORD>(std::min<std::uint64_t>(remaining, block.size()));
      DWORD written = 0;
      ASSERT_TRUE(WriteFile(file.get(), block.data(), request, &written, nullptr));
      ASSERT_EQ(written, request);
      remaining -= written;
    }
  }
  ASSERT_TRUE(FlushFileBuffers(file.get()));
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

hotfix::hash256_t file_hash(const std::filesystem::path &path) {
  return hotfix::sha256(read_bytes(path));
}

hotfix::hash256_t tree_fingerprint(const std::filesystem::path &root) {
  std::vector<std::filesystem::path> paths {root};
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
         root, std::filesystem::directory_options::skip_permission_denied)) {
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());
  std::vector<std::uint8_t> bytes;
  const auto append = [&](const auto &value) {
    const auto *first = reinterpret_cast<const std::uint8_t *>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(value));
  };
  for (const auto &path : paths) {
    const auto relative = path.lexically_relative(root).wstring();
    const auto relative_size = static_cast<std::uint64_t>(relative.size());
    append(relative_size);
    const auto *relative_bytes = reinterpret_cast<const std::uint8_t *>(relative.data());
    bytes.insert(bytes.end(), relative_bytes, relative_bytes + relative.size() * sizeof(wchar_t));
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      throw std::runtime_error("cannot fingerprint attributes");
    }
    append(attributes);
    local_handle_t handle(CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT |
        ((attributes & FILE_ATTRIBUTE_DIRECTORY) ? FILE_FLAG_BACKUP_SEMANTICS : 0),
      nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("cannot fingerprint object handle");
    }
    FILE_ID_INFO identity {};
    FILE_STANDARD_INFO standard {};
    if (!GetFileInformationByHandleEx(handle.get(), FileIdInfo, &identity, sizeof(identity)) ||
        !GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard, sizeof(standard))) {
      throw std::runtime_error("cannot fingerprint object identity");
    }
    append(identity);
    append(standard.EndOfFile.QuadPart);
    append(standard.NumberOfLinks);
    PSECURITY_DESCRIPTOR security = nullptr;
    const auto security_status = GetSecurityInfo(
      handle.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      nullptr, nullptr, nullptr, nullptr, &security);
    local_memory_t security_memory(reinterpret_cast<HLOCAL>(security));
    if (security_status != ERROR_SUCCESS || !security) {
      throw std::runtime_error("cannot fingerprint object security");
    }
    const auto security_size = GetSecurityDescriptorLength(security);
    append(security_size);
    const auto *security_bytes = static_cast<const std::uint8_t *>(security);
    bytes.insert(bytes.end(), security_bytes, security_bytes + security_size);
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
      const auto hash = file_hash(path);
      bytes.insert(bytes.end(), hash.begin(), hash.end());
    }
  }
  return hotfix::sha256(bytes);
}

fake_scm_state_t read_fake_state(const std::filesystem::path &path) {
  fake_scm_state_t state;
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char *>(&state), sizeof(state));
  EXPECT_EQ(input.gcount(), sizeof(state));
  return state;
}

void write_fake_state(const std::filesystem::path &path, const fake_scm_state_t &state) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(&state), sizeof(state));
  output.flush();
  ASSERT_TRUE(output.good());
}

std::string uuid_for(const std::uint64_t value) {
  std::ostringstream output;
  output << "00000000-0000-0000-0000-" << std::hex;
  output.width(12);
  output.fill('0');
  output << value;
  return output.str();
}

class cli_fixture_t {
 public:
  explicit cli_fixture_t(const std::uint64_t candidate_main_size = 8ULL * mib) {
    static std::uint64_t sequence = 1;
    root_ = std::filesystem::temp_directory_path() /
            (L"vibepollo-audio-hotfix-native-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(sequence++));
    std::filesystem::create_directories(root_ / L"candidates");
    create_secure_directory(root_ / L"Apollo");
    create_secure_directory(root_ / L"Apollo" / L"tools");
    create_file(main_target(), 64ULL * mib, 0, true, true);
    create_file(original_helper(), 1ULL * mib, 0x11, false, true);
    create_file(service_executable(), 2ULL * mib, 0xc3, false, true);
    create_file(candidate_main(), candidate_main_size, 0xa5, false, false);
    create_file(candidate_helper(), 4ULL * mib, 0x5a, false, false);
    fake_scm_state_t state;
    write_fake_state(fake_scm(), state);
    expected_original_ = hotfix::hash_from_hex(fixture_original_hash).value();
    candidate_main_hash_ = file_hash(candidate_main());
    candidate_helper_hash_ = file_hash(candidate_helper());
    EXPECT_EQ(file_hash(main_target()), expected_original_);
  }

  ~cli_fixture_t() {
    std::error_code enumeration_error;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
           root_, std::filesystem::directory_options::skip_permission_denied, enumeration_error)) {
      const auto attributes = GetFileAttributesW(entry.path().c_str());
      if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(entry.path().c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
      }
    }
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &root() const { return root_; }
  [[nodiscard]] std::filesystem::path main_target() const { return root_ / L"Apollo" / L"sunshine.exe"; }
  [[nodiscard]] std::filesystem::path original_helper() const { return root_ / L"Apollo" / L"tools" / L"sunshine_audio_policy_helper.exe"; }
  [[nodiscard]] std::filesystem::path service_executable() const { return root_ / L"Apollo" / L"tools" / L"sunshinesvc.exe"; }
  [[nodiscard]] std::filesystem::path candidate_main() const { return root_ / L"candidates" / L"sunshine-candidate.exe"; }
  [[nodiscard]] std::filesystem::path candidate_helper() const { return root_ / L"candidates" / L"helper-candidate.exe"; }
  [[nodiscard]] std::filesystem::path fake_scm() const { return root_ / L"fake_scm.bin"; }
  [[nodiscard]] std::filesystem::path transaction(const std::string &id) const {
    return root_ / L"Apollo" / L".vibepollo-audio-hotfix" / widen(id);
  }

  child_result_t run(
    const std::vector<std::wstring> &arguments,
    const std::optional<std::string> &failpoint = std::nullopt,
    const std::optional<std::string> &replace_shape = std::nullopt
  ) const {
    SECURITY_ATTRIBUTES pipe_attributes {sizeof(pipe_attributes), nullptr, TRUE};
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    EXPECT_TRUE(CreatePipe(&raw_read, &raw_write, &pipe_attributes, 0));
    local_handle_t read_pipe(raw_read);
    local_handle_t write_pipe(raw_write);
    EXPECT_TRUE(SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0));

    std::wstring command = quote(widen(VIBEPOLLO_AUDIO_HOTFIX_TEST_CLI_PATH));
    for (const auto &argument : arguments) {
      command += L" " + quote(argument);
    }
    SetEnvironmentVariableW(L"VIBEPOLLO_AUDIO_HOTFIX_TEST_ROOT", root_.c_str());
    SetEnvironmentVariableA("VIBEPOLLO_AUDIO_HOTFIX_FAILPOINT", failpoint ? failpoint->c_str() : nullptr);
    SetEnvironmentVariableA("VIBEPOLLO_AUDIO_HOTFIX_REPLACE_SHAPE", replace_shape ? replace_shape->c_str() : nullptr);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process {};
    const auto created = CreateProcessW(
      nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(L"VIBEPOLLO_AUDIO_HOTFIX_TEST_ROOT", nullptr);
    SetEnvironmentVariableA("VIBEPOLLO_AUDIO_HOTFIX_FAILPOINT", nullptr);
    SetEnvironmentVariableA("VIBEPOLLO_AUDIO_HOTFIX_REPLACE_SHAPE", nullptr);
    EXPECT_TRUE(created);
    if (!created) {
      return {GetLastError(), "CreateProcess failed"};
    }
    local_handle_t process_handle(process.hProcess);
    local_handle_t thread_handle(process.hThread);
    write_pipe.reset();
    const auto wait = WaitForSingleObject(process_handle.get(), 120'000);
    if (wait == WAIT_TIMEOUT) {
      TerminateProcess(process_handle.get(), 198);
      WaitForSingleObject(process_handle.get(), 10'000);
    }
    DWORD exit_code = 0;
    EXPECT_TRUE(GetExitCodeProcess(process_handle.get(), &exit_code));
    std::string output;
    std::array<char, 4096> buffer {};
    DWORD count = 0;
    while (ReadFile(read_pipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0) {
      output.append(buffer.data(), count);
    }
    return {exit_code, std::move(output)};
  }

  std::vector<std::wstring> validate_args() const {
    return {
      L"validate",
      L"--candidate-main", candidate_main().wstring(),
      L"--candidate-helper", candidate_helper().wstring(),
      L"--expected-original-hash", widen(hotfix::hash_to_hex(expected_original_)),
      L"--candidate-main-hash", widen(hotfix::hash_to_hex(candidate_main_hash_)),
      L"--candidate-helper-hash", widen(hotfix::hash_to_hex(candidate_helper_hash_)),
    };
  }

  std::string baseline_digest() const {
    const auto result = run(validate_args());
    EXPECT_EQ(result.exit_code, 0U) << result.output;
    constexpr std::string_view marker = "baseline_digest=";
    const auto start = result.output.find(marker);
    EXPECT_NE(start, std::string::npos) << result.output;
    if (start == std::string::npos) {
      return {};
    }
    return result.output.substr(start + marker.size(), 64);
  }

  std::vector<std::wstring> mutating_args(
    const std::wstring &command,
    const std::string &id,
    const std::string &digest,
    const bool include_candidates
  ) const {
    std::vector<std::wstring> args {
      command,
      L"--transaction-id", widen(id),
    };
    if (include_candidates) {
      args.insert(args.end(), {
        L"--candidate-main", candidate_main().wstring(),
        L"--candidate-helper", candidate_helper().wstring(),
      });
    }
    args.insert(args.end(), {
      L"--expected-original-hash", widen(hotfix::hash_to_hex(expected_original_)),
      L"--candidate-main-hash", widen(hotfix::hash_to_hex(candidate_main_hash_)),
      L"--candidate-helper-hash", widen(hotfix::hash_to_hex(candidate_helper_hash_)),
      L"--baseline-digest", widen(digest),
    });
    return args;
  }

  bool receipt_valid(const std::string &id) const {
    const auto path = transaction(id) / L"receipt.bin";
    if (!std::filesystem::exists(path)) {
      return false;
    }
    return hotfix::parse_receipt(read_bytes(path)).ok();
  }

  void expect_original_healthy() const {
    EXPECT_EQ(file_hash(main_target()), expected_original_);
    EXPECT_NE(file_hash(original_helper()), candidate_helper_hash_);
    EXPECT_EQ(read_fake_state(fake_scm()).running, 1U);
  }

 private:
  std::filesystem::path root_;
  hotfix::hash256_t expected_original_ {};
  hotfix::hash256_t candidate_main_hash_ {};
  hotfix::hash256_t candidate_helper_hash_ {};
};

}  // namespace

TEST(AudioHotfixDeployerCli, ValidateIsByteAndSemanticReadOnly) {
  cli_fixture_t fixture;
  const auto before_main = file_hash(fixture.main_target());
  const auto before_helper = file_hash(fixture.original_helper());
  const auto before_service = file_hash(fixture.service_executable());
  const auto before_scm = read_fake_state(fixture.fake_scm());
  EXPECT_FALSE(std::filesystem::exists(fixture.main_target().parent_path() / L".vibepollo-audio-hotfix"));

  const auto first = fixture.run(fixture.validate_args());
  const auto second = fixture.run(fixture.validate_args());
  ASSERT_EQ(first.exit_code, 0U) << first.output;
  ASSERT_EQ(second.exit_code, 0U) << second.output;
  EXPECT_EQ(first.output, second.output);
  EXPECT_EQ(file_hash(fixture.main_target()), before_main);
  EXPECT_EQ(file_hash(fixture.original_helper()), before_helper);
  EXPECT_EQ(file_hash(fixture.service_executable()), before_service);
  EXPECT_EQ(read_fake_state(fixture.fake_scm()), before_scm);
  EXPECT_FALSE(std::filesystem::exists(fixture.main_target().parent_path() / L".vibepollo-audio-hotfix"));
}

TEST(AudioHotfixDeployerCli, DeployAbruptExitMatrixAlwaysRecoversOrStaysPristine) {
  const std::vector<std::string> failpoints {
    "main-copy-create", "main-copy-mid", "main-copy-flush",
    "helper-copy-create", "helper-copy-mid", "helper-copy-flush",
    "main-snapshot-create", "main-snapshot-mid", "main-snapshot-flush",
    "helper-snapshot-create", "helper-snapshot-mid", "helper-snapshot-flush",
    "stages-volume-flush", "receipt-create", "receipt-write", "receipt-flush",
    "receipt-reread", "receipt-volume-flush", "before-stop", "after-stop-request",
    "helper-original-rename", "helper-candidate-rename", "before-main-replace",
    "after-main-replace", "replacement-volume-flush", "before-start",
    "after-start-request", "before-health", "after-health",
  };
  std::uint64_t sequence = 100;
  for (const auto &failpoint : failpoints) {
    SCOPED_TRACE(failpoint);
    cli_fixture_t fixture;
    const auto digest = fixture.baseline_digest();
    const auto id = uuid_for(sequence++);
    const auto start = fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true));
    ASSERT_EQ(start.exit_code, 0U) << start.output;
    const auto killed = fixture.run(fixture.mutating_args(L"deploy", id, digest, true), failpoint);
    ASSERT_EQ(killed.exit_code, 197U) << killed.output;
    if (fixture.receipt_valid(id)) {
      const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
      const auto first = fixture.run(rollback_args);
      ASSERT_EQ(first.exit_code, 0U) << first.output;
      const auto second = fixture.run(rollback_args);
      ASSERT_EQ(second.exit_code, 0U) << second.output;
    }
    fixture.expect_original_healthy();
  }
}

TEST(AudioHotfixDeployerCli, RollbackAbruptExitMatrixConvergesTwice) {
  const std::vector<std::string> failpoints {
    "before-stop", "after-stop-request", "rollback-helper-failed-rename",
    "rollback-live-helper-rename",
    "before-rollback-live-main-replace", "after-rollback-live-main-replace",
    "rollback-volume-flush", "before-start", "after-start-request",
    "before-rollback-health", "after-rollback-health",
  };
  std::uint64_t sequence = 200;
  for (const auto &failpoint : failpoints) {
    SCOPED_TRACE(failpoint);
    cli_fixture_t fixture;
    const auto digest = fixture.baseline_digest();
    const auto id = uuid_for(sequence++);
    ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
    const auto deployed = fixture.run(fixture.mutating_args(L"deploy", id, digest, true));
    ASSERT_EQ(deployed.exit_code, 0U) << deployed.output;
    const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
    const auto killed = fixture.run(rollback_args, failpoint);
    ASSERT_EQ(killed.exit_code, 197U) << killed.output;
    const auto first = fixture.run(rollback_args);
    ASSERT_EQ(first.exit_code, 0U) << first.output;
    const auto second = fixture.run(rollback_args);
    ASSERT_EQ(second.exit_code, 0U) << second.output;
    fixture.expect_original_healthy();
  }
}

TEST(AudioHotfixDeployerCli, AlternateRollbackRenameCutsConvergeTwice) {
  const std::array<std::pair<std::string, std::string>, 2> cases {{
    {"unable-move-replacement", "rollback-main-snapshot-rename"},
    {"unable-move-replacement-2", "rollback-live-main-rename"},
  }};
  std::uint64_t sequence = 250;
  for (const auto &[shape, failpoint] : cases) {
    SCOPED_TRACE(shape + ":" + failpoint);
    cli_fixture_t fixture;
    const auto digest = fixture.baseline_digest();
    const auto id = uuid_for(sequence++);
    ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
    const auto deploy = fixture.run(
      fixture.mutating_args(L"deploy", id, digest, true), std::nullopt, shape);
    ASSERT_EQ(deploy.exit_code, 1U) << deploy.output;
    const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
    const auto killed = fixture.run(rollback_args, failpoint);
    ASSERT_EQ(killed.exit_code, 197U) << killed.output;
    const auto first = fixture.run(rollback_args);
    ASSERT_EQ(first.exit_code, 0U) << first.output;
    const auto second = fixture.run(rollback_args);
    ASSERT_EQ(second.exit_code, 0U) << second.output;
    fixture.expect_original_healthy();
  }
}

TEST(AudioHotfixDeployerCli, ProductionSizedHappyPathRollsBackIdempotently) {
  cli_fixture_t fixture(68ULL * mib);
  const auto digest = fixture.baseline_digest();
  const auto id = uuid_for(300);
  ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
  const auto deployed = fixture.run(fixture.mutating_args(L"deploy", id, digest, true));
  ASSERT_EQ(deployed.exit_code, 0U) << deployed.output;
  const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
  ASSERT_EQ(fixture.run(rollback_args).exit_code, 0U);
  ASSERT_EQ(fixture.run(rollback_args).exit_code, 0U);
  fixture.expect_original_healthy();
}

TEST(AudioHotfixDeployerCli, LingeringBaselineSunshineBlocksBeforeReplacement) {
  cli_fixture_t fixture;
  const auto digest = fixture.baseline_digest();
  const auto id = uuid_for(350);
  ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
  auto state = read_fake_state(fixture.fake_scm());
  if (state.sunshine_pid == 0) {
    state.sunshine_pid = 1001;
  }
  state.linger_sunshine_on_stop = 1;
  write_fake_state(fixture.fake_scm(), state);
  const auto before_main = file_hash(fixture.main_target());
  const auto before_helper = file_hash(fixture.original_helper());

  const auto deploy = fixture.run(fixture.mutating_args(L"deploy", id, digest, true));
  EXPECT_EQ(deploy.exit_code, 1U) << deploy.output;
  EXPECT_EQ(file_hash(fixture.main_target()), before_main);
  EXPECT_EQ(file_hash(fixture.original_helper()), before_helper);
  const auto after = read_fake_state(fixture.fake_scm());
  EXPECT_EQ(after.running, 0U);
  EXPECT_EQ(after.sunshine_pid, state.sunshine_pid);
  EXPECT_EQ(after.start_count, state.start_count);
  EXPECT_EQ(after.stop_count, state.stop_count + 1U);
}

TEST(AudioHotfixDeployerCli, CandidateHealthRejectsBaselinePidReuse) {
  cli_fixture_t fixture;
  const auto digest = fixture.baseline_digest();
  const auto id = uuid_for(351);
  ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
  auto state = read_fake_state(fixture.fake_scm());
  if (state.sunshine_pid == 0) {
    state.sunshine_pid = 1001;
  }
  state.last_stopped_sunshine_pid = state.sunshine_pid;
  state.next_sunshine_pid = state.sunshine_pid;
  state.reuse_sunshine_pid_on_start = 1;
  write_fake_state(fixture.fake_scm(), state);

  const auto deploy = fixture.run(fixture.mutating_args(L"deploy", id, digest, true));
  EXPECT_EQ(deploy.exit_code, 1U) << deploy.output;
  EXPECT_EQ(file_hash(fixture.main_target()), file_hash(fixture.candidate_main()));
  const auto after = read_fake_state(fixture.fake_scm());
  EXPECT_EQ(after.running, 1U);
  EXPECT_EQ(after.sunshine_pid, state.sunshine_pid);
  EXPECT_EQ(after.start_count, state.start_count + 1U);
  EXPECT_EQ(after.stop_count, state.stop_count + 1U);

  const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
  const auto first = fixture.run(rollback_args);
  ASSERT_EQ(first.exit_code, 0U) << first.output;
  const auto second = fixture.run(rollback_args);
  ASSERT_EQ(second.exit_code, 0U) << second.output;
  fixture.expect_original_healthy();
}

TEST(AudioHotfixDeployerCli, DocumentedReplaceFileOutcomesRecoverIdempotently) {
  const std::array<std::string, 3> shapes {
    "unable-remove-replaced",
    "unable-move-replacement",
    "unable-move-replacement-2",
  };
  std::uint64_t sequence = 400;
  for (const auto &shape : shapes) {
    SCOPED_TRACE(shape);
    cli_fixture_t fixture;
    const auto digest = fixture.baseline_digest();
    const auto id = uuid_for(sequence++);
    ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
    const auto deploy = fixture.run(fixture.mutating_args(L"deploy", id, digest, true), std::nullopt, shape);
    ASSERT_EQ(deploy.exit_code, 1U) << deploy.output;
    const auto rollback_args = fixture.mutating_args(L"rollback", id, digest, false);
    const auto first = fixture.run(rollback_args);
    ASSERT_EQ(first.exit_code, 0U) << first.output;
    const auto second = fixture.run(rollback_args);
    ASSERT_EQ(second.exit_code, 0U) << second.output;
    fixture.expect_original_healthy();
  }
}

TEST(AudioHotfixDeployerCli, RecoveryTamperPreflightHasZeroMutationOrServiceTransition) {
  const std::array<std::string, 7> cases {
    "wrong-bytes",
    "wrong-file-id",
    "hardlink",
    "reparse",
    "acl",
    "scm",
    "service-substitution",
  };
  std::uint64_t sequence = 500;
  for (const auto &tamper : cases) {
    SCOPED_TRACE(tamper);
    cli_fixture_t fixture;
    const auto digest = fixture.baseline_digest();
    const auto id = uuid_for(sequence++);
    ASSERT_EQ(fixture.run(fixture.mutating_args(L"start-baseline", id, digest, true)).exit_code, 0U);
    const auto deployed = fixture.run(fixture.mutating_args(L"deploy", id, digest, true));
    ASSERT_EQ(deployed.exit_code, 0U) << deployed.output;

    if (tamper == "wrong-bytes") {
      create_file(fixture.main_target(), 8ULL * mib, 0x77, false, true);
    } else if (tamper == "wrong-file-id") {
      ASSERT_TRUE(DeleteFileW(fixture.main_target().c_str()));
      create_file(fixture.main_target(), 8ULL * mib, 0xa5, false, true);
    } else if (tamper == "hardlink") {
      ASSERT_TRUE(CreateHardLinkW((fixture.root() / L"extra-main-link.exe").c_str(), fixture.main_target().c_str(), nullptr));
    } else if (tamper == "reparse") {
      ASSERT_TRUE(DeleteFileW(fixture.original_helper().c_str()));
      ASSERT_TRUE(CreateSymbolicLinkW(
        fixture.original_helper().c_str(), fixture.candidate_helper().c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE));
    } else if (tamper == "acl") {
      PSECURITY_DESCRIPTOR raw = nullptr;
      ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;WD)",
        SDDL_REVISION_1, &raw, nullptr));
      local_memory_t descriptor(reinterpret_cast<HLOCAL>(raw));
      ASSERT_TRUE(SetFileSecurityW(
        fixture.main_target().c_str(), OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        static_cast<PSECURITY_DESCRIPTOR>(descriptor.get())));
    } else if (tamper == "scm") {
      auto state = read_fake_state(fixture.fake_scm());
      ++state.semantic_epoch;
      write_fake_state(fixture.fake_scm(), state);
    } else if (tamper == "service-substitution") {
      ASSERT_TRUE(DeleteFileW(fixture.service_executable().c_str()));
      create_file(fixture.service_executable(), 2ULL * mib, 0xc3, false, true);
    }

    const auto before_state = read_fake_state(fixture.fake_scm());
    const auto before_tree = tree_fingerprint(fixture.root());
    const auto rollback = fixture.run(fixture.mutating_args(L"rollback", id, digest, false));
    EXPECT_EQ(rollback.exit_code, 1U) << rollback.output;
    EXPECT_EQ(read_fake_state(fixture.fake_scm()), before_state);
    EXPECT_EQ(tree_fingerprint(fixture.root()), before_tree);
  }
}
#endif
