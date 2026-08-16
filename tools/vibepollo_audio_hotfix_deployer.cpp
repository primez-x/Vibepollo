#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "tools/vibepollo_audio_hotfix_deployer_core.h"

#include <windows.h>
#include <aclapi.h>
#include <iphlpapi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <winsvc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace hotfix = vibepollo::audio_hotfix;

namespace {

constexpr wchar_t service_name[] = L"ApolloService";

#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
std::wstring required_test_root() {
  const auto required = GetEnvironmentVariableW(L"VIBEPOLLO_AUDIO_HOTFIX_TEST_ROOT", nullptr, 0);
  if (required <= 1 || required > 32768) {
    return {};
  }
  std::wstring value(required - 1, L'\0');
  if (GetEnvironmentVariableW(L"VIBEPOLLO_AUDIO_HOTFIX_TEST_ROOT", value.data(), required) != required - 1) {
    return {};
  }
  return value;
}
const std::wstring test_root = required_test_root();
const std::wstring install_root = test_root + L"\\Apollo";
const std::wstring main_target = install_root + L"\\sunshine.exe";
const std::wstring tools_root = install_root + L"\\tools";
const std::wstring helper_target = tools_root + L"\\sunshine_audio_policy_helper.exe";
const std::wstring service_executable = tools_root + L"\\sunshinesvc.exe";
const std::wstring service_command = L"\"" + service_executable + L"\" --service";
const std::wstring transaction_root = install_root + L"\\.vibepollo-audio-hotfix";
const std::wstring global_mutex_name = L"Local\\VibepolloAudioHotfixDeployerTest";
constexpr char expected_original_hash_hex[] = "3b6a07d0d404fab4e23b6d34bc6696a6a312dd92821332385e5af7c01c421351";
#else
const std::wstring install_root = L"C:\\Program Files\\Apollo";
const std::wstring main_target = L"C:\\Program Files\\Apollo\\sunshine.exe";
const std::wstring helper_target = L"C:\\Program Files\\Apollo\\tools\\sunshine_audio_policy_helper.exe";
const std::wstring tools_root = L"C:\\Program Files\\Apollo\\tools";
const std::wstring service_executable = L"C:\\Program Files\\Apollo\\tools\\sunshinesvc.exe";
const std::wstring service_command = L"\"C:\\Program Files\\Apollo\\tools\\sunshinesvc.exe\" --service";
const std::wstring transaction_root = L"C:\\Program Files\\Apollo\\.vibepollo-audio-hotfix";
const std::wstring global_mutex_name = L"Global\\VibepolloAudioHotfixDeployer";
constexpr char expected_original_hash_hex[] = "bf7e4a39a3957e7567484bce245cdd067db39a6c52e63b0e3e87f4d89e5eda4a";
#endif
constexpr std::uint64_t maximum_artifact_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t safety_margin_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr DWORD service_transition_timeout_ms = 30'000;
constexpr DWORD service_health_timeout_ms = 30'000;

constexpr wchar_t receipt_name[] = L"receipt.bin";
constexpr wchar_t main_stage_name[] = L"candidate.main.stage";
constexpr wchar_t helper_stage_name[] = L"candidate.helper.stage";
constexpr wchar_t main_snapshot_name[] = L"original.main.snapshot";
constexpr wchar_t helper_snapshot_name[] = L"original.helper.snapshot";
constexpr wchar_t main_live_backup_name[] = L"original.main.live";
constexpr wchar_t helper_live_backup_name[] = L"original.helper.live";
constexpr wchar_t failed_main_name[] = L"candidate.main.failed";
constexpr wchar_t failed_helper_name[] = L"candidate.helper.failed";
constexpr wchar_t rollback_main_stage_name[] = L"rollback.main.stage";

class handle_t {
 public:
  handle_t() = default;
  explicit handle_t(HANDLE value): value_(value) {}
  ~handle_t() {
    reset();
  }
  handle_t(const handle_t &) = delete;
  handle_t &operator=(const handle_t &) = delete;
  handle_t(handle_t &&other) noexcept: value_(other.release()) {}
  handle_t &operator=(handle_t &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
  HANDLE release() noexcept {
    const auto value = value_;
    value_ = INVALID_HANDLE_VALUE;
    return value;
  }
  void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
    if (valid()) {
      CloseHandle(value_);
    }
    value_ = value;
  }

 private:
  HANDLE value_ {INVALID_HANDLE_VALUE};
};

class service_handle_t {
 public:
  service_handle_t() = default;
  explicit service_handle_t(SC_HANDLE value): value_(value) {}
  ~service_handle_t() {
#ifndef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
    if (value_) {
      CloseServiceHandle(value_);
    }
#endif
  }
  service_handle_t(const service_handle_t &) = delete;
  service_handle_t &operator=(const service_handle_t &) = delete;
  service_handle_t(service_handle_t &&other) noexcept: value_(other.value_) { other.value_ = nullptr; }
  service_handle_t &operator=(service_handle_t &&other) noexcept {
    if (this != &other) {
#ifndef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
      if (value_) {
        CloseServiceHandle(value_);
      }
#endif
      value_ = other.value_;
      other.value_ = nullptr;
    }
    return *this;
  }
  [[nodiscard]] SC_HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }

 private:
  SC_HANDLE value_ {};
};

class mutex_release_t {
 public:
  explicit mutex_release_t(const HANDLE mutex): mutex_(mutex) {}
  ~mutex_release_t() {
    if (mutex_) {
      ReleaseMutex(mutex_);
    }
  }
  mutex_release_t(const mutex_release_t &) = delete;
  mutex_release_t &operator=(const mutex_release_t &) = delete;

 private:
  HANDLE mutex_ {};
};

struct local_memory_t {
  HLOCAL value {};
  ~local_memory_t() {
    if (value) {
      LocalFree(value);
    }
  }
};

struct file_record_t {
  handle_t handle;
  hotfix::hash256_t hash {};
  hotfix::file_identity_t identity;
  std::uint64_t size {};
  std::wstring canonical_path;
};

// MinGW's winsvc.h exposes SERVICE_CONFIG_TRIGGER_INFO but omits the query
// payload declarations. These pointer-bearing ABI views are consumed only long
// enough to copy the semantic values into the pointer-free baseline encoding.
struct service_trigger_specific_data_item_abi_t {
  DWORD dwDataType;
  DWORD cbData;
  PBYTE pData;
};

struct service_trigger_abi_t {
  DWORD dwTriggerType;
  DWORD dwAction;
  GUID *pTriggerSubtype;
  DWORD cDataItems;
  service_trigger_specific_data_item_abi_t *pDataItems;
};

struct service_trigger_info_abi_t {
  DWORD cTriggers;
  service_trigger_abi_t *pTriggers;
  PBYTE pReserved;
};

struct service_preferred_node_info_abi_t {
  USHORT preferred_node;
  BOOLEAN delete_preference;
};

struct service_launch_protected_info_abi_t {
  DWORD launch_protected;
};

enum class command_t {
  validate,
  start_baseline,
  deploy,
  status,
  rollback,
  recover,
};

struct arguments_t {
  command_t command {};
  std::string transaction_id;
  std::wstring candidate_main;
  std::wstring candidate_helper;
  hotfix::hash256_t expected_original_hash {};
  hotfix::hash256_t candidate_main_hash {};
  hotfix::hash256_t candidate_helper_hash {};
  std::optional<hotfix::hash256_t> baseline_digest;
};

#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
struct fake_scm_state_t {
  std::uint32_t magic {0x46435356U};  // VSCF
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
};

const std::wstring fake_scm_path = test_root + L"\\fake_scm.bin";
#endif

std::string windows_error(const DWORD error = GetLastError()) {
  wchar_t *message = nullptr;
  const auto length = FormatMessageW(
    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    nullptr,
    error,
    0,
    reinterpret_cast<wchar_t *>(&message),
    0,
    nullptr);
  local_memory_t memory {message};
  std::wstring wide = length && message ? std::wstring(message, length) : L"unknown error";
  while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
    wide.pop_back();
  }
  const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string result(bytes > 0 ? static_cast<std::size_t>(bytes) : 0U, '\0');
  if (bytes > 0) {
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), result.data(), bytes, nullptr, nullptr);
  }
  return result + " (" + std::to_string(error) + ")";
}

std::string utf8(const std::wstring &wide) {
  if (wide.empty()) {
    return {};
  }
  const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), result.data(), count, nullptr, nullptr) != count) {
    return {};
  }
  return result;
}

std::wstring lower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
    return static_cast<wchar_t>(towlower(character));
  });
  return value;
}

std::wstring path_join(const std::wstring &parent, const std::wstring &child) {
  return parent + L"\\" + child;
}

std::wstring transaction_path(const std::string &transaction_id) {
  return path_join(transaction_root, std::wstring(transaction_id.begin(), transaction_id.end()));
}

bool fail(std::string &error, std::string message) {
  error = std::move(message);
  return false;
}

#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
bool read_fake_scm(fake_scm_state_t &state, std::string &error) {
  handle_t file(CreateFileW(fake_scm_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file.valid()) {
    return fail(error, "cannot open fake SCM state: " + windows_error());
  }
  DWORD read = 0;
  if (!ReadFile(file.get(), &state, sizeof(state), &read, nullptr) || read != sizeof(state) ||
      state.magic != 0x46435356U || state.version != 1 || state.running > 1 ||
      state.health_ok > 1 || state.listeners_ok > 1 ||
      state.linger_sunshine_on_stop > 1 || state.reuse_sunshine_pid_on_start > 1) {
    return fail(error, "fake SCM state is invalid");
  }
  return true;
}

bool write_fake_scm(const fake_scm_state_t &state, std::string &error) {
  handle_t file(CreateFileW(fake_scm_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
  if (!file.valid()) {
    return fail(error, "cannot update fake SCM state: " + windows_error());
  }
  DWORD written = 0;
  if (!WriteFile(file.get(), &state, sizeof(state), &written, nullptr) || written != sizeof(state) ||
      !FlushFileBuffers(file.get())) {
    return fail(error, "cannot durably update fake SCM state: " + windows_error());
  }
  return true;
}
#endif

#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
[[noreturn]] void failpoint_exit(const char *name) {
  std::cerr << "failpoint=" << name << '\n';
  TerminateProcess(GetCurrentProcess(), 197);
  std::abort();
}

void failpoint(const char *name) {
  std::array<char, 128> value {};
  const auto count = GetEnvironmentVariableA("VIBEPOLLO_AUDIO_HOTFIX_FAILPOINT", value.data(), static_cast<DWORD>(value.size()));
  if (count != 0 && count < value.size() && std::string(value.data(), count) == name) {
    failpoint_exit(name);
  }
}
#else
void failpoint(const char *) {}
#endif

bool fixed_original_hash(const hotfix::hash256_t &hash) {
  const auto expected = hotfix::hash_from_hex(expected_original_hash_hex);
  return expected && *expected == hash;
}

bool parse_arguments(const int argc, wchar_t **argv, arguments_t &arguments, std::string &error) {
  if (argc < 2) {
    return fail(error, "missing command");
  }
  const std::map<std::wstring, command_t> commands {
    {L"validate", command_t::validate},
    {L"start-baseline", command_t::start_baseline},
    {L"deploy", command_t::deploy},
    {L"status", command_t::status},
    {L"rollback", command_t::rollback},
    {L"recover", command_t::recover},
  };
  const auto command = commands.find(argv[1]);
  if (command == commands.end()) {
    return fail(error, "unknown command");
  }
  arguments.command = command->second;
  std::map<std::wstring, std::wstring> options;
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 >= argc || std::wstring(argv[index]).rfind(L"--", 0) != 0) {
      return fail(error, "options require exact --name value pairs");
    }
    const std::wstring name = argv[index];
    if (!options.emplace(name, argv[index + 1]).second) {
      return fail(error, "duplicate option: " + utf8(name));
    }
  }
  const std::set<std::wstring> allowed {
    L"--transaction-id", L"--candidate-main", L"--candidate-helper",
    L"--expected-original-hash", L"--candidate-main-hash", L"--candidate-helper-hash",
    L"--baseline-digest",
  };
  for (const auto &[name, unused] : options) {
    static_cast<void>(unused);
    if (!allowed.contains(name)) {
      return fail(error, "unknown option: " + utf8(name));
    }
  }
  const auto get = [&](const wchar_t *name) -> std::optional<std::wstring> {
    const auto value = options.find(name);
    return value == options.end() ? std::nullopt : std::optional(value->second);
  };
  if (const auto value = get(L"--transaction-id")) {
    arguments.transaction_id = utf8(*value);
  }
  if (const auto value = get(L"--candidate-main")) {
    arguments.candidate_main = *value;
  }
  if (const auto value = get(L"--candidate-helper")) {
    arguments.candidate_helper = *value;
  }
  const auto parse_hash_option = [&](const wchar_t *name, hotfix::hash256_t &destination, const bool required) {
    const auto value = get(name);
    if (!value) {
      return !required;
    }
    const auto parsed = hotfix::hash_from_hex(utf8(*value));
    if (!parsed) {
      return false;
    }
    destination = *parsed;
    return true;
  };
  const auto mutating = arguments.command == command_t::start_baseline || arguments.command == command_t::deploy ||
                        arguments.command == command_t::rollback || arguments.command == command_t::recover;
  const auto requires_candidates = arguments.command == command_t::validate || arguments.command == command_t::start_baseline ||
                                   arguments.command == command_t::deploy;
  const auto requires_transaction = arguments.command != command_t::validate;
  if ((requires_transaction && !hotfix::valid_transaction_id(arguments.transaction_id)) ||
      (!arguments.transaction_id.empty() && !hotfix::valid_transaction_id(arguments.transaction_id))) {
    return fail(error, "a lowercase canonical transaction UUID is required");
  }
  if (requires_candidates && (arguments.candidate_main.empty() || arguments.candidate_helper.empty())) {
    return fail(error, "candidate-main and candidate-helper are required");
  }
  if (!parse_hash_option(L"--expected-original-hash", arguments.expected_original_hash, mutating || requires_candidates) ||
      !parse_hash_option(L"--candidate-main-hash", arguments.candidate_main_hash, mutating || requires_candidates) ||
      !parse_hash_option(L"--candidate-helper-hash", arguments.candidate_helper_hash, mutating || requires_candidates)) {
    return fail(error, "required hashes must be exactly 64 hexadecimal digits");
  }
  if ((mutating || requires_candidates) && !fixed_original_hash(arguments.expected_original_hash)) {
    return fail(error, "expected-original-hash does not equal the compiled approved baseline");
  }
  if (const auto value = get(L"--baseline-digest")) {
    arguments.baseline_digest = hotfix::hash_from_hex(utf8(*value));
    if (!arguments.baseline_digest) {
      return fail(error, "baseline-digest must be exactly 64 hexadecimal digits");
    }
  }
  if (mutating && !arguments.baseline_digest) {
    return fail(error, "mutating commands require baseline-digest");
  }
  return true;
}

bool read_all(HANDLE file, const std::uint64_t size, std::vector<std::uint8_t> &bytes, std::string &error) {
  if (size > maximum_artifact_bytes || size > std::numeric_limits<std::size_t>::max()) {
    return fail(error, "artifact exceeds the 512 MiB safety bound");
  }
  LARGE_INTEGER zero {};
  if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
    return fail(error, "cannot seek artifact: " + windows_error());
  }
  bytes.resize(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
    DWORD count = 0;
    if (!ReadFile(file, bytes.data() + offset, request, &count, nullptr) || count == 0) {
      return fail(error, "cannot read artifact: " + windows_error());
    }
    offset += count;
  }
  return true;
}

bool canonical_path(HANDLE file, std::wstring &path, std::string &error) {
  const auto required = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (required == 0 || required > 32768) {
    return fail(error, "cannot resolve canonical path: " + windows_error());
  }
  std::wstring buffer(required, L'\0');
  const auto written = GetFinalPathNameByHandleW(file, buffer.data(), required, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= required) {
    return fail(error, "cannot resolve canonical path: " + windows_error());
  }
  buffer.resize(written);
  constexpr std::wstring_view prefix = L"\\\\?\\";
  if (buffer.rfind(prefix, 0) == 0) {
    buffer.erase(0, prefix.size());
  }
  path = std::move(buffer);
  return true;
}

std::optional<std::wstring> lexical_full_path(const std::wstring &path, std::string &error) {
  const auto required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0 || required > 32768) {
    fail(error, "cannot resolve lexical full path: " + windows_error());
    return std::nullopt;
  }
  std::wstring buffer(required, L'\0');
  const auto written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
  if (written == 0 || written >= required) {
    fail(error, "cannot resolve lexical full path: " + windows_error());
    return std::nullopt;
  }
  buffer.resize(written);
  constexpr std::wstring_view prefix = L"\\\\?\\";
  if (buffer.rfind(prefix, 0) == 0) {
    buffer.erase(0, prefix.size());
  }
  return buffer;
}

bool secure_file_shape(HANDLE file, std::string &error) {
  FILE_ATTRIBUTE_TAG_INFO attributes {};
  if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes))) {
    return fail(error, "cannot query file attributes: " + windows_error());
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(error, "reparse-point artifacts are forbidden");
  }
  BY_HANDLE_FILE_INFORMATION information {};
  if (!GetFileInformationByHandle(file, &information)) {
    return fail(error, "cannot query link count: " + windows_error());
  }
  if (information.nNumberOfLinks != 1) {
    return fail(error, "multi-link artifacts are forbidden");
  }
  std::vector<std::uint8_t> streams(64U * 1024U);
  if (!GetFileInformationByHandleEx(file, FileStreamInfo, streams.data(), static_cast<DWORD>(streams.size()))) {
    return fail(error, "cannot query file streams: " + windows_error());
  }
  const auto *stream = reinterpret_cast<const FILE_STREAM_INFO *>(streams.data());
  std::size_t count = 0;
  while (stream) {
    ++count;
    const std::wstring_view name(stream->StreamName, stream->StreamNameLength / sizeof(wchar_t));
    if (name != L"::$DATA") {
      return fail(error, "alternate data streams are forbidden");
    }
    if (stream->NextEntryOffset == 0) {
      break;
    }
    if (stream->NextEntryOffset > streams.size()) {
      return fail(error, "invalid stream enumeration");
    }
    stream = reinterpret_cast<const FILE_STREAM_INFO *>(reinterpret_cast<const std::uint8_t *>(stream) + stream->NextEntryOffset);
  }
  if (count != 1) {
    return fail(error, "artifact must have exactly one unnamed data stream");
  }
  return true;
}

bool file_identity(HANDLE file, hotfix::file_identity_t &identity, std::uint64_t &size, std::string &error) {
  FILE_ID_INFO id {};
  FILE_STANDARD_INFO standard {};
  if (!GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id)) ||
      !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard))) {
    return fail(error, "cannot query file identity: " + windows_error());
  }
  if (standard.Directory || standard.EndOfFile.QuadPart < 0) {
    return fail(error, "expected a regular file");
  }
  identity.volume_serial = id.VolumeSerialNumber;
  std::copy_n(id.FileId.Identifier, identity.file_id.size(), identity.file_id.begin());
  size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
  return true;
}

std::optional<file_record_t> open_artifact(
  const std::wstring &path,
  const std::optional<std::wstring> &required_canonical,
  const bool allow_absent,
  std::string &error,
  const bool share_delete = true
) {
  handle_t handle(CreateFileW(
    path.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL,
    FILE_SHARE_READ | (share_delete ? FILE_SHARE_DELETE : 0), nullptr, OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  if (!handle.valid()) {
    if (allow_absent && GetLastError() == ERROR_FILE_NOT_FOUND) {
      return file_record_t {};
    }
    fail(error, "cannot open artifact " + utf8(path) + ": " + windows_error());
    return std::nullopt;
  }
  if (!secure_file_shape(handle.get(), error)) {
    return std::nullopt;
  }
  file_record_t record;
  record.handle = std::move(handle);
  const auto lexical = required_canonical ? required_canonical : lexical_full_path(path, error);
  if (!lexical || !canonical_path(record.handle.get(), record.canonical_path, error) ||
      lower(record.canonical_path) != lower(*lexical) ||
      !file_identity(record.handle.get(), record.identity, record.size, error)) {
    if (error.empty()) {
      fail(error, "canonical artifact path mismatch");
    }
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  if (!read_all(record.handle.get(), record.size, bytes, error)) {
    return std::nullopt;
  }
  record.hash = hotfix::sha256(bytes);
  return record;
}

bool write_bytes(HANDLE file, const std::vector<std::uint8_t> &bytes, std::string &error) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto request = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024U * 1024U));
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr) || written != request) {
      return fail(error, "write failed: " + windows_error());
    }
    offset += written;
  }
  return true;
}

bool flush_volume(std::string &error) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  static_cast<void>(error);
  return true;
#else
  handle_t volume(CreateFileW(L"\\\\.\\C:", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!volume.valid() || !FlushFileBuffers(volume.get())) {
    return fail(error, "volume flush failed: " + windows_error());
  }
  return true;
#endif
}

bool make_security_attributes(const wchar_t *sddl, SECURITY_ATTRIBUTES &attributes, local_memory_t &memory, std::string &error);
bool verify_protected_acl(const std::wstring &path, bool directory, std::string &error);

bool copy_from_handle(
  HANDLE source,
  const std::uint64_t size,
  const std::wstring &destination,
  const hotfix::hash256_t &expected,
  const char *copy_failpoint,
  const char *mid_copy_failpoint,
  const char *flush_failpoint,
  std::string &error
) {
  std::vector<std::uint8_t> bytes;
  if (!read_all(source, size, bytes, error)) {
    return false;
  }
  local_memory_t descriptor;
  SECURITY_ATTRIBUTES attributes {};
  if (!make_security_attributes(
        L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)", attributes, descriptor, error)) {
    return false;
  }
  handle_t output(CreateFileW(destination.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                             FILE_SHARE_READ, &attributes, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!output.valid()) {
    if (GetLastError() == ERROR_FILE_EXISTS) {
      auto existing = open_artifact(destination, destination, false, error);
      return existing && existing->hash == expected && verify_protected_acl(destination, false, error);
    }
    return fail(error, "cannot create staged artifact: " + windows_error());
  }
  failpoint(copy_failpoint);
  if (!write_bytes(output.get(), bytes, error)) {
    return false;
  }
  failpoint(mid_copy_failpoint);
  if (!FlushFileBuffers(output.get())) {
    return fail(error, "staged artifact flush failed: " + windows_error());
  }
  failpoint(flush_failpoint);
  LARGE_INTEGER zero {};
  if (!SetFilePointerEx(output.get(), zero, nullptr, FILE_BEGIN)) {
    return fail(error, "cannot rewind staged artifact: " + windows_error());
  }
  std::vector<std::uint8_t> reread;
  if (!read_all(output.get(), size, reread, error) || hotfix::sha256(reread) != expected) {
    return fail(error, "staged artifact verification failed");
  }
  return verify_protected_acl(destination, false, error);
}

bool make_security_attributes(const wchar_t *sddl, SECURITY_ATTRIBUTES &attributes, local_memory_t &memory, std::string &error) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
    return fail(error, "invalid built-in security descriptor: " + windows_error());
  }
  memory.value = descriptor;
  attributes = {sizeof(attributes), descriptor, FALSE};
  return true;
}

bool verify_protected_handle_acl(HANDLE object, const SE_OBJECT_TYPE object_type, std::string &error) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  PACL dacl = nullptr;
  PSID owner = nullptr;
  const auto status = GetSecurityInfo(object, object_type,
                                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                      &owner, nullptr, &dacl, nullptr, &descriptor);
  local_memory_t descriptor_memory {descriptor};
  if (status != ERROR_SUCCESS || !owner || !dacl) {
    return fail(error, "cannot query protected ACL: " + windows_error(status));
  }
  local_memory_t system_sid;
  local_memory_t admin_sid;
  local_memory_t trusted_installer_sid;
  if (!ConvertStringSidToSidW(L"S-1-5-18", reinterpret_cast<PSID *>(&system_sid.value)) ||
      !ConvertStringSidToSidW(L"S-1-5-32-544", reinterpret_cast<PSID *>(&admin_sid.value)) ||
      !ConvertStringSidToSidW(
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
        reinterpret_cast<PSID *>(&trusted_installer_sid.value))) {
    return fail(error, "cannot construct trusted principals: " + windows_error());
  }
  if (!EqualSid(owner, system_sid.value) && !EqualSid(owner, admin_sid.value) &&
      !EqualSid(owner, trusted_installer_sid.value)) {
    return fail(error, "protected object owner is not SYSTEM, Administrators, or TrustedInstaller");
  }
  bool system_full = false;
  bool admins_full = false;
  const auto full_access = object_type == SE_KERNEL_OBJECT ? MUTEX_ALL_ACCESS : FILE_ALL_ACCESS;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void *raw = nullptr;
    if (!GetAce(dacl, index, &raw)) {
      return fail(error, "cannot inspect protected ACL");
    }
    const auto *header = static_cast<ACE_HEADER *>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERIT_ONLY_ACE) != 0) {
      continue;
    }
    const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
    auto *sid = const_cast<DWORD *>(&ace->SidStart);
    auto dangerous_mask = GENERIC_ALL | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER | DELETE;
    dangerous_mask |= object_type == SE_KERNEL_OBJECT ? MUTEX_MODIFY_STATE :
                      (FILE_DELETE_CHILD | FILE_WRITE_DATA | FILE_APPEND_DATA);
    const auto dangerous = ace->Mask & dangerous_mask;
    if (EqualSid(sid, system_sid.value)) {
      system_full = (ace->Mask & GENERIC_ALL) != 0 || (ace->Mask & full_access) == full_access;
    } else if (EqualSid(sid, admin_sid.value)) {
      admins_full = (ace->Mask & GENERIC_ALL) != 0 || (ace->Mask & full_access) == full_access;
    } else if (EqualSid(sid, trusted_installer_sid.value)) {
      continue;
    } else if (dangerous != 0) {
      return fail(error, "untrusted principal can modify protected object");
    }
  }
  if (!system_full || !admins_full) {
    return fail(error, "protected ACL lacks SYSTEM/Administrators full control");
  }
  return true;
}

bool verify_protected_acl(const std::wstring &path, const bool directory, std::string &error) {
  handle_t handle(CreateFileW(
    path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
    FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  if (!handle.valid()) {
    return fail(error, "cannot open protected object: " + windows_error());
  }
  FILE_ATTRIBUTE_TAG_INFO attributes {};
  if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return fail(error, "protected object is a reparse point or unreadable");
  }
  return verify_protected_handle_acl(handle.get(), SE_FILE_OBJECT, error);
}

bool ensure_secure_directory(const std::wstring &path, std::string &error) {
  local_memory_t descriptor;
  SECURITY_ATTRIBUTES attributes {};
  if (!make_security_attributes(L"O:BAG:SYD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)", attributes, descriptor, error)) {
    return false;
  }
  if (!CreateDirectoryW(path.c_str(), &attributes) && GetLastError() != ERROR_ALREADY_EXISTS) {
    return fail(error, "cannot create protected directory: " + windows_error());
  }
  return verify_protected_acl(path, true, error);
}

struct directory_entry_t {
  std::wstring name;
  DWORD attributes {};
};

std::optional<std::vector<directory_entry_t>> enumerate_directory_handle(
  const std::wstring &path,
  std::string &error
) {
  handle_t directory(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!directory.valid()) {
    fail(error, "cannot open namespace directory: " + windows_error());
    return std::nullopt;
  }
  FILE_ATTRIBUTE_TAG_INFO attributes {};
  if (!GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    fail(error, "namespace directory is a reparse point or unreadable");
    return std::nullopt;
  }
  std::vector<directory_entry_t> entries;
  std::vector<std::uint8_t> buffer(64U * 1024U);
  bool restart = true;
  for (;;) {
    const auto information_class = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
    if (!GetFileInformationByHandleEx(directory.get(), information_class, buffer.data(), static_cast<DWORD>(buffer.size()))) {
      const auto query_error = GetLastError();
      if (query_error == ERROR_NO_MORE_FILES) {
        break;
      }
      fail(error, "handle-based namespace enumeration failed: " + windows_error(query_error));
      return std::nullopt;
    }
    restart = false;
    auto *entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(buffer.data());
    for (;;) {
      std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        entries.push_back({std::move(name), entry->FileAttributes});
      }
      if (entry->NextEntryOffset == 0) {
        break;
      }
      entry = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(reinterpret_cast<std::uint8_t *>(entry) + entry->NextEntryOffset);
    }
  }
  return entries;
}

bool verify_exact_namespace(const std::wstring &directory, std::string &error) {
  const auto root_entries = enumerate_directory_handle(transaction_root, error);
  const auto transaction_entries = enumerate_directory_handle(directory, error);
  if (!root_entries || !transaction_entries) {
    return false;
  }
  for (const auto &entry : *root_entries) {
    const auto id = utf8(entry.name);
    if (!hotfix::valid_transaction_id(id) ||
        (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return fail(error, "transaction root contains an unknown object: " + utf8(entry.name));
    }
  }
  const std::set<std::wstring> allowed {
    receipt_name,
    main_stage_name,
    helper_stage_name,
    main_snapshot_name,
    helper_snapshot_name,
    main_live_backup_name,
    helper_live_backup_name,
    failed_main_name,
    failed_helper_name,
    rollback_main_stage_name,
  };
  for (const auto &entry : *transaction_entries) {
    if (!allowed.contains(entry.name) ||
        (entry.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      return fail(error, "transaction namespace contains an unknown object: " + utf8(entry.name));
    }
  }
  return true;
}

bool exact_path_exists(const std::wstring &path, std::string &error) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return false;
    }
    fail(error, "cannot inspect exact transaction path: " + windows_error());
    return false;
  }
  if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    fail(error, "exact transaction path is not a regular non-reparse file");
    return false;
  }
  return true;
}

std::vector<std::uint8_t> encode_u32(const DWORD value) {
  return {
    static_cast<std::uint8_t>(value & 0xffU),
    static_cast<std::uint8_t>((value >> 8U) & 0xffU),
    static_cast<std::uint8_t>((value >> 16U) & 0xffU),
    static_cast<std::uint8_t>((value >> 24U) & 0xffU),
  };
}

void append_u32(std::vector<std::uint8_t> &bytes, const DWORD value) {
  const auto encoded = encode_u32(value);
  bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

void append_wide(std::vector<std::uint8_t> &bytes, const wchar_t *value) {
  const auto text = value ? utf8(value) : std::string {};
  append_u32(bytes, static_cast<DWORD>(text.size()));
  bytes.insert(bytes.end(), text.begin(), text.end());
}

bool buffer_contains(
  const std::vector<std::uint8_t> &buffer,
  const void *pointer,
  const std::size_t bytes
) noexcept {
  if (!pointer) {
    return bytes == 0;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(buffer.data());
  const auto end = start + buffer.size();
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  return address >= start && address <= end && bytes <= end - address;
}

bool bounded_wide_string(const std::vector<std::uint8_t> &buffer, const wchar_t *value) noexcept {
  if (!value) {
    return true;
  }
  if (!buffer_contains(buffer, value, sizeof(wchar_t))) {
    return false;
  }
  const auto end = reinterpret_cast<std::uintptr_t>(buffer.data()) + buffer.size();
  for (auto current = value; reinterpret_cast<std::uintptr_t>(current) + sizeof(wchar_t) <= end; ++current) {
    if (*current == L'\0') {
      return true;
    }
  }
  return false;
}

[[maybe_unused]] bool query_config2(SC_HANDLE service, const DWORD level, hotfix::optional_blob_t &field, std::string &error) {
  DWORD required = 0;
  SetLastError(ERROR_SUCCESS);
  if (!QueryServiceConfig2W(service, level, nullptr, 0, &required)) {
    const auto query_error = GetLastError();
    if (query_error == ERROR_INVALID_LEVEL || query_error == ERROR_INVALID_PARAMETER || query_error == ERROR_CALL_NOT_IMPLEMENTED) {
      field = {};
      return true;
    }
    if (query_error != ERROR_INSUFFICIENT_BUFFER) {
      return fail(error, "QueryServiceConfig2 failed: " + windows_error(query_error));
    }
  }
  std::vector<std::uint8_t> buffer(required);
  if (!QueryServiceConfig2W(service, level, buffer.data(), required, &required)) {
    return fail(error, "QueryServiceConfig2 read failed: " + windows_error());
  }
  field.availability = hotfix::field_availability_t::present;
  auto &value = field.value;
  switch (level) {
    case SERVICE_CONFIG_FAILURE_ACTIONS: {
      if (buffer.size() < sizeof(SERVICE_FAILURE_ACTIONSW)) {
        return fail(error, "failure actions payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_FAILURE_ACTIONSW *>(buffer.data());
      if (!bounded_wide_string(buffer, info->lpRebootMsg) ||
          !bounded_wide_string(buffer, info->lpCommand) ||
          info->cActions > 1024U ||
          !buffer_contains(buffer, info->lpsaActions, static_cast<std::size_t>(info->cActions) * sizeof(SC_ACTION))) {
        return fail(error, "failure-actions pointers/count are outside the returned buffer");
      }
      append_u32(value, info->dwResetPeriod);
      append_wide(value, info->lpRebootMsg);
      append_wide(value, info->lpCommand);
      append_u32(value, info->cActions);
      if (info->cActions != 0 && !info->lpsaActions) {
        return fail(error, "failure actions returned a null action array");
      }
      for (DWORD index = 0; index < info->cActions; ++index) {
        append_u32(value, info->lpsaActions[index].Type);
        append_u32(value, info->lpsaActions[index].Delay);
      }
      break;
    }
    case SERVICE_CONFIG_FAILURE_ACTIONS_FLAG: {
      if (buffer.size() < sizeof(SERVICE_FAILURE_ACTIONS_FLAG)) {
        return fail(error, "failure-actions flag payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_FAILURE_ACTIONS_FLAG *>(buffer.data());
      value = encode_u32(info->fFailureActionsOnNonCrashFailures ? 1U : 0U);
      break;
    }
    case SERVICE_CONFIG_DELAYED_AUTO_START_INFO: {
      if (buffer.size() < sizeof(SERVICE_DELAYED_AUTO_START_INFO)) {
        return fail(error, "delayed-start payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_DELAYED_AUTO_START_INFO *>(buffer.data());
      value = encode_u32(info->fDelayedAutostart ? 1U : 0U);
      break;
    }
    case SERVICE_CONFIG_SERVICE_SID_INFO: {
      if (buffer.size() < sizeof(SERVICE_SID_INFO)) {
        return fail(error, "service SID payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_SID_INFO *>(buffer.data());
      value = encode_u32(info->dwServiceSidType);
      break;
    }
    case SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO: {
      if (buffer.size() < sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW)) {
        return fail(error, "required-privileges payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_REQUIRED_PRIVILEGES_INFOW *>(buffer.data());
      const wchar_t *current = info->pmszRequiredPrivileges;
      while (current) {
        if (!buffer_contains(buffer, current, sizeof(wchar_t))) {
          return fail(error, "required-privileges MULTI_SZ is outside the returned buffer");
        }
        if (*current == L'\0') {
          break;
        }
        if (!bounded_wide_string(buffer, current)) {
          return fail(error, "required-privileges MULTI_SZ is outside the returned buffer");
        }
        append_wide(value, current);
        current += wcslen(current) + 1;
      }
      append_u32(value, 0);
      break;
    }
    case SERVICE_CONFIG_PRESHUTDOWN_INFO: {
      if (buffer.size() < sizeof(SERVICE_PRESHUTDOWN_INFO)) {
        return fail(error, "preshutdown payload is truncated");
      }
      const auto *info = reinterpret_cast<const SERVICE_PRESHUTDOWN_INFO *>(buffer.data());
      value = encode_u32(info->dwPreshutdownTimeout);
      break;
    }
    case SERVICE_CONFIG_TRIGGER_INFO: {
      if (buffer.size() < sizeof(service_trigger_info_abi_t)) {
        return fail(error, "trigger payload is truncated");
      }
      const auto *info = reinterpret_cast<const service_trigger_info_abi_t *>(buffer.data());
      if (info->cTriggers > 1024U ||
          !buffer_contains(buffer, info->pTriggers,
                           static_cast<std::size_t>(info->cTriggers) * sizeof(service_trigger_abi_t))) {
        return fail(error, "trigger array is outside the returned buffer");
      }
      append_u32(value, info->cTriggers);
      if (info->cTriggers != 0 && !info->pTriggers) {
        return fail(error, "trigger info returned a null trigger array");
      }
      for (DWORD index = 0; index < info->cTriggers; ++index) {
        const auto &trigger = info->pTriggers[index];
        append_u32(value, trigger.dwTriggerType);
        append_u32(value, trigger.dwAction);
        if (trigger.pTriggerSubtype && !buffer_contains(buffer, trigger.pTriggerSubtype, sizeof(GUID))) {
          return fail(error, "trigger subtype is outside the returned buffer");
        }
        append_u32(value, trigger.pTriggerSubtype ? sizeof(GUID) : 0);
        if (trigger.pTriggerSubtype) {
          const auto *guid = reinterpret_cast<const std::uint8_t *>(trigger.pTriggerSubtype);
          value.insert(value.end(), guid, guid + sizeof(GUID));
        }
        append_u32(value, trigger.cDataItems);
        if (trigger.cDataItems > 4096U ||
            !buffer_contains(buffer, trigger.pDataItems,
                             static_cast<std::size_t>(trigger.cDataItems) * sizeof(service_trigger_specific_data_item_abi_t))) {
          return fail(error, "trigger data array is outside the returned buffer");
        }
        for (DWORD item = 0; item < trigger.cDataItems; ++item) {
          append_u32(value, trigger.pDataItems[item].dwDataType);
          append_u32(value, trigger.pDataItems[item].cbData);
          if (trigger.pDataItems[item].cbData > hotfix::k_max_scm_baseline_bytes ||
              !buffer_contains(buffer, trigger.pDataItems[item].pData, trigger.pDataItems[item].cbData)) {
            return fail(error, "trigger item data is outside the returned buffer");
          }
          value.insert(value.end(), trigger.pDataItems[item].pData,
                       trigger.pDataItems[item].pData + trigger.pDataItems[item].cbData);
        }
      }
      break;
    }
    case SERVICE_CONFIG_PREFERRED_NODE: {
      if (buffer.size() < sizeof(service_preferred_node_info_abi_t)) {
        return fail(error, "preferred-node payload is truncated");
      }
      const auto *info = reinterpret_cast<const service_preferred_node_info_abi_t *>(buffer.data());
      append_u32(value, info->preferred_node);
      append_u32(value, info->delete_preference ? 1U : 0U);
      break;
    }
    case SERVICE_CONFIG_LAUNCH_PROTECTED: {
      if (buffer.size() < sizeof(service_launch_protected_info_abi_t)) {
        return fail(error, "launch-protection payload is truncated");
      }
      const auto *info = reinterpret_cast<const service_launch_protected_info_abi_t *>(buffer.data());
      value = encode_u32(info->launch_protected);
      break;
    }
    default:
      value = std::move(buffer);
      break;
  }
  return true;
}

[[maybe_unused]] bool security_sddl(HANDLE object, const SE_OBJECT_TYPE type, std::vector<std::uint8_t> &encoded, std::string &error) {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto status = GetSecurityInfo(object, type, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, nullptr, nullptr, &descriptor);
  local_memory_t descriptor_memory {descriptor};
  if (status != ERROR_SUCCESS || !descriptor) {
    return fail(error, "cannot query owner/DACL: " + windows_error(status));
  }
  wchar_t *text = nullptr;
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
        descriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &text, nullptr)) {
    return fail(error, "cannot canonicalize owner/DACL: " + windows_error());
  }
  local_memory_t text_memory {text};
  const auto converted = utf8(text);
  encoded.assign(converted.begin(), converted.end());
  return true;
}

[[maybe_unused]] std::vector<std::string> parse_multisz(const wchar_t *value) {
  std::vector<std::string> items;
  for (auto current = value; current && *current; current += wcslen(current) + 1) {
    items.push_back(utf8(current));
  }
  return items;
}

bool query_scm_baseline(
  service_handle_t &manager,
  service_handle_t &service,
  hotfix::scm_semantic_baseline_t &baseline,
  const bool mutating_access,
  std::string &error
) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  static_cast<void>(mutating_access);
  fake_scm_state_t state;
  if (test_root.empty() || !read_fake_scm(state, error)) {
    return fail(error, error.empty() ? "test fixture root is missing" : error);
  }
  manager = service_handle_t(reinterpret_cast<SC_HANDLE>(1));
  service = service_handle_t(reinterpret_cast<SC_HANDLE>(2));
  baseline.account = "LocalSystem";
  baseline.binary_path = utf8(service_command);
  baseline.service_type = SERVICE_WIN32_OWN_PROCESS;
  baseline.start_type = SERVICE_AUTO_START;
  baseline.error_control = SERVICE_ERROR_NORMAL;
  baseline.dependencies = {};
  baseline.failure_actions.availability = hotfix::field_availability_t::present;
  baseline.failure_actions.value = {0, 0, 0, 0};
  baseline.failure_actions_flag.availability = hotfix::field_availability_t::present;
  baseline.failure_actions_flag.value = {0, 0, 0, 0};
  baseline.delayed_start.availability = hotfix::field_availability_t::present;
  baseline.delayed_start.value = {0, 0, 0, 0};
  baseline.sid_type.availability = hotfix::field_availability_t::present;
  baseline.sid_type.value = {0, 0, 0, 0};
  baseline.required_privileges.availability = hotfix::field_availability_t::present;
  baseline.required_privileges.value = {0, 0, 0, 0};
  baseline.preshutdown_timeout.availability = hotfix::field_availability_t::present;
  baseline.preshutdown_timeout.value = {0, 0, 0, 0};
  baseline.triggers.availability = hotfix::field_availability_t::present;
  baseline.triggers.value = encode_u32(state.semantic_epoch);
  baseline.preferred_node.availability = hotfix::field_availability_t::unsupported;
  baseline.managed_account.availability = hotfix::field_availability_t::unsupported;
  baseline.launch_protection.availability = hotfix::field_availability_t::present;
  baseline.launch_protection.value = {0, 0, 0, 0};
  baseline.service_security_owner_dacl = {'O', ':', 'B', 'A', 'D', ':', 'P'};
  baseline.registry_security_owner_dacl = {'O', ':', 'B', 'A', 'D', ':', 'P'};
  return true;
#else
  manager = service_handle_t(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
  if (!manager.valid()) {
    return fail(error, "cannot open SCM: " + windows_error());
  }
  auto access = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | READ_CONTROL;
  if (mutating_access) {
    access |= SERVICE_START | SERVICE_STOP;
  }
  service = service_handle_t(OpenServiceW(manager.get(), service_name, access));
  if (!service.valid()) {
    return fail(error, "cannot open ApolloService: " + windows_error());
  }
  DWORD required = 0;
  QueryServiceConfigW(service.get(), nullptr, 0, &required);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0) {
    return fail(error, "cannot size service configuration: " + windows_error());
  }
  std::vector<std::uint8_t> buffer(required);
  auto *config = reinterpret_cast<QUERY_SERVICE_CONFIGW *>(buffer.data());
  if (!QueryServiceConfigW(service.get(), config, required, &required)) {
    return fail(error, "cannot read service configuration: " + windows_error());
  }
  baseline.account = utf8(config->lpServiceStartName ? config->lpServiceStartName : L"");
  baseline.binary_path = utf8(config->lpBinaryPathName ? config->lpBinaryPathName : L"");
  baseline.service_type = config->dwServiceType;
  baseline.start_type = config->dwStartType;
  baseline.error_control = config->dwErrorControl;
  baseline.dependencies = parse_multisz(config->lpDependencies);
  if (lower(config->lpBinaryPathName ? config->lpBinaryPathName : L"") != lower(service_command) ||
      lower(config->lpServiceStartName ? config->lpServiceStartName : L"") != L"localsystem") {
    return fail(error, "ApolloService path/account does not match the fixed contract");
  }

  if (!query_config2(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, baseline.failure_actions, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, baseline.failure_actions_flag, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, baseline.delayed_start, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_SERVICE_SID_INFO, baseline.sid_type, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, baseline.required_privileges, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_PRESHUTDOWN_INFO, baseline.preshutdown_timeout, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_TRIGGER_INFO, baseline.triggers, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_PREFERRED_NODE, baseline.preferred_node, error) ||
      !query_config2(service.get(), SERVICE_CONFIG_LAUNCH_PROTECTED, baseline.launch_protection, error)) {
    return false;
  }
  baseline.managed_account.availability = hotfix::field_availability_t::unsupported;
  if (!security_sddl(service.get(), SE_SERVICE, baseline.service_security_owner_dacl, error)) {
    return false;
  }
  HKEY registry_key = nullptr;
  const auto registry_status = RegOpenKeyExW(
    HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ApolloService", 0,
    READ_CONTROL | KEY_QUERY_VALUE, &registry_key);
  if (registry_status != ERROR_SUCCESS) {
    return fail(error, "cannot open service registry key: " + windows_error(registry_status));
  }
  const auto registry_ok = security_sddl(registry_key, SE_REGISTRY_KEY, baseline.registry_security_owner_dacl, error);
  RegCloseKey(registry_key);
  return registry_ok;
#endif
}

bool query_service_status(SC_HANDLE service, SERVICE_STATUS_PROCESS &status, std::string &error) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  static_cast<void>(service);
  fake_scm_state_t state;
  if (!read_fake_scm(state, error)) {
    return false;
  }
  status = {};
  status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  status.dwCurrentState = state.running ? SERVICE_RUNNING : SERVICE_STOPPED;
  status.dwProcessId = state.running ? 4242U : 0U;
  return true;
#else
  DWORD bytes = 0;
  if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE *>(&status), sizeof(status), &bytes)) {
    return fail(error, "cannot query service status: " + windows_error());
  }
  return true;
#endif
}

[[maybe_unused]] bool wait_for_service(SC_HANDLE service, const DWORD desired, const DWORD timeout, std::string &error) {
  const auto deadline = GetTickCount64() + timeout;
  SERVICE_STATUS_PROCESS status {};
  while (GetTickCount64() < deadline) {
    if (!query_service_status(service, status, error)) {
      return false;
    }
    if (status.dwCurrentState == desired) {
      return true;
    }
    if (status.dwCurrentState != SERVICE_START_PENDING && status.dwCurrentState != SERVICE_STOP_PENDING) {
      return fail(error, "service entered unexpected state " + std::to_string(status.dwCurrentState));
    }
    Sleep(100);
  }
  return fail(error, "service transition timed out");
}

bool stop_service(SC_HANDLE service, std::string &error) {
  SERVICE_STATUS_PROCESS status {};
  if (!query_service_status(service, status, error)) {
    return false;
  }
  if (status.dwCurrentState == SERVICE_STOPPED) {
    return true;
  }
  if (status.dwCurrentState != SERVICE_RUNNING) {
    return fail(error, "service is not in a stoppable running state");
  }
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  failpoint("before-stop");
  fake_scm_state_t state;
  if (!read_fake_scm(state, error)) {
    return false;
  }
  state.running = 0;
  state.last_stopped_sunshine_pid = state.sunshine_pid;
  if (!state.linger_sunshine_on_stop) {
    state.sunshine_pid = 0;
  }
  ++state.stop_count;
  if (!write_fake_scm(state, error)) {
    return false;
  }
  failpoint("after-stop-request");
  return true;
#else
  SERVICE_STATUS ignored {};
  failpoint("before-stop");
  if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored)) {
    return fail(error, "service stop request failed: " + windows_error());
  }
  failpoint("after-stop-request");
  return wait_for_service(service, SERVICE_STOPPED, service_transition_timeout_ms, error);
#endif
}

bool start_service(SC_HANDLE service, std::string &error) {
  SERVICE_STATUS_PROCESS status {};
  if (!query_service_status(service, status, error)) {
    return false;
  }
  if (status.dwCurrentState == SERVICE_RUNNING) {
    return true;
  }
  if (status.dwCurrentState != SERVICE_STOPPED) {
    return fail(error, "service is not in a startable stopped state");
  }
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  failpoint("before-start");
  fake_scm_state_t state;
  if (!read_fake_scm(state, error)) {
    return false;
  }
  state.running = 1;
  if (state.reuse_sunshine_pid_on_start && state.last_stopped_sunshine_pid != 0) {
    state.sunshine_pid = state.last_stopped_sunshine_pid;
  } else {
    ++state.next_sunshine_pid;
    if (state.next_sunshine_pid == 0) {
      ++state.next_sunshine_pid;
    }
    state.sunshine_pid = state.next_sunshine_pid;
  }
  ++state.start_count;
  if (!write_fake_scm(state, error)) {
    return false;
  }
  failpoint("after-start-request");
  return true;
#else
  failpoint("before-start");
  if (!StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
    return fail(error, "service start request failed: " + windows_error());
  }
  failpoint("after-start-request");
  return wait_for_service(service, SERVICE_RUNNING, service_transition_timeout_ms, error);
#endif
}

bool validate_file_record(const file_record_t &record, const hotfix::hash256_t &hash, const std::uint64_t volume, std::string &error) {
  if (!record.handle.valid() || record.hash != hash || (volume != 0 && record.identity.volume_serial != volume)) {
    return fail(error, "artifact hash/volume contract mismatch: " + utf8(record.canonical_path));
  }
  return true;
}

bool create_mutex(handle_t &mutex, std::string &error) {
  local_memory_t descriptor;
  SECURITY_ATTRIBUTES attributes {};
  if (!make_security_attributes(L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)", attributes, descriptor, error)) {
    return false;
  }
  mutex = handle_t(CreateMutexW(&attributes, FALSE, global_mutex_name.c_str()));
  if (!mutex.valid()) {
    return fail(error, "cannot create/open deployer mutex: " + windows_error());
  }
  if (!verify_protected_handle_acl(mutex.get(), SE_KERNEL_OBJECT, error)) {
    return false;
  }
  const auto wait = WaitForSingleObject(mutex.get(), 0);
  if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
    return fail(error, wait == WAIT_TIMEOUT ? "another deployer owns the global mutex" : "mutex wait failed: " + windows_error());
  }
  return true;
}

bool immutable_receipt_write(const std::wstring &path, const std::vector<std::uint8_t> &bytes, std::string &error) {
  local_memory_t descriptor;
  SECURITY_ATTRIBUTES attributes {};
  if (!make_security_attributes(
        L"O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)", attributes, descriptor, error)) {
    return false;
  }
  handle_t file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
                           FILE_SHARE_READ, &attributes, CREATE_NEW,
                           FILE_ATTRIBUTE_READONLY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid()) {
    return fail(error, "receipt CREATE_NEW failed: " + windows_error());
  }
  failpoint("receipt-create");
  if (!write_bytes(file.get(), bytes, error)) {
    return false;
  }
  failpoint("receipt-write");
  if (!FlushFileBuffers(file.get())) {
    return fail(error, "receipt flush failed: " + windows_error());
  }
  failpoint("receipt-flush");
  LARGE_INTEGER zero {};
  if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN)) {
    return fail(error, "receipt rewind failed: " + windows_error());
  }
  std::vector<std::uint8_t> reread;
  if (!read_all(file.get(), bytes.size(), reread, error) || reread != bytes || !hotfix::parse_receipt(reread).ok()) {
    return fail(error, "receipt reread verification failed");
  }
  failpoint("receipt-reread");
  if (!flush_volume(error)) {
    return false;
  }
  failpoint("receipt-volume-flush");
  return verify_protected_acl(path, false, error);
}

std::optional<hotfix::receipt_t> read_receipt(const std::wstring &path, std::string &error) {
  if (!verify_protected_acl(path, false, error)) {
    return std::nullopt;
  }
  auto file = open_artifact(path, path, false, error);
  if (!file) {
    return std::nullopt;
  }
  std::vector<std::uint8_t> bytes;
  if (!read_all(file->handle.get(), file->size, bytes, error)) {
    return std::nullopt;
  }
  auto parsed = hotfix::parse_receipt(bytes);
  if (!parsed.ok()) {
    fail(error, "receipt is invalid: " + parsed.error);
    return std::nullopt;
  }
  return parsed.value;
}

bool arguments_match_receipt(const arguments_t &arguments, const hotfix::receipt_t &receipt, std::string &error) {
  if (receipt.transaction_id != arguments.transaction_id ||
      receipt.expected_original_hash != arguments.expected_original_hash ||
      receipt.candidate_main_hash != arguments.candidate_main_hash ||
      receipt.candidate_helper_hash != arguments.candidate_helper_hash ||
      !arguments.baseline_digest || receipt.baseline_digest != *arguments.baseline_digest) {
    return fail(error, "arguments do not match the immutable receipt");
  }
  return true;
}

bool check_free_space(const hotfix::space_inputs_t &input, std::string &error) {
  const auto required = hotfix::required_free_space(input);
  if (!required) {
    return fail(error, "free-space calculation overflowed");
  }
  ULARGE_INTEGER available {};
  if (!GetDiskFreeSpaceExW(install_root.c_str(), &available, nullptr, nullptr)) {
    return fail(error, "cannot query free space: " + windows_error());
  }
  if (available.QuadPart < *required) {
    return fail(error, "insufficient free space; required=" + std::to_string(*required) + " available=" + std::to_string(available.QuadPart));
  }
  return true;
}

bool validate_common(
  const arguments_t &arguments,
  file_record_t &main,
  file_record_t &helper,
  file_record_t &service_binary,
  file_record_t &candidate_main,
  file_record_t &candidate_helper,
  hotfix::scm_semantic_baseline_t &baseline,
  std::vector<std::uint8_t> &baseline_bytes,
  hotfix::hash256_t &baseline_digest,
  service_handle_t &manager,
  service_handle_t &service,
  const bool mutating_access,
  std::string &error
) {
  auto opened_main = open_artifact(main_target, std::wstring(main_target), false, error);
  auto opened_helper = open_artifact(helper_target, std::wstring(helper_target), true, error);
  auto opened_service = open_artifact(service_executable, std::wstring(service_executable), false, error, false);
  auto opened_candidate_main = open_artifact(arguments.candidate_main, std::nullopt, false, error, false);
  auto opened_candidate_helper = open_artifact(arguments.candidate_helper, std::nullopt, false, error, false);
  if (!opened_main || !opened_helper || !opened_service || !opened_candidate_main || !opened_candidate_helper) {
    return false;
  }
  main = std::move(*opened_main);
  helper = std::move(*opened_helper);
  service_binary = std::move(*opened_service);
  candidate_main = std::move(*opened_candidate_main);
  candidate_helper = std::move(*opened_candidate_helper);
  if (!verify_protected_acl(install_root, true, error) ||
      !verify_protected_acl(tools_root, true, error) ||
      !verify_protected_acl(main_target, false, error) ||
      !verify_protected_acl(service_executable, false, error) ||
      (helper.handle.valid() && !verify_protected_acl(helper_target, false, error))) {
    return false;
  }
  if (!validate_file_record(main, arguments.expected_original_hash, 0, error) ||
      !validate_file_record(candidate_main, arguments.candidate_main_hash, main.identity.volume_serial, error) ||
      !validate_file_record(candidate_helper, arguments.candidate_helper_hash, main.identity.volume_serial, error) ||
      service_binary.identity.volume_serial != main.identity.volume_serial ||
      (helper.handle.valid() && helper.identity.volume_serial != main.identity.volume_serial)) {
    return fail(error, error.empty() ? "all fixed and candidate artifacts must share the install volume" : error);
  }
  if (!query_scm_baseline(manager, service, baseline, mutating_access, error)) {
    return false;
  }
  const auto serialized = hotfix::serialize_scm_baseline(baseline);
  if (!serialized.ok()) {
    return fail(error, "cannot serialize SCM baseline: " + serialized.error);
  }
  baseline_bytes = serialized.value;
  baseline_digest = hotfix::sha256(baseline_bytes);
  if (arguments.baseline_digest && baseline_digest != *arguments.baseline_digest) {
    return fail(error, "live SCM semantic digest differs from the supplied baseline digest");
  }
  hotfix::space_inputs_t space {
    .original_main_bytes = main.size,
    .original_helper_bytes = helper.handle.valid() ? helper.size : 0,
    .candidate_main_bytes = candidate_main.size,
    .candidate_helper_bytes = candidate_helper.size,
    .receipt_bytes = static_cast<std::uint64_t>(baseline_bytes.size() + 1024U),
    .safety_margin_bytes = safety_margin_bytes,
  };
  return check_free_space(space, error);
}

std::uint64_t filetime_value(const FILETIME &value) {
  return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
}

std::uint64_t current_filetime() {
  FILETIME now {};
  GetSystemTimeAsFileTime(&now);
  return filetime_value(now);
}

bool verify_process_handle(
  HANDLE process,
  const std::wstring &path,
  const hotfix::hash256_t &hash,
  const hotfix::file_identity_t &identity,
  const std::uint64_t minimum_creation_time,
  std::string &error
) {
  std::wstring image(32768, L'\0');
  DWORD count = static_cast<DWORD>(image.size());
  if (!QueryFullProcessImageNameW(process, 0, image.data(), &count)) {
    return fail(error, "cannot query process image: " + windows_error());
  }
  image.resize(count);
  if (lower(image) != lower(path)) {
    return fail(error, "process image path does not match the fixed executable");
  }
  auto opened = open_artifact(image, path, false, error);
  if (!opened || opened->hash != hash || opened->identity != identity) {
    return fail(error, "process image identity/hash mismatch");
  }
  if (minimum_creation_time != 0) {
    FILETIME creation {};
    FILETIME exit {};
    FILETIME kernel {};
    FILETIME user {};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user) ||
        filetime_value(creation) < minimum_creation_time) {
      return fail(error, "process was not created after the replacement boundary");
    }
  }
  return true;
}

bool verify_process_image(
  const DWORD process_id,
  const std::wstring &path,
  const hotfix::hash256_t &hash,
  const hotfix::file_identity_t &identity,
  std::string &error,
  const std::uint64_t minimum_creation_time = 0
) {
  handle_t process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
  if (!process.valid()) {
    return fail(error, "cannot open service process: " + windows_error());
  }
  return verify_process_handle(process.get(), path, hash, identity, minimum_creation_time, error);
}

[[maybe_unused]] std::set<std::uint16_t> tcp_listener_ports(const DWORD process_id, std::string &error) {
  DWORD bytes = 0;
  if (GetExtendedTcpTable(nullptr, &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER) {
    fail(error, "cannot size TCP listener table");
    return {};
  }
  std::vector<std::uint8_t> buffer(bytes);
  const auto status = GetExtendedTcpTable(buffer.data(), &bytes, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
  if (status != NO_ERROR) {
    fail(error, "cannot read TCP listener table: " + windows_error(status));
    return {};
  }
  std::set<std::uint16_t> ports;
  const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(buffer.data());
  for (DWORD index = 0; index < table->dwNumEntries; ++index) {
    if (table->table[index].dwOwningPid == process_id) {
      ports.insert(ntohs(static_cast<u_short>(table->table[index].dwLocalPort)));
    }
  }
  return ports;
}

[[maybe_unused]] std::optional<DWORD> sunshine_process(
  const hotfix::hash256_t &expected_hash,
  const hotfix::file_identity_t &expected_identity,
  std::string &error,
  const std::set<DWORD> &forbidden_pids = {},
  const std::uint64_t minimum_creation_time = 0
) {
  handle_t snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (!snapshot.valid()) {
    fail(error, "cannot enumerate processes: " + windows_error());
    return std::nullopt;
  }
  PROCESSENTRY32W entry {};
  entry.dwSize = sizeof(entry);
  DWORD match = 0;
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
    if (_wcsicmp(entry.szExeFile, L"sunshine.exe") != 0) {
      continue;
    }
    if (forbidden_pids.contains(entry.th32ProcessID)) {
      continue;
    }
    std::string candidate_error;
    if (verify_process_image(
          entry.th32ProcessID, main_target, expected_hash, expected_identity,
          candidate_error, minimum_creation_time)) {
      if (match != 0) {
        fail(error, "more than one exact sunshine process is running");
        return std::nullopt;
      }
      match = entry.th32ProcessID;
    }
  }
  if (match == 0) {
    fail(error, "no exact sunshine process is running");
    return std::nullopt;
  }
  return match;
}

struct baseline_processes_t {
  std::set<DWORD> pids;
  std::vector<handle_t> handles;
};

bool capture_baseline_sunshine_processes(
  const hotfix::receipt_t &receipt,
  baseline_processes_t &baseline,
  std::string &error
) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  fake_scm_state_t state;
  if (!read_fake_scm(state, error) || !state.running || state.sunshine_pid == 0) {
    return fail(error, error.empty() ? "fake baseline Sunshine process is not running" : error);
  }
  baseline.pids.insert(state.sunshine_pid);
  return true;
#else
  auto process_id = sunshine_process(
    receipt.expected_original_hash, receipt.original_main_identity, error);
  if (!process_id) {
    return false;
  }
  handle_t process(OpenProcess(
    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *process_id));
  if (!process.valid() ||
      !verify_process_handle(
        process.get(), main_target, receipt.expected_original_hash,
        receipt.original_main_identity, 0, error)) {
    return fail(error, error.empty() ? "cannot pin the baseline Sunshine process" : error);
  }
  baseline.pids.insert(*process_id);
  baseline.handles.push_back(std::move(process));
  return true;
#endif
}

bool wait_for_baseline_sunshine_exit(
  const baseline_processes_t &baseline,
  std::string &error
) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  fake_scm_state_t state;
  if (!read_fake_scm(state, error)) {
    return false;
  }
  if (state.sunshine_pid != 0 && baseline.pids.contains(state.sunshine_pid)) {
    return fail(error, "baseline Sunshine process remains alive after service stop");
  }
  return true;
#else
  const auto deadline = GetTickCount64() + service_transition_timeout_ms;
  for (;;) {
    const auto all_exited = std::all_of(
      baseline.handles.begin(), baseline.handles.end(),
      [](const auto &process) { return WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0; });
    if (all_exited) {
      return true;
    }
    if (GetTickCount64() >= deadline) {
      return fail(error, "baseline Sunshine process did not exit after bounded service stop");
    }
    Sleep(50);
  }
#endif
}

bool health_check(
  SC_HANDLE service,
  const hotfix::receipt_t &receipt,
  const bool candidate,
  std::string &error,
  const std::set<DWORD> &forbidden_sunshine_pids = {},
  const std::uint64_t minimum_creation_time = 0
) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  static_cast<void>(service);
  fake_scm_state_t state;
  if (!read_fake_scm(state, error) || !state.running || state.sunshine_pid == 0 ||
      !state.health_ok || !state.listeners_ok) {
    return fail(error, error.empty() ? "fake service health/listeners are not ready" : error);
  }
  auto service_file = open_artifact(service_executable, service_executable, false, error, false);
  auto main_file = open_artifact(main_target, main_target, false, error, false);
  const auto expected_hash = candidate ? receipt.candidate_main_hash : receipt.expected_original_hash;
  const auto expected_identity = candidate ? receipt.candidate_main_identity : receipt.original_main_identity;
  if (!service_file || !main_file || service_file->hash != receipt.service_executable_hash ||
      service_file->identity != receipt.service_executable_identity || main_file->hash != expected_hash ||
      (!candidate && main_file->identity != expected_identity) ||
      (candidate && forbidden_sunshine_pids.contains(state.sunshine_pid))) {
    return fail(error, "fake health exact image/FileId/hash predicate failed");
  }
  return true;
#else
  const auto deadline = GetTickCount64() + service_health_timeout_ms;
  do {
    SERVICE_STATUS_PROCESS status {};
    std::string attempt_error;
    if (query_service_status(service, status, attempt_error) && status.dwCurrentState == SERVICE_RUNNING && status.dwProcessId != 0 &&
        verify_process_image(status.dwProcessId, service_executable, receipt.service_executable_hash,
                             receipt.service_executable_identity, attempt_error,
                             candidate ? minimum_creation_time : 0)) {
      const auto expected_hash = candidate ? receipt.candidate_main_hash : receipt.expected_original_hash;
      auto current_main = open_artifact(main_target, std::wstring(main_target), false, attempt_error);
      if (current_main && current_main->hash == expected_hash) {
        auto process = sunshine_process(
          expected_hash, current_main->identity, attempt_error,
          candidate ? forbidden_sunshine_pids : std::set<DWORD> {},
          candidate ? minimum_creation_time : 0);
        if (process) {
          const auto ports = tcp_listener_ports(*process, attempt_error);
          if (attempt_error.empty() && ports.contains(47984) && ports.contains(47989) && ports.contains(48010)) {
            return true;
          }
        }
      }
    }
    Sleep(250);
  } while (GetTickCount64() < deadline);
  return fail(error, "bounded health predicate failed: exact process image/FileId/hash/listeners not proven");
#endif
}

bool make_receipt(
  const arguments_t &arguments,
  const file_record_t &main,
  const file_record_t &helper,
  const file_record_t &service_binary,
  const file_record_t &candidate_main,
  const file_record_t &candidate_helper,
  const std::vector<std::uint8_t> &baseline,
  const hotfix::hash256_t &baseline_digest,
  hotfix::receipt_t &receipt,
  std::vector<std::uint8_t> &encoded,
  std::string &error
) {
  receipt.transaction_id = arguments.transaction_id;
  receipt.expected_original_hash = arguments.expected_original_hash;
  receipt.candidate_main_hash = arguments.candidate_main_hash;
  receipt.candidate_helper_hash = arguments.candidate_helper_hash;
  receipt.original_helper_hash = helper.handle.valid() ? helper.hash : hotfix::hash256_t {};
  receipt.service_executable_hash = service_binary.hash;
  receipt.baseline_digest = baseline_digest;
  receipt.original_main_identity = main.identity;
  // Finalized to the staged snapshot identity before receipt publication.
  receipt.original_main_snapshot_identity = main.identity;
  receipt.original_helper_identity = helper.handle.valid() ? helper.identity : hotfix::file_identity_t {};
  receipt.service_executable_identity = service_binary.identity;
  receipt.candidate_main_identity = candidate_main.identity;
  receipt.candidate_helper_identity = candidate_helper.identity;
  receipt.original_main_size = main.size;
  receipt.original_helper_size = helper.handle.valid() ? helper.size : 0;
  receipt.service_executable_size = service_binary.size;
  receipt.candidate_main_size = candidate_main.size;
  receipt.candidate_helper_size = candidate_helper.size;
  receipt.helper_originally_present = helper.handle.valid();
  FILETIME now {};
  GetSystemTimeAsFileTime(&now);
  receipt.created_filetime = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32U) | now.dwLowDateTime;
  receipt.scm_baseline = baseline;
  auto serialized = hotfix::serialize_receipt(receipt);
  if (!serialized.ok()) {
    return fail(error, "cannot serialize receipt: " + serialized.error);
  }
  encoded = std::move(serialized.value);
  return true;
}

bool move_no_replace(const std::wstring &source, const std::wstring &destination, const char *boundary, std::string &error) {
  failpoint(boundary);
  if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
    return fail(error, "write-through rename failed: " + windows_error());
  }
  return true;
}

BOOL deploy_replace_file(
  const std::wstring &target,
  const std::wstring &replacement,
  const std::wstring &backup
) {
#ifdef VIBEPOLLO_AUDIO_HOTFIX_DEPLOYER_TESTING
  std::array<char, 64> shape_buffer {};
  const auto shape_size = GetEnvironmentVariableA(
    "VIBEPOLLO_AUDIO_HOTFIX_REPLACE_SHAPE", shape_buffer.data(),
    static_cast<DWORD>(shape_buffer.size()));
  const auto shape = shape_size > 0 && shape_size < shape_buffer.size() ?
                       std::string(shape_buffer.data(), shape_size) : std::string {};
  DWORD simulated_error = ERROR_SUCCESS;
  if (shape == "unable-remove-replaced") {
    simulated_error = ERROR_UNABLE_TO_REMOVE_REPLACED;
  } else if (shape == "unable-move-replacement" || shape == "unable-move-replacement-2") {
    if (!MoveFileExW(target.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
      return FALSE;
    }
    if (shape == "unable-move-replacement") {
      if (!DeleteFileW(backup.c_str())) {
        return FALSE;
      }
      simulated_error = ERROR_UNABLE_TO_MOVE_REPLACEMENT;
    } else {
      simulated_error = ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
    }
  }
  if (simulated_error != ERROR_SUCCESS) {
    SetLastError(simulated_error);
    return FALSE;
  }
#endif
  return ReplaceFileW(
    target.c_str(), replacement.c_str(), backup.c_str(),
    REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
}

bool deploy_command(const arguments_t &arguments, std::string &error) {
  file_record_t main;
  file_record_t helper;
  file_record_t service_binary;
  file_record_t candidate_main;
  file_record_t candidate_helper;
  hotfix::scm_semantic_baseline_t baseline;
  std::vector<std::uint8_t> baseline_bytes;
  hotfix::hash256_t baseline_digest {};
  service_handle_t manager;
  service_handle_t service;
  if (!validate_common(arguments, main, helper, service_binary, candidate_main, candidate_helper,
                       baseline, baseline_bytes, baseline_digest, manager, service, true, error)) {
    return false;
  }
  SERVICE_STATUS_PROCESS initial_status {};
  if (!query_service_status(service.get(), initial_status, error) || initial_status.dwCurrentState != SERVICE_RUNNING) {
    return fail(error, "deploy requires a healthy running original baseline");
  }

  hotfix::receipt_t receipt;
  std::vector<std::uint8_t> receipt_bytes;
  if (!make_receipt(arguments, main, helper, service_binary, candidate_main, candidate_helper,
                    baseline_bytes, baseline_digest, receipt, receipt_bytes, error) ||
      !health_check(service.get(), receipt, false, error)) {
    return false;
  }
  handle_t mutex;
  if (!create_mutex(mutex, error)) {
    return false;
  }
  mutex_release_t mutex_release(mutex.get());
  if (!ensure_secure_directory(transaction_root, error) ||
      !ensure_secure_directory(transaction_path(arguments.transaction_id), error) ||
      !verify_exact_namespace(transaction_path(arguments.transaction_id), error)) {
    return false;
  }
  const auto directory = transaction_path(arguments.transaction_id);
  if (exact_path_exists(path_join(directory, receipt_name), error)) {
    return fail(error, "immutable receipt already exists; use status or recover");
  }
  if (!error.empty()) {
    return false;
  }
  for (const auto *post_receipt_name : {
         main_live_backup_name, helper_live_backup_name, failed_main_name, failed_helper_name,
         rollback_main_stage_name}) {
    if (exact_path_exists(path_join(directory, post_receipt_name), error)) {
      return fail(error, "post-receipt artifact exists without a receipt");
    }
    if (!error.empty()) {
      return false;
    }
  }
  handle_t pinned_root(CreateFileW(install_root.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  handle_t pinned_transaction(CreateFileW(transaction_path(arguments.transaction_id).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!pinned_root.valid() || !pinned_transaction.valid()) {
    return fail(error, "cannot pin protected parent directories");
  }
  if (!copy_from_handle(candidate_main.handle.get(), candidate_main.size, path_join(directory, main_stage_name),
                        candidate_main.hash, "main-copy-create", "main-copy-mid", "main-copy-flush", error) ||
      !copy_from_handle(candidate_helper.handle.get(), candidate_helper.size, path_join(directory, helper_stage_name),
                        candidate_helper.hash, "helper-copy-create", "helper-copy-mid", "helper-copy-flush", error) ||
      !copy_from_handle(main.handle.get(), main.size, path_join(directory, main_snapshot_name),
                        main.hash, "main-snapshot-create", "main-snapshot-mid", "main-snapshot-flush", error) ||
      (helper.handle.valid() && !copy_from_handle(helper.handle.get(), helper.size, path_join(directory, helper_snapshot_name),
                                                  helper.hash, "helper-snapshot-create", "helper-snapshot-mid", "helper-snapshot-flush", error)) ||
      !flush_volume(error)) {
    return false;
  }
  failpoint("stages-volume-flush");
  auto pinned_main_stage = open_artifact(
    path_join(directory, main_stage_name), path_join(directory, main_stage_name), false, error);
  auto pinned_helper_stage = open_artifact(
    path_join(directory, helper_stage_name), path_join(directory, helper_stage_name), false, error);
  auto pinned_main_snapshot = open_artifact(
    path_join(directory, main_snapshot_name), path_join(directory, main_snapshot_name), false, error);
  auto pinned_helper_snapshot = open_artifact(
    path_join(directory, helper_snapshot_name), path_join(directory, helper_snapshot_name), true, error);
  if (!pinned_main_stage || !pinned_helper_stage || !pinned_main_snapshot || !pinned_helper_snapshot ||
      pinned_main_stage->hash != receipt.candidate_main_hash ||
      pinned_helper_stage->hash != receipt.candidate_helper_hash ||
      pinned_main_snapshot->hash != receipt.expected_original_hash ||
      (receipt.helper_originally_present &&
       (!pinned_helper_snapshot->handle.valid() || pinned_helper_snapshot->hash != receipt.original_helper_hash)) ||
      (!receipt.helper_originally_present && pinned_helper_snapshot->handle.valid())) {
    return fail(error, error.empty() ? "staged/snapshot artifacts changed before receipt" : error);
  }
  receipt.candidate_main_identity = pinned_main_stage->identity;
  receipt.candidate_helper_identity = pinned_helper_stage->identity;
  receipt.original_main_snapshot_identity = pinned_main_snapshot->identity;
  const auto finalized_receipt = hotfix::serialize_receipt(receipt);
  if (!finalized_receipt.ok()) {
    return fail(error, "cannot finalize staged receipt: " + finalized_receipt.error);
  }
  receipt_bytes = finalized_receipt.value;
  service_handle_t refreshed_manager;
  service_handle_t refreshed_service;
  hotfix::scm_semantic_baseline_t refreshed_baseline;
  if (!query_scm_baseline(refreshed_manager, refreshed_service, refreshed_baseline, true, error)) {
    return false;
  }
  const auto refreshed_encoding = hotfix::serialize_scm_baseline(refreshed_baseline);
  if (!refreshed_encoding.ok() || hotfix::sha256(refreshed_encoding.value) != receipt.baseline_digest ||
      !health_check(refreshed_service.get(), receipt, false, error)) {
    return fail(error, error.empty() ? "baseline changed before receipt publication" : error);
  }
  manager = std::move(refreshed_manager);
  service = std::move(refreshed_service);
  if (!immutable_receipt_write(path_join(directory, receipt_name), receipt_bytes, error)) {
    return false;
  }
  auto pinned_receipt = open_artifact(
    path_join(directory, receipt_name), path_join(directory, receipt_name), false, error, false);
  if (!pinned_receipt) {
    return false;
  }
  auto pinned_service_executable = open_artifact(
    service_executable, std::wstring(service_executable), false, error, false);
  if (!pinned_service_executable ||
      pinned_service_executable->hash != receipt.service_executable_hash ||
      pinned_service_executable->identity != receipt.service_executable_identity) {
    return fail(error, error.empty() ? "service executable identity/hash differs from receipt" : error);
  }
  baseline_processes_t baseline_processes;
  if (!capture_baseline_sunshine_processes(receipt, baseline_processes, error) ||
      !stop_service(service.get(), error) ||
      !wait_for_baseline_sunshine_exit(baseline_processes, error)) {
    return false;
  }

  if (helper.handle.valid() &&
      !move_no_replace(helper_target, path_join(directory, helper_live_backup_name), "helper-original-rename", error)) {
    return false;
  }
  if (!move_no_replace(path_join(directory, helper_stage_name), helper_target, "helper-candidate-rename", error)) {
    return false;
  }
  // ReplaceFileW must reopen the replacement with write access. Keep the
  // verified stage pinned across the service stop, then release only that
  // handle immediately before the atomic replacement boundary.
  pinned_main_stage->handle.reset();
  failpoint("before-main-replace");
  if (!deploy_replace_file(
        main_target, path_join(directory, main_stage_name),
        path_join(directory, main_live_backup_name))) {
    const auto replace_error = GetLastError();
    auto target = open_artifact(main_target, std::wstring(main_target), true, error);
    auto stage = open_artifact(path_join(directory, main_stage_name), path_join(directory, main_stage_name), true, error);
    auto backup = open_artifact(path_join(directory, main_live_backup_name), path_join(directory, main_live_backup_name), true, error);
    if (!target || !stage || !backup ||
        !(replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT || replace_error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2 ||
          replace_error == ERROR_UNABLE_TO_REMOVE_REPLACED)) {
      return fail(error, "ReplaceFile failed with an unrecognized outcome: " + windows_error(replace_error));
    }
    return fail(error, "ReplaceFile reported a recognized nonterminal outcome; run recover: " + windows_error(replace_error));
  }
  failpoint("after-main-replace");
  if (!flush_volume(error)) {
    return false;
  }
  failpoint("replacement-volume-flush");
  auto pinned_candidate_main = open_artifact(main_target, std::wstring(main_target), false, error, false);
  auto pinned_candidate_helper = open_artifact(helper_target, std::wstring(helper_target), false, error, false);
  auto pinned_main_live_backup = open_artifact(
    path_join(directory, main_live_backup_name), path_join(directory, main_live_backup_name), false, error, false);
  auto pinned_helper_live_backup = open_artifact(
    path_join(directory, helper_live_backup_name), path_join(directory, helper_live_backup_name), true, error, false);
  if (!pinned_candidate_main || !pinned_candidate_helper ||
      !pinned_main_live_backup || !pinned_helper_live_backup ||
      pinned_candidate_main->hash != receipt.candidate_main_hash ||
      pinned_candidate_helper->hash != receipt.candidate_helper_hash ||
      pinned_candidate_main->identity != receipt.candidate_main_identity ||
      pinned_candidate_helper->identity != receipt.candidate_helper_identity ||
      pinned_main_live_backup->hash != receipt.expected_original_hash ||
      pinned_main_live_backup->identity != receipt.original_main_identity ||
      (receipt.helper_originally_present &&
       (!pinned_helper_live_backup->handle.valid() ||
        pinned_helper_live_backup->hash != receipt.original_helper_hash ||
        pinned_helper_live_backup->identity != receipt.original_helper_identity)) ||
      (!receipt.helper_originally_present && pinned_helper_live_backup->handle.valid()) ||
      !verify_protected_acl(main_target, false, error) ||
      !verify_protected_acl(helper_target, false, error)) {
    return fail(error, error.empty() ? "deployed target identity/hash/ACL verification failed" : error);
  }
  failpoint("before-health");
  const auto candidate_start_boundary = current_filetime();
  if (!start_service(service.get(), error) ||
      !health_check(
        service.get(), receipt, true, error,
        baseline_processes.pids, candidate_start_boundary)) {
    return false;
  }
  failpoint("after-health");
  std::cout << "result=deployed\ntransaction_id=" << arguments.transaction_id << "\n";
  return true;
}

bool restore_main(const std::wstring &directory, const hotfix::receipt_t &receipt, std::string &error) {
  auto current = open_artifact(main_target, std::wstring(main_target), true, error);
  if (!current) {
    return false;
  }
  if (current->handle.valid() && current->hash == receipt.expected_original_hash) {
    return true;
  }
  if (current->handle.valid() && current->hash != receipt.candidate_main_hash) {
    return fail(error, "main target contains unknown bytes");
  }
  auto live_backup = open_artifact(
    path_join(directory, main_live_backup_name), path_join(directory, main_live_backup_name), true, error);
  if (!live_backup) {
    return false;
  }
  if (live_backup->handle.valid()) {
    if (live_backup->hash != receipt.expected_original_hash ||
        live_backup->identity != receipt.original_main_identity) {
      return fail(error, "live original backup identity/hash is invalid");
    }
    if (!current->handle.valid()) {
      return move_no_replace(path_join(directory, main_live_backup_name), main_target,
                             "rollback-live-main-rename", error);
    }
    // The live backup has been verified by its pinned handle. ReplaceFileW
    // needs to reopen that replacement with write access, so release only at
    // the final atomic boundary.
    live_backup->handle.reset();
    failpoint("before-rollback-live-main-replace");
    if (!ReplaceFileW(main_target.c_str(), path_join(directory, main_live_backup_name).c_str(),
                      path_join(directory, failed_main_name).c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
      return fail(error, "live-backup rollback ReplaceFile failed: " + windows_error());
    }
    failpoint("after-rollback-live-main-replace");
    return true;
  }

  // The immutable byte snapshot is a fallback only when ReplaceFile left no
  // target and retained the candidate stage: the fail-closed physical outcome
  // of the documented unable-to-move replacement errors. A missing live backup
  // beside an installed candidate is treated as tampering, not as recovery.
  if (current->handle.valid()) {
    return fail(error, "live original backup is missing beside an installed candidate");
  }
  auto retained_stage = open_artifact(
    path_join(directory, main_stage_name), path_join(directory, main_stage_name), true, error);
  if (!retained_stage || !retained_stage->handle.valid() ||
      retained_stage->hash != receipt.candidate_main_hash) {
    return fail(error, error.empty() ? "snapshot fallback does not match a documented ReplaceFile outcome" : error);
  }
  // Its bytes and volume must still match the receipt.
  auto snapshot = open_artifact(path_join(directory, main_snapshot_name), path_join(directory, main_snapshot_name), false, error);
  if (!snapshot || snapshot->hash != receipt.expected_original_hash ||
      snapshot->identity != receipt.original_main_snapshot_identity) {
    return fail(error, "immutable main snapshot is invalid");
  }
  snapshot->handle.reset();
  return move_no_replace(
    path_join(directory, main_snapshot_name), main_target,
    "rollback-main-snapshot-rename", error);
}

bool restore_helper(const std::wstring &directory, const hotfix::receipt_t &receipt, std::string &error) {
  auto current = open_artifact(helper_target, std::wstring(helper_target), true, error);
  if (!current) {
    return false;
  }
  if (receipt.helper_originally_present && current->handle.valid() && current->hash == receipt.original_helper_hash) {
    return true;
  }
  if (!receipt.helper_originally_present && !current->handle.valid()) {
    return true;
  }
  if (current->handle.valid() && current->hash != receipt.candidate_helper_hash) {
    return fail(error, "helper target contains unknown bytes");
  }
  if (current->handle.valid() &&
      !move_no_replace(helper_target, path_join(directory, failed_helper_name), "rollback-helper-failed-rename", error)) {
    return false;
  }
  if (!receipt.helper_originally_present) {
    return true;
  }
  auto live_backup = open_artifact(
    path_join(directory, helper_live_backup_name), path_join(directory, helper_live_backup_name), true, error);
  if (!live_backup) {
    return false;
  }
  if (live_backup->handle.valid()) {
    if (live_backup->hash != receipt.original_helper_hash ||
        live_backup->identity != receipt.original_helper_identity) {
      return fail(error, "live original helper backup identity/hash is invalid");
    }
    return move_no_replace(path_join(directory, helper_live_backup_name), helper_target,
                           "rollback-live-helper-rename", error);
  }
  return fail(error, "live original helper backup is missing; refusing snapshot substitution");
}

bool recovery_physical_preflight(
  const std::wstring &directory,
  const hotfix::receipt_t &receipt,
  std::string &error
) {
  const auto open = [&](const std::wstring &path, const bool allow_absent) -> std::optional<file_record_t> {
    auto record = open_artifact(path, path, allow_absent, error);
    if (!record || (record->handle.valid() && !verify_protected_acl(path, false, error))) {
      return std::nullopt;
    }
    if (record->handle.valid() && record->identity.volume_serial != receipt.original_main_identity.volume_serial) {
      fail(error, "recovery artifact is on the wrong volume");
      return std::nullopt;
    }
    return record;
  };
  auto main = open(main_target, true);
  auto helper = open(helper_target, true);
  auto main_snapshot = open(path_join(directory, main_snapshot_name), true);
  auto helper_snapshot = open(path_join(directory, helper_snapshot_name), true);
  auto main_stage = open(path_join(directory, main_stage_name), true);
  auto helper_stage = open(path_join(directory, helper_stage_name), true);
  auto main_live = open(path_join(directory, main_live_backup_name), true);
  auto helper_live = open(path_join(directory, helper_live_backup_name), true);
  auto failed_main = open(path_join(directory, failed_main_name), true);
  auto failed_helper = open(path_join(directory, failed_helper_name), true);
  auto rollback_stage = open(path_join(directory, rollback_main_stage_name), true);
  if (!main || !helper || !main_snapshot || !helper_snapshot || !main_stage || !helper_stage ||
      !main_live || !helper_live || !failed_main || !failed_helper || !rollback_stage) {
    return false;
  }
  const auto hash_is = [](const file_record_t &record, const hotfix::hash256_t &expected) {
    return !record.handle.valid() || record.hash == expected;
  };
  const auto main_is_original = main->handle.valid() && main->hash == receipt.expected_original_hash;
  const auto main_is_candidate = main->handle.valid() && main->hash == receipt.candidate_main_hash;
  const auto main_is_absent = !main->handle.valid();
  if (!main_is_original && !main_is_candidate && !main_is_absent) {
    return fail(error, "main target contains unrecognized bytes before rollback");
  }
  const auto helper_is_original = receipt.helper_originally_present && helper->handle.valid() &&
                                  helper->hash == receipt.original_helper_hash;
  const auto helper_is_candidate = helper->handle.valid() && helper->hash == receipt.candidate_helper_hash;
  if (helper->handle.valid() && !helper_is_original && !helper_is_candidate) {
    return fail(error, "helper target contains unrecognized bytes before rollback");
  }
  if (main_is_candidate && main->identity != receipt.candidate_main_identity) {
    return fail(error, "candidate main FileId differs from the receipt");
  }
  if (helper_is_candidate && helper->identity != receipt.candidate_helper_identity) {
    return fail(error, "candidate helper FileId differs from the receipt");
  }
  if (helper_is_original && helper->identity != receipt.original_helper_identity) {
    return fail(error, "original helper FileId differs from the receipt");
  }
  const auto snapshot_present = main_snapshot->handle.valid();
  const auto recovered_snapshot_target =
    main_is_original && main->identity == receipt.original_main_snapshot_identity &&
    !snapshot_present && !main_live->handle.valid() && main_stage->handle.valid() &&
    main_stage->hash == receipt.candidate_main_hash;
  if ((snapshot_present &&
       (main_snapshot->hash != receipt.expected_original_hash ||
        main_snapshot->identity != receipt.original_main_snapshot_identity)) ||
      (!snapshot_present && !recovered_snapshot_target) ||
      (receipt.helper_originally_present &&
       (!helper_snapshot->handle.valid() || helper_snapshot->hash != receipt.original_helper_hash)) ||
      (!receipt.helper_originally_present && helper_snapshot->handle.valid()) ||
      !hash_is(*main_stage, receipt.candidate_main_hash) ||
      !hash_is(*helper_stage, receipt.candidate_helper_hash) ||
      !hash_is(*failed_main, receipt.candidate_main_hash) ||
      !hash_is(*failed_helper, receipt.candidate_helper_hash) ||
      !hash_is(*rollback_stage, receipt.expected_original_hash)) {
    return fail(error, "transaction stage/snapshot/failed artifact hash mismatch");
  }
  if ((main_stage->handle.valid() && main_stage->identity != receipt.candidate_main_identity) ||
      (helper_stage->handle.valid() && helper_stage->identity != receipt.candidate_helper_identity) ||
      (failed_main->handle.valid() && failed_main->identity != receipt.candidate_main_identity) ||
      (failed_helper->handle.valid() && failed_helper->identity != receipt.candidate_helper_identity)) {
    return fail(error, "candidate stage/failed FileId differs from the receipt");
  }
  if (main_live->handle.valid() &&
      (main_live->hash != receipt.expected_original_hash || main_live->identity != receipt.original_main_identity)) {
    return fail(error, "live main backup identity/hash mismatch");
  }
  if (helper_live->handle.valid() &&
      (!receipt.helper_originally_present || helper_live->hash != receipt.original_helper_hash ||
       helper_live->identity != receipt.original_helper_identity)) {
    return fail(error, "live helper backup identity/hash mismatch");
  }
  if (main_is_candidate && !main_live->handle.valid()) {
    return fail(error, "candidate main has no exact live original backup");
  }
  if (main_is_original && main->identity != receipt.original_main_identity && !recovered_snapshot_target) {
    return fail(error, "original main FileId differs from the receipt outside snapshot fallback");
  }
  if (main_is_absent && !main_live->handle.valid() &&
      (!main_stage->handle.valid() || main_stage->hash != receipt.candidate_main_hash)) {
    return fail(error, "absent main does not match a documented ReplaceFile recovery shape");
  }
  if (receipt.helper_originally_present && !helper_is_original && !helper_live->handle.valid()) {
    return fail(error, "non-original helper has no exact live original backup");
  }
  return true;
}

bool rollback_command(const arguments_t &arguments, std::string &error) {
  handle_t mutex;
  if (!create_mutex(mutex, error)) {
    return false;
  }
  mutex_release_t mutex_release(mutex.get());
  const auto directory = transaction_path(arguments.transaction_id);
  if (!verify_protected_acl(transaction_root, true, error) || !verify_protected_acl(directory, true, error) ||
      !verify_exact_namespace(directory, error)) {
    return false;
  }
  auto receipt = read_receipt(path_join(directory, receipt_name), error);
  if (!receipt || !arguments_match_receipt(arguments, *receipt, error)) {
    return false;
  }
  auto pinned_receipt = open_artifact(
    path_join(directory, receipt_name), path_join(directory, receipt_name), false, error, false);
  if (!pinned_receipt) {
    return false;
  }
  auto pinned_service_executable = open_artifact(
    service_executable, std::wstring(service_executable), false, error, false);
  if (!pinned_service_executable ||
      pinned_service_executable->hash != receipt->service_executable_hash ||
      pinned_service_executable->identity != receipt->service_executable_identity) {
    return fail(error, error.empty() ? "service executable identity/hash differs from receipt" : error);
  }
  service_handle_t manager;
  service_handle_t service;
  hotfix::scm_semantic_baseline_t baseline;
  if (!query_scm_baseline(manager, service, baseline, true, error)) {
    return false;
  }
  const auto baseline_encoded = hotfix::serialize_scm_baseline(baseline);
  if (!baseline_encoded.ok() || hotfix::sha256(baseline_encoded.value) != receipt->baseline_digest) {
    return fail(error, "SCM semantic baseline changed; refusing rollback");
  }
  if (!recovery_physical_preflight(directory, *receipt, error)) {
    return false;
  }
  SERVICE_STATUS_PROCESS service_status {};
  if (!query_service_status(service.get(), service_status, error)) {
    return false;
  }
  {
    auto current_main = open_artifact(main_target, std::wstring(main_target), true, error, false);
    auto current_helper = open_artifact(helper_target, std::wstring(helper_target), true, error, false);
    if (!current_main || !current_helper) {
      return false;
    }
    const auto helper_is_original = receipt->helper_originally_present ?
      (current_helper->handle.valid() && current_helper->hash == receipt->original_helper_hash) :
      !current_helper->handle.valid();
    if (current_main->handle.valid() && current_main->hash == receipt->expected_original_hash && helper_is_original &&
        service_status.dwCurrentState == SERVICE_RUNNING) {
      auto health_receipt = *receipt;
      health_receipt.original_main_identity = current_main->identity;
      std::string health_error;
      if (health_check(service.get(), health_receipt, false, health_error)) {
        std::cout << "result=rolled_back\ntransaction_id=" << arguments.transaction_id << "\nalready_converged=true\n";
        return true;
      }
    }
  }
  if (service_status.dwCurrentState != SERVICE_STOPPED && !stop_service(service.get(), error)) {
    return false;
  }
  if (!restore_helper(directory, *receipt, error) || !restore_main(directory, *receipt, error) || !flush_volume(error)) {
    return false;
  }
  failpoint("rollback-volume-flush");
  auto pinned_original_main = open_artifact(main_target, std::wstring(main_target), false, error, false);
  auto pinned_original_helper = open_artifact(helper_target, std::wstring(helper_target), true, error, false);
  auto pinned_failed_main = open_artifact(
    path_join(directory, failed_main_name), path_join(directory, failed_main_name), true, error, false);
  auto pinned_failed_helper = open_artifact(
    path_join(directory, failed_helper_name), path_join(directory, failed_helper_name), true, error, false);
  if (!pinned_original_main || !pinned_original_helper || !pinned_failed_main || !pinned_failed_helper ||
      pinned_original_main->hash != receipt->expected_original_hash ||
      (receipt->helper_originally_present &&
       (!pinned_original_helper->handle.valid() || pinned_original_helper->hash != receipt->original_helper_hash)) ||
      (!receipt->helper_originally_present && pinned_original_helper->handle.valid()) ||
      (pinned_failed_main->handle.valid() && pinned_failed_main->hash != receipt->candidate_main_hash) ||
      (pinned_failed_helper->handle.valid() && pinned_failed_helper->hash != receipt->candidate_helper_hash) ||
      !verify_protected_acl(main_target, false, error) ||
      (receipt->helper_originally_present && !verify_protected_acl(helper_target, false, error)) ||
      !start_service(service.get(), error)) {
    return fail(error, error.empty() ? "rolled-back target identity/hash/ACL verification failed" : error);
  }
  failpoint("before-rollback-health");
  auto health_receipt = *receipt;
  health_receipt.original_main_identity = pinned_original_main->identity;
  if (!health_check(service.get(), health_receipt, false, error)) {
    return false;
  }
  failpoint("after-rollback-health");
  std::cout << "result=rolled_back\ntransaction_id=" << arguments.transaction_id << "\n";
  return true;
}

bool start_baseline_command(const arguments_t &arguments, std::string &error) {
  file_record_t main;
  file_record_t helper;
  file_record_t service_binary;
  file_record_t candidate_main;
  file_record_t candidate_helper;
  hotfix::scm_semantic_baseline_t baseline;
  std::vector<std::uint8_t> baseline_bytes;
  hotfix::hash256_t baseline_digest {};
  service_handle_t manager;
  service_handle_t service;
  if (!validate_common(arguments, main, helper, service_binary, candidate_main, candidate_helper,
                       baseline, baseline_bytes, baseline_digest, manager, service, true, error)) {
    return false;
  }
  handle_t mutex;
  if (!create_mutex(mutex, error)) {
    return false;
  }
  mutex_release_t mutex_release(mutex.get());
  SERVICE_STATUS_PROCESS initial_status {};
  if (!query_service_status(service.get(), initial_status, error)) {
    return false;
  }
  const auto started_by_command = initial_status.dwCurrentState == SERVICE_STOPPED;
  if (!start_service(service.get(), error)) {
    return false;
  }
  hotfix::receipt_t transient;
  std::vector<std::uint8_t> ignored;
  if (!make_receipt(arguments, main, helper, service_binary, candidate_main, candidate_helper,
                    baseline_bytes, baseline_digest, transient, ignored, error) ||
      !health_check(service.get(), transient, false, error)) {
    if (started_by_command) {
      std::string stop_error;
      if (!stop_service(service.get(), stop_error)) {
        error += "; failed to restore stopped baseline: " + stop_error;
      }
    }
    return false;
  }
  std::cout << "result=baseline_healthy\n";
  return true;
}

bool validate_command(const arguments_t &arguments, std::string &error) {
  file_record_t main;
  file_record_t helper;
  file_record_t service_binary;
  file_record_t candidate_main;
  file_record_t candidate_helper;
  hotfix::scm_semantic_baseline_t baseline;
  std::vector<std::uint8_t> baseline_bytes;
  hotfix::hash256_t baseline_digest {};
  service_handle_t manager;
  service_handle_t service;
  if (!validate_common(arguments, main, helper, service_binary, candidate_main, candidate_helper,
                       baseline, baseline_bytes, baseline_digest, manager, service, false, error)) {
    return false;
  }
  std::cout << "result=valid\n"
            << "baseline_digest=" << hotfix::hash_to_hex(baseline_digest) << "\n"
            << "service_executable_hash=" << hotfix::hash_to_hex(service_binary.hash) << "\n"
            << "required_original_hash=" << expected_original_hash_hex << "\n";
  return true;
}

bool status_command(const arguments_t &arguments, std::string &error) {
  const auto directory = transaction_path(arguments.transaction_id);
  if (!verify_protected_acl(transaction_root, true, error) || !verify_protected_acl(directory, true, error) ||
      !verify_exact_namespace(directory, error)) {
    return false;
  }
  auto receipt = read_receipt(path_join(directory, receipt_name), error);
  if (!receipt) {
    return false;
  }
  service_handle_t manager;
  service_handle_t service;
  hotfix::scm_semantic_baseline_t baseline;
  if (!query_scm_baseline(manager, service, baseline, false, error)) {
    return false;
  }
  const auto encoded = hotfix::serialize_scm_baseline(baseline);
  if (!encoded.ok()) {
    return fail(error, encoded.error);
  }
  const auto content = [](const file_record_t &record, const hotfix::hash256_t &original, const hotfix::hash256_t &candidate) {
    if (!record.handle.valid()) {
      return hotfix::artifact_content_t::absent;
    }
    if (record.hash == original) {
      return hotfix::artifact_content_t::original;
    }
    if (record.hash == candidate) {
      return hotfix::artifact_content_t::candidate;
    }
    return hotfix::artifact_content_t::other;
  };
  const auto observe = [&](const std::wstring &path, const hotfix::hash256_t &original,
                           const hotfix::hash256_t &candidate) -> std::optional<hotfix::artifact_observation_t> {
    auto record = open_artifact(path, path, true, error);
    if (!record) {
      return std::nullopt;
    }
    const auto kind = content(*record, original, candidate);
    if (record->handle.valid() && !verify_protected_acl(path, false, error)) {
      return std::nullopt;
    }
    const auto identity_ok = !record->handle.valid() || record->identity.volume_serial == receipt->original_main_identity.volume_serial;
    return hotfix::artifact_observation_t {
      .content = kind,
      .trusted_acl = true,
      .same_volume = identity_ok,
      .no_reparse_points = true,
      .single_link = true,
      .no_extra_streams = true,
      .identity_matches_receipt = identity_ok,
    };
  };
  const hotfix::hash256_t no_candidate {};
  auto main = observe(main_target, receipt->expected_original_hash, receipt->candidate_main_hash);
  auto helper = observe(helper_target, receipt->original_helper_hash, receipt->candidate_helper_hash);
  auto main_snapshot = observe(path_join(directory, main_snapshot_name), receipt->expected_original_hash, no_candidate);
  auto helper_snapshot = observe(path_join(directory, helper_snapshot_name), receipt->original_helper_hash, no_candidate);
  auto live_backup = observe(path_join(directory, main_live_backup_name), receipt->expected_original_hash, no_candidate);
  auto failed_main = observe(path_join(directory, failed_main_name), no_candidate, receipt->candidate_main_hash);
  auto failed_helper = observe(path_join(directory, failed_helper_name), no_candidate, receipt->candidate_helper_hash);
  if (!main || !helper || !main_snapshot || !helper_snapshot || !live_backup || !failed_main || !failed_helper) {
    return false;
  }
  SERVICE_STATUS_PROCESS service_status {};
  if (!query_service_status(service.get(), service_status, error)) {
    return false;
  }
  auto service_health = hotfix::service_health_t::stopped;
  if (service_status.dwCurrentState != SERVICE_STOPPED) {
    std::string health_error;
    if (main->content == hotfix::artifact_content_t::candidate && health_check(service.get(), *receipt, true, health_error)) {
      service_health = hotfix::service_health_t::healthy_candidate;
    } else if (main->content == hotfix::artifact_content_t::original && health_check(service.get(), *receipt, false, health_error)) {
      service_health = hotfix::service_health_t::healthy_original;
    } else {
      service_health = hotfix::service_health_t::running_unverified;
    }
  }
  hotfix::physical_observation_t observation {
    .receipt = hotfix::receipt_presence_t::valid,
    .namespace_exact = true,
    .scm_baseline_matches = hotfix::sha256(encoded.value) == receipt->baseline_digest,
    .main_target = *main,
    .helper_target = *helper,
    .original_snapshot = *main_snapshot,
    .helper_snapshot = *helper_snapshot,
    .live_backup = *live_backup,
    .failed_main = *failed_main,
    .failed_helper = *failed_helper,
    .service_health = service_health,
  };
  const auto state = hotfix::classify_physical_state(observation);
  std::cout << "result=status\n"
            << "transaction_id=" << receipt->transaction_id << "\n"
            << "scm_baseline_matches=" << (hotfix::sha256(encoded.value) == receipt->baseline_digest ? "true" : "false") << "\n"
            << "physical_state=" << static_cast<unsigned>(state) << "\n";
  return state != hotfix::physical_state_t::unsafe && state != hotfix::physical_state_t::unknown;
}

void print_usage() {
  std::cerr
    << "Usage:\n"
    << "  vibepollo_audio_hotfix_deployer validate --candidate-main PATH --candidate-helper PATH --expected-original-hash HEX --candidate-main-hash HEX --candidate-helper-hash HEX [--baseline-digest HEX]\n"
    << "  vibepollo_audio_hotfix_deployer start-baseline|deploy --transaction-id UUID --candidate-main PATH --candidate-helper PATH --expected-original-hash HEX --candidate-main-hash HEX --candidate-helper-hash HEX --baseline-digest HEX\n"
    << "  vibepollo_audio_hotfix_deployer status --transaction-id UUID\n"
    << "  vibepollo_audio_hotfix_deployer rollback|recover --transaction-id UUID --expected-original-hash HEX --candidate-main-hash HEX --candidate-helper-hash HEX --baseline-digest HEX\n";
}

}  // namespace

int wmain(const int argc, wchar_t **argv) {
  arguments_t arguments;
  std::string error;
  if (!parse_arguments(argc, argv, arguments, error)) {
    std::cerr << "error=" << error << '\n';
    print_usage();
    return 2;
  }
  bool success = false;
  switch (arguments.command) {
    case command_t::validate:
      success = validate_command(arguments, error);
      break;
    case command_t::start_baseline:
      success = start_baseline_command(arguments, error);
      break;
    case command_t::deploy:
      success = deploy_command(arguments, error);
      break;
    case command_t::status:
      success = status_command(arguments, error);
      break;
    case command_t::rollback:
    case command_t::recover:
      success = rollback_command(arguments, error);
      break;
  }
  if (!success) {
    std::cerr << "error=" << error << '\n';
    return 1;
  }
  return 0;
}
