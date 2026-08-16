/**
 * @file src/platform/windows/audio_policy_process.cpp
 * @brief Bounded CreateProcess/Job supervisor for audio policy operations.
 */

#include "audio_policy_process.h"

#include <Windows.h>
#include <Aclapi.h>
#include <ShlObj.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace platf::audio_policy {
namespace {

  constexpr std::wstring_view kHelperBasename = L"sunshine_audio_policy_helper.exe";
  constexpr DWORD kPipeBufferBytes = 16u * 1024u;
  constexpr DWORD kReapTimeoutMilliseconds = 2'000u;

  class unique_handle_t {
  public:
    unique_handle_t() = default;
    explicit unique_handle_t(HANDLE handle): handle_(handle) {}
    ~unique_handle_t() {
      reset();
    }

    unique_handle_t(const unique_handle_t &) = delete;
    unique_handle_t &operator=(const unique_handle_t &) = delete;

    unique_handle_t(unique_handle_t &&other) noexcept:
      handle_(other.release()) {}

    unique_handle_t &operator=(unique_handle_t &&other) noexcept {
      if (this != &other) {
        reset(other.release());
      }
      return *this;
    }

    HANDLE get() const {
      return handle_;
    }

    explicit operator bool() const {
      return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release() {
      const auto result = handle_;
      handle_ = nullptr;
      return result;
    }

    void reset(HANDLE handle = nullptr) {
      if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
      }
      handle_ = handle;
    }

  private:
    HANDLE handle_ {nullptr};
  };

  struct process_handles_t {
    unique_handle_t process;
    unique_handle_t thread;
  };

  struct file_identity_t {
    DWORD volume_serial {0u};
    DWORD file_index_high {0u};
    DWORD file_index_low {0u};
    DWORD link_count {0u};
  };

  struct pinned_helper_t {
    std::filesystem::path requested_path;
    std::filesystem::path canonical_path;
    file_identity_t identity;
    std::vector<unique_handle_t> ancestors;
  };

  result_t failed_result(const failure_stage_e stage, const DWORD win32_error = ERROR_SUCCESS) {
    result_t result;
    result.stage = stage;
    result.win32_error = win32_error;
    return result;
  }

  bool sid_is_well_known(PSID sid, const WELL_KNOWN_SID_TYPE type) {
    std::array<std::uint8_t, SECURITY_MAX_SID_SIZE> buffer {};
    DWORD size = static_cast<DWORD>(buffer.size());
    return CreateWellKnownSid(
             type,
             nullptr,
             buffer.data(),
             &size) != FALSE &&
           EqualSid(sid, buffer.data()) != FALSE;
  }

  bool current_user_sid(std::vector<std::uint8_t> &sid_buffer) {
    unique_handle_t token;
    HANDLE token_handle = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle)) {
      return false;
    }
    token.reset(token_handle);
    DWORD size = 0u;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0u, &size);
    if (size == 0u) {
      return false;
    }
    std::vector<std::uint8_t> token_buffer(size);
    if (!GetTokenInformation(
          token.get(),
          TokenUser,
          token_buffer.data(),
          size,
          &size)) {
      return false;
    }
    const auto *user = reinterpret_cast<const TOKEN_USER *>(token_buffer.data());
    const auto sid_size = GetLengthSid(user->User.Sid);
    if (sid_size == 0u) {
      return false;
    }
    sid_buffer.resize(sid_size);
    return CopySid(
      sid_size,
      sid_buffer.data(),
      user->User.Sid) != FALSE;
  }

  bool owner_is_trusted(PSID owner, const bool allow_current_owner) {
    if (sid_is_well_known(owner, WinLocalSystemSid) ||
        sid_is_well_known(owner, WinBuiltinAdministratorsSid)) {
      return true;
    }
    PSID trusted_installer = nullptr;
    const auto trusted_installer_ok =
      ConvertStringSidToSidW(
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",
        &trusted_installer) != FALSE;
    const auto is_trusted_installer = trusted_installer_ok &&
      EqualSid(owner, trusted_installer) != FALSE;
    if (trusted_installer != nullptr) {
      LocalFree(trusted_installer);
    }
    if (is_trusted_installer) {
      return true;
    }
    if (!allow_current_owner) {
      return false;
    }
    std::vector<std::uint8_t> current_sid;
    return current_user_sid(current_sid) &&
           EqualSid(owner, current_sid.data()) != FALSE;
  }

  constexpr ACCESS_MASK kDisallowedBroadWrite =
    FILE_WRITE_DATA |
    FILE_APPEND_DATA |
    FILE_WRITE_EA |
    FILE_WRITE_ATTRIBUTES |
    DELETE |
    WRITE_DAC |
    WRITE_OWNER |
    FILE_DELETE_CHILD;

  struct allow_ace_view_t {
    ACCESS_MASK mask;
    PSID sid;
  };

  bool is_allowed_ace_type(const BYTE ace_type) {
    return ace_type == ACCESS_ALLOWED_ACE_TYPE ||
           ace_type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
           ace_type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
           ace_type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
  }

  std::optional<allow_ace_view_t> parse_allowed_ace(
    const ACE_HEADER *header) {
    if (header == nullptr ||
        header->AceSize < sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) {
      return std::nullopt;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(header);
    const auto ace_size = static_cast<std::size_t>(header->AceSize);
    ACCESS_MASK mask = 0u;
    std::memcpy(
      &mask,
      bytes + sizeof(ACE_HEADER),
      sizeof(mask));
    std::size_t sid_offset = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK);
    if (
      header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
      header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
      if (ace_size < sid_offset + sizeof(DWORD)) {
        return std::nullopt;
      }
      DWORD flags = 0u;
      std::memcpy(&flags, bytes + sid_offset, sizeof(flags));
      if (
        (flags &
         ~(ACE_OBJECT_TYPE_PRESENT | ACE_INHERITED_OBJECT_TYPE_PRESENT)) != 0u) {
        return std::nullopt;
      }
      sid_offset += sizeof(flags);
      if ((flags & ACE_OBJECT_TYPE_PRESENT) != 0u) {
        sid_offset += sizeof(GUID);
      }
      if ((flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0u) {
        sid_offset += sizeof(GUID);
      }
    }
    if (sid_offset + sizeof(DWORD) > ace_size) {
      return std::nullopt;
    }
    auto *sid = reinterpret_cast<PSID>(
      const_cast<std::uint8_t *>(bytes + sid_offset));
    if (!IsValidSid(sid)) {
      return std::nullopt;
    }
    const auto sid_size = static_cast<std::size_t>(GetLengthSid(sid));
    if (sid_size == 0u || sid_offset + sid_size > ace_size) {
      return std::nullopt;
    }
    return allow_ace_view_t {mask, sid};
  }

  bool is_volume_root(HANDLE handle) {
    std::vector<wchar_t> buffer(512u);
    const auto length = GetFinalPathNameByHandleW(
      handle,
      buffer.data(),
      static_cast<DWORD>(buffer.size()),
      FILE_NAME_NORMALIZED);
    if (length == 0u || length >= buffer.size()) {
      return false;
    }
    std::wstring path(buffer.data(), length);
    if (path.rfind(L"\\\\?\\", 0u) == 0u) {
      path.erase(0u, 4u);
    }
    wchar_t volume_path[MAX_PATH] {};
    return GetVolumePathNameW(path.c_str(), volume_path, MAX_PATH) != FALSE &&
           _wcsicmp(path.c_str(), volume_path) == 0;
  }

  bool broad_write_ace(
    PSID sid,
    const ACCESS_MASK mask,
    const bool is_directory,
    const bool volume_root,
    const bool allow_current_owner) {
    auto normalized_mask = mask;
    GENERIC_MAPPING mapping {
      FILE_GENERIC_READ,
      FILE_GENERIC_WRITE,
      FILE_GENERIC_EXECUTE,
      FILE_ALL_ACCESS,
    };
    MapGenericMask(&normalized_mask, &mapping);
    auto disallowed = kDisallowedBroadWrite;
    // The fixed-volume root has an inherited-only Authenticated Users
    // create-subdirectory ACE on standard Windows installations. It cannot
    // modify an existing pinned ancestor; descendants are checked separately.
    if (is_directory && volume_root) {
      disallowed &= ~FILE_APPEND_DATA;
    }
    if ((normalized_mask & disallowed) == 0u) {
      return false;
    }
    return !owner_is_trusted(sid, allow_current_owner);
  }

  bool secure_acl(HANDLE handle, const bool allow_current_owner) {
    BY_HANDLE_FILE_INFORMATION file_information {};
    if (GetFileInformationByHandle(handle, &file_information) == FALSE) {
      SetLastError(ERROR_INVALID_DATA);
      return false;
    }
    const auto is_directory =
      (file_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    BOOL dacl_present = FALSE;
    BOOL dacl_defaulted = FALSE;
    const auto status = GetSecurityInfo(
      handle,
      SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner,
      nullptr,
      &dacl,
      nullptr,
      &descriptor);
    if (status != ERROR_SUCCESS || descriptor == nullptr || owner == nullptr) {
      SetLastError(status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
      if (descriptor != nullptr) {
        LocalFree(descriptor);
      }
      return false;
    }
    if (!owner_is_trusted(owner, allow_current_owner)) {
      SetLastError(ERROR_ACCESS_DENIED);
      LocalFree(descriptor);
      return false;
    }
    if (GetSecurityDescriptorDacl(
          descriptor,
          &dacl_present,
          &dacl,
          &dacl_defaulted) == FALSE) {
      SetLastError(ERROR_INVALID_DATA);
      LocalFree(descriptor);
      return false;
    }
    if (!dacl_present || dacl == nullptr) {
      SetLastError(ERROR_ACCESS_DENIED);
      if (descriptor != nullptr) {
        LocalFree(descriptor);
      }
      return false;
    }

    const auto volume_root = is_directory && is_volume_root(handle);
    bool secure = true;
    for (DWORD index = 0u; index < dacl->AceCount && secure; ++index) {
      LPVOID raw_ace = nullptr;
      if (GetAce(dacl, index, &raw_ace) == FALSE || raw_ace == nullptr) {
        SetLastError(ERROR_INVALID_DATA);
        secure = false;
        break;
      }
      const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
      if (header->AceType == ACCESS_ALLOWED_COMPOUND_ACE_TYPE) {
        SetLastError(ERROR_ACCESS_DENIED);
        secure = false;
        break;
      }
      if (!is_allowed_ace_type(header->AceType)) {
        continue;
      }
      const auto parsed = parse_allowed_ace(header);
      if (!parsed) {
        SetLastError(ERROR_INVALID_DATA);
        secure = false;
        break;
      }
      if (
        (header->AceFlags & INHERIT_ONLY_ACE) != 0u &&
        volume_root) {
        continue;
      }
      if (broad_write_ace(
            parsed->sid,
            parsed->mask,
            is_directory,
            volume_root,
            allow_current_owner)) {
        SetLastError(ERROR_ACCESS_DENIED);
        secure = false;
      }
    }
    LocalFree(descriptor);
    return secure;
  }

  bool query_identity(
    HANDLE handle,
    file_identity_t &identity,
    const bool require_single_link) {
    BY_HANDLE_FILE_INFORMATION information {};
    if (GetFileInformationByHandle(handle, &information) == FALSE ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
        (require_single_link && information.nNumberOfLinks != 1u)) {
      return false;
    }
    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index_high = information.nFileIndexHigh;
    identity.file_index_low = information.nFileIndexLow;
    identity.link_count = information.nNumberOfLinks;
    return true;
  }

  bool same_identity(
    const file_identity_t &left,
    const file_identity_t &right) {
    return left.volume_serial == right.volume_serial &&
           left.file_index_high == right.file_index_high &&
           left.file_index_low == right.file_index_low;
  }

  unique_handle_t open_pinned_path(const std::filesystem::path &path) {
    return unique_handle_t {CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE,
      FILE_SHARE_READ,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  }

  std::optional<std::filesystem::path> final_path_for_handle(HANDLE handle) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
      const auto length = GetFinalPathNameByHandleW(
        handle,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED);
      if (length == 0u) {
        return std::nullopt;
      }
      if (length < buffer.size()) {
        std::wstring value(buffer.data(), length);
        if (value.rfind(L"\\\\?\\", 0u) == 0u) {
          value.erase(0u, 4u);
        }
        return std::filesystem::path {std::move(value)};
      }
      if (buffer.size() >= 32'768u) {
        return std::nullopt;
      }
      buffer.resize(buffer.size() * 2u);
    }
  }

  std::optional<std::filesystem::path> full_path_for_name(
    const std::filesystem::path &path) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
      const auto required = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
      if (required == 0u) {
        return std::nullopt;
      }
      if (required < buffer.size()) {
        return std::filesystem::path {std::wstring {buffer.data(), required}};
      }
      if (buffer.size() >= 32'768u) {
        return std::nullopt;
      }
      buffer.resize(required + 1u);
    }
  }

  std::optional<std::filesystem::path> program_files_root() {
    PWSTR raw_path = nullptr;
    if (FAILED(SHGetKnownFolderPath(
          FOLDERID_ProgramFiles,
          KF_FLAG_DEFAULT,
          nullptr,
          &raw_path)) ||
        raw_path == nullptr ||
        raw_path[0] == L'\0') {
      if (raw_path != nullptr) {
        CoTaskMemFree(raw_path);
      }
      return std::nullopt;
    }
    const std::filesystem::path root {raw_path};
    CoTaskMemFree(raw_path);
    return full_path_for_name(root);
  }

  // Production helper paths must be a strict descendant of the native 64-bit
  // Program Files subtree. This is the same canonical local-install
  // contract enforced by the MSI and bootstrapper; arbitrary fixed drives are
  // not trusted for LocalSystem execution.
  bool is_program_files_subtree(const std::filesystem::path &path) {
    const auto root = program_files_root();
    const auto full = full_path_for_name(path);
    if (!root || !full) {
      return false;
    }
    auto root_value = root->native();
    auto full_value = full->native();
    while (root_value.size() > 3u &&
           (root_value.back() == L'\\' || root_value.back() == L'/')) {
      root_value.pop_back();
    }
    while (full_value.size() > 3u &&
           (full_value.back() == L'\\' || full_value.back() == L'/')) {
      full_value.pop_back();
    }
    if (full_value.size() <= root_value.size() + 1u ||
        _wcsnicmp(
          full_value.c_str(),
          root_value.c_str(),
          root_value.size()) != 0) {
      return false;
    }
    const auto separator = full_value[root_value.size()];
    return separator == L'\\' || separator == L'/';
  }

  bool is_fixed_local_path(const std::filesystem::path &path) {
    if (!path.is_absolute()) {
      return false;
    }
    const auto root = path.root_name().native();
    if (root.empty() || root.rfind(L"\\\\", 0u) == 0u ||
        root.rfind(L"//", 0u) == 0u) {
      return false;
    }
    wchar_t volume_path[MAX_PATH] {};
    if (!GetVolumePathNameW(path.c_str(), volume_path, MAX_PATH)) {
      return false;
    }
    return GetDriveTypeW(volume_path) == DRIVE_FIXED;
  }

  std::optional<pinned_helper_t> pin_helper_path(
    const std::filesystem::path &path,
    const bool allow_current_owner) {
    if (!is_fixed_local_path(path) ||
        _wcsicmp(path.filename().c_str(), kHelperBasename.data()) != 0) {
      SetLastError(ERROR_BAD_PATHNAME);
      return std::nullopt;
    }
#if !defined(SUNSHINE_AUDIO_POLICY_PROCESS_TESTING)
    if (!is_program_files_subtree(path)) {
      SetLastError(ERROR_BAD_PATHNAME);
      return std::nullopt;
    }
#endif

    std::vector<std::filesystem::path> paths;
    auto current = path;
    for (;;) {
      paths.push_back(current);
      const auto parent = current.parent_path();
      if (parent.empty() || parent == current) {
        break;
      }
      current = parent;
    }

    pinned_helper_t pinned;
    pinned.requested_path = path;
    for (std::size_t index = paths.size(); index != 0u; --index) {
      const auto &component = paths[index - 1u];
      const auto is_helper = index == 1u;
      auto handle = open_pinned_path(component);
      if (!handle) {
        return std::nullopt;
      }
      file_identity_t identity;
      if (!query_identity(handle.get(), identity, false)) {
        SetLastError(ERROR_INVALID_DATA);
        return std::nullopt;
      }
      if (!secure_acl(handle.get(), allow_current_owner)) {
        return std::nullopt;
      }
      const auto canonical = final_path_for_handle(handle.get());
      if (!canonical) {
        SetLastError(ERROR_INVALID_DATA);
        return std::nullopt;
      }
      if (is_helper) {
        BY_HANDLE_FILE_INFORMATION information {};
        if (GetFileInformationByHandle(handle.get(), &information) == FALSE ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
            information.nNumberOfLinks != 1u) {
          SetLastError(ERROR_BAD_EXE_FORMAT);
          return std::nullopt;
        }
        pinned.canonical_path = *canonical;
        pinned.identity = identity;
      }
      pinned.ancestors.push_back(std::move(handle));
    }
    if (pinned.ancestors.empty()) {
      return std::nullopt;
    }
    const auto requested_full = full_path_for_name(path);
    if (!requested_full ||
        _wcsicmp(
          requested_full->c_str(),
          pinned.canonical_path.c_str()) != 0) {
      SetLastError(ERROR_BAD_PATHNAME);
      return std::nullopt;
    }
    return pinned;
  }

  std::optional<std::filesystem::path> module_path() {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
      const auto length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
      if (length == 0u) {
        return std::nullopt;
      }
      if (length < buffer.size() - 1u) {
        return std::filesystem::path {std::wstring {buffer.data(), length}};
      }
      if (buffer.size() >= 32'768u) {
        return std::nullopt;
      }
      buffer.resize(buffer.size() * 2u);
    }
  }

  std::optional<std::filesystem::path> production_helper_path() {
    const auto module = module_path();
    if (!module) {
      return std::nullopt;
    }
    const auto path = module->parent_path() / L"tools" / kHelperBasename;
    return path;
  }

  bool create_pipe(unique_handle_t &read_end, unique_handle_t &write_end) {
    SECURITY_ATTRIBUTES attributes {
      sizeof(SECURITY_ATTRIBUTES),
      nullptr,
      TRUE,
    };
    HANDLE read_handle = nullptr;
    HANDLE write_handle = nullptr;
    if (!CreatePipe(&read_handle, &write_handle, &attributes, kPipeBufferBytes)) {
      return false;
    }
    read_end.reset(read_handle);
    write_end.reset(write_handle);
    return true;
  }

  unique_handle_t create_discard_handle() {
    SECURITY_ATTRIBUTES attributes {
      sizeof(SECURITY_ATTRIBUTES),
      nullptr,
      TRUE,
    };
    return unique_handle_t {CreateFileW(
      L"NUL",
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr)};
  }

  bool make_parent_end_noninheritable(const unique_handle_t &handle) {
    return SetHandleInformation(handle.get(), HANDLE_FLAG_INHERIT, 0u) != FALSE;
  }

  std::vector<wchar_t> command_line_for(const std::filesystem::path &path) {
    std::wstring command_line;
    command_line.reserve(path.native().size() + 32u);
    command_line.push_back(L'"');
    std::size_t backslashes = 0u;
    for (const auto character : path.native()) {
      if (character == L'\\') {
        ++backslashes;
        continue;
      }
      if (character == L'"') {
        command_line.append(backslashes * 2u + 1u, L'\\');
        command_line.push_back(L'"');
        backslashes = 0u;
        continue;
      }
      command_line.append(backslashes, L'\\');
      backslashes = 0u;
      command_line.push_back(character);
    }
    command_line.append(backslashes * 2u, L'\\');
    command_line += L"\" --protocol=2";
    command_line.push_back(L'\0');
    return {command_line.begin(), command_line.end()};
  }

  bool write_exact(
    const unique_handle_t &handle,
    const std::vector<std::uint8_t> &encoded) {
    const auto *cursor = encoded.data();
    DWORD remaining = static_cast<DWORD>(encoded.size());
    while (remaining != 0u) {
      DWORD transferred = 0u;
      if (!WriteFile(handle.get(), cursor, remaining, &transferred, nullptr) || transferred == 0u) {
        return false;
      }
      cursor += transferred;
      remaining -= transferred;
    }
    return true;
  }

  bool read_exact(
    const unique_handle_t &handle,
    void *buffer,
    const DWORD size) {
    auto *cursor = static_cast<std::uint8_t *>(buffer);
    DWORD remaining = size;
    while (remaining != 0u) {
      DWORD transferred = 0u;
      if (!ReadFile(handle.get(), cursor, remaining, &transferred, nullptr) || transferred == 0u) {
        return false;
      }
      cursor += transferred;
      remaining -= transferred;
    }
    return true;
  }

  bool no_extra_bytes(const unique_handle_t &handle) {
    std::uint8_t byte = 0u;
    DWORD transferred = 0u;
    if (ReadFile(handle.get(), &byte, 1u, &transferred, nullptr)) {
      return transferred == 0u;
    }
    return GetLastError() == ERROR_BROKEN_PIPE;
  }

  result_t read_result(
    const unique_handle_t &result_read,
    const DWORD process_exit_code,
    const operation_e expected_operation,
    const execution_disposition_e launched_disposition) {
    result_t result;
    result.process_exit_code = process_exit_code;
    result.execution_disposition = launched_disposition;
    protocol::response_header_t header {};
    if (!read_exact(result_read, &header, static_cast<DWORD>(sizeof(header)))) {
      result.stage = failure_stage_e::protocol;
      return result;
    }
    if (header.readback_bytes > protocol::kMaxEndpointIdBytes ||
        header.readback_bytes != header.readback_code_units * sizeof(wchar_t) ||
        header.readback_bytes % sizeof(wchar_t) != 0u) {
      result.stage = failure_stage_e::protocol;
      return result;
    }

    std::vector<std::uint8_t> encoded(sizeof(header) + header.readback_bytes);
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (header.readback_bytes != 0u &&
        !read_exact(
          result_read,
          encoded.data() + sizeof(header),
          header.readback_bytes)) {
      result.stage = failure_stage_e::protocol;
      return result;
    }
    if (!no_extra_bytes(result_read)) {
      result.stage = failure_stage_e::protocol;
      return result;
    }

    protocol::response_message_t response {};
    if (!protocol::decode_response(encoded, response) ||
        response.operation != expected_operation ||
        process_exit_code != 0u) {
      result.stage = failure_stage_e::protocol;
      return result;
    }
    result.com_hresult = response.com_hresult;
    result.set_hresult = response.set_hresult;
    result.format_hresult = response.format_hresult;
    result.read_hresult = response.read_hresult;
    result.readback_id = std::move(response.readback_id);
    switch (response.status) {
      case protocol::helper_status_e::success:
        result.stage = failure_stage_e::success;
        break;
      case protocol::helper_status_e::com_failure:
        result.stage = failure_stage_e::com;
        break;
      case protocol::helper_status_e::set_failure:
        result.stage = failure_stage_e::set;
        break;
      case protocol::helper_status_e::read_failure:
        result.stage = failure_stage_e::read;
        break;
      case protocol::helper_status_e::format_failure:
        result.stage = failure_stage_e::format;
        break;
      case protocol::helper_status_e::precondition_mismatch:
        result.stage = failure_stage_e::precondition;
        break;
      case protocol::helper_status_e::pre_read_failure:
        result.stage = failure_stage_e::pre_read;
        break;
      case protocol::helper_status_e::post_set_readback_failure:
        result.stage = failure_stage_e::post_set_readback;
        break;
      case protocol::helper_status_e::invalid_request:
      default:
        result.stage = failure_stage_e::protocol;
        break;
    }
    result.execution_disposition = response.execution;
    return result;
  }

  bool set_job_limits(const unique_handle_t &job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
    limits.BasicLimitInformation.LimitFlags =
      JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1u;
    return SetInformationJobObject(
      job.get(),
      JobObjectExtendedLimitInformation,
      &limits,
      sizeof(limits)) != FALSE;
  }

  result_t terminate_unassigned_and_reap(
    const unique_handle_t &process,
    result_t result) {
    result.kill_attempted = true;
    const auto terminated = TerminateProcess(process.get(), 1u) != FALSE;
    const auto terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
    const auto wait_status = WaitForSingleObject(process.get(), kReapTimeoutMilliseconds);
    if (wait_status == WAIT_OBJECT_0) {
      result.process_reaped = true;
      // TerminateProcess can lose a race with a naturally exiting suspended
      // child. A successful wait is the authoritative reap result; preserve
      // the original launch/protocol disposition in that case.
      if (!terminated && terminate_error != ERROR_ACCESS_DENIED) {
        result.win32_error = ERROR_SUCCESS;
      }
      return result;
    }
    result.stage = failure_stage_e::reap;
    if (wait_status == WAIT_FAILED) {
      result.win32_error = GetLastError();
    } else if (!terminated) {
      result.win32_error = terminate_error;
    }
    return result;
  }

  result_t terminate_and_reap(
    const unique_handle_t &job,
    const unique_handle_t &process,
    result_t result) {
    result.kill_attempted = true;
    const auto terminated = TerminateJobObject(job.get(), 1u) != FALSE;
    const auto terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
    const auto wait_status = WaitForSingleObject(process.get(), kReapTimeoutMilliseconds);
    if (wait_status == WAIT_OBJECT_0) {
      result.process_reaped = true;
      // A timeout/cancellation disposition remains authoritative if the child
      // exited in the termination race. Do not turn an already-reaped child
      // into a spurious kill/reap failure solely because the Job was closed by
      // its natural exit first.
      if (!terminated) {
        result.win32_error = ERROR_SUCCESS;
      }
      return result;
    }
    result.stage = failure_stage_e::reap;
    if (wait_status == WAIT_FAILED) {
      result.win32_error = GetLastError();
    } else if (!terminated) {
      result.stage = failure_stage_e::kill;
      result.win32_error = terminate_error;
    }
    return result;
  }

  unique_handle_t open_identity_handle(const std::filesystem::path &path) {
    return unique_handle_t {CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  }

  bool process_image_matches(
    HANDLE process,
    const pinned_helper_t &pinned) {
    std::vector<wchar_t> buffer(512u);
    for (;;) {
      DWORD size = static_cast<DWORD>(buffer.size());
      if (QueryFullProcessImageNameW(process, 0u, buffer.data(), &size) == FALSE) {
        return false;
      }
      if (size < buffer.size() - 1u) {
        const std::filesystem::path image_path {std::wstring {buffer.data(), size}};
        auto image_handle = open_identity_handle(image_path);
        if (!image_handle) {
          return false;
        }
        file_identity_t image_identity;
        if (!query_identity(image_handle.get(), image_identity, false) ||
            !same_identity(image_identity, pinned.identity)) {
          return false;
        }
        const auto image_canonical = final_path_for_handle(image_handle.get());
        return image_canonical.has_value() &&
               _wcsicmp(
                 image_canonical->c_str(),
                 pinned.canonical_path.c_str()) == 0;
      }
      if (buffer.size() >= 32'768u) {
        return false;
      }
      buffer.resize(buffer.size() * 2u);
    }
  }

  result_t invoke_impl(
    const request_t &request,
    const std::filesystem::path &helper_path,
    const std::function<void()> &before_resume) {
    result_t result;
    if (request.stop_token.stop_requested()) {
      result.stage = failure_stage_e::cancelled;
      return result;
    }
    if (request.timeout.count() <= 0) {
      result.stage = failure_stage_e::protocol;
      return result;
    }

#if defined(SUNSHINE_AUDIO_POLICY_PROCESS_TESTING)
    constexpr bool allow_current_owner = true;
#else
    constexpr bool allow_current_owner = false;
#endif
    const auto pinned = pin_helper_path(helper_path, allow_current_owner);
    if (!pinned) {
      const auto error = GetLastError();
      return failed_result(
        failure_stage_e::launch,
        error == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : error);
    }

    protocol::request_message_t message {
      request.operation,
      request.role,
      request.endpoint_id,
      request.device_format,
      request.expected_current_id,
    };
    std::vector<std::uint8_t> encoded_request;
    if (!protocol::encode_request(message, encoded_request)) {
      result.stage = failure_stage_e::protocol;
      return result;
    }

    unique_handle_t stop_event;
    std::optional<std::stop_callback<std::function<void()>>> stop_callback;
    if (request.stop_token.stop_possible()) {
      stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      if (!stop_event) {
        return failed_result(failure_stage_e::launch, GetLastError());
      }
      stop_callback.emplace(request.stop_token, [event = stop_event.get()]() {
        SetEvent(event);
      });
      if (request.stop_token.stop_requested()) {
        result.stage = failure_stage_e::cancelled;
        return result;
      }
    }

    unique_handle_t request_read;
    unique_handle_t request_write;
    unique_handle_t result_read;
    unique_handle_t result_write;
    unique_handle_t discard_write;
    if (!create_pipe(request_read, request_write) ||
        !create_pipe(result_read, result_write) ||
        !(discard_write = create_discard_handle()) ||
        !make_parent_end_noninheritable(request_write) ||
        !make_parent_end_noninheritable(result_read)) {
      result.stage = failure_stage_e::launch;
      result.win32_error = GetLastError();
      return result;
    }

    unique_handle_t job {CreateJobObjectW(nullptr, nullptr)};
    if (!job) {
      return failed_result(failure_stage_e::launch, GetLastError());
    }
    if (!set_job_limits(job)) {
      return failed_result(failure_stage_e::launch, GetLastError());
    }

    SIZE_T attribute_size = 0u;
    InitializeProcThreadAttributeList(nullptr, 1u, 0u, &attribute_size);
    if (attribute_size == 0u) {
      return failed_result(failure_stage_e::launch, GetLastError());
    }
    std::vector<std::uint8_t> attribute_storage(attribute_size);
    auto *attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.data());
    if (!InitializeProcThreadAttributeList(
          attribute_list,
          1u,
          0u,
          &attribute_size)) {
      return failed_result(failure_stage_e::launch, GetLastError());
    }
    const HANDLE inherited_handles[] = {
      request_read.get(),
      result_write.get(),
      discard_write.get(),
    };
    if (!UpdateProcThreadAttribute(
          attribute_list,
          0u,
          PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          const_cast<HANDLE *>(inherited_handles),
          sizeof(inherited_handles),
          nullptr,
          nullptr)) {
      DeleteProcThreadAttributeList(attribute_list);
      return failed_result(failure_stage_e::launch, GetLastError());
    }

    STARTUPINFOEXW startup_info {};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdInput = request_read.get();
    startup_info.StartupInfo.hStdOutput = result_write.get();
    startup_info.StartupInfo.hStdError = discard_write.get();
    PROCESS_INFORMATION process_info {};
    auto command_line = command_line_for(pinned->canonical_path);
    const auto created = CreateProcessW(
      pinned->canonical_path.c_str(),
      command_line.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
      nullptr,
      pinned->canonical_path.parent_path().c_str(),
      &startup_info.StartupInfo,
      &process_info);
    DeleteProcThreadAttributeList(attribute_list);
    if (!created) {
      result.stage = failure_stage_e::launch;
      result.win32_error = GetLastError();
      return result;
    }
    process_handles_t process_handles {
      unique_handle_t {process_info.hProcess},
      unique_handle_t {process_info.hThread},
    };
    request_read.reset();
    result_write.reset();
    discard_write.reset();

    if (!AssignProcessToJobObject(job.get(), process_handles.process.get())) {
      result.stage = failure_stage_e::launch;
      result.win32_error = GetLastError();
      return terminate_unassigned_and_reap(process_handles.process, result);
    }
    if (!write_exact(request_write, encoded_request)) {
      result.stage = failure_stage_e::protocol;
      result.win32_error = GetLastError();
      request_write.reset();
      return terminate_and_reap(job, process_handles.process, result);
    }
    request_write.reset();

    if (before_resume) {
      before_resume();
    }
    if (!process_image_matches(process_handles.process.get(), *pinned)) {
      result.stage = failure_stage_e::launch;
      result.win32_error = ERROR_BAD_EXE_FORMAT;
      return terminate_and_reap(job, process_handles.process, result);
    }
    if ((stop_event && WaitForSingleObject(stop_event.get(), 0u) == WAIT_OBJECT_0) ||
        request.stop_token.stop_requested()) {
      result.stage = failure_stage_e::cancelled;
      return terminate_and_reap(job, process_handles.process, result);
    }
    if (ResumeThread(process_handles.thread.get()) == static_cast<DWORD>(-1)) {
      result.stage = failure_stage_e::launch;
      result.win32_error = GetLastError();
      return terminate_and_reap(job, process_handles.process, result);
    }
    result.execution_disposition =
      execution_disposition_e::child_resumed_may_have_executed;

    HANDLE wait_handles[2] {process_handles.process.get(), stop_event.get()};
    const auto wait_count = stop_event ? 2u : 1u;
    const auto timeout = static_cast<DWORD>(std::min(
      request.timeout,
      kMaximumTimeout).count());
    const auto wait_status = WaitForMultipleObjects(
      wait_count,
      wait_handles,
      FALSE,
      timeout);
    if (wait_status == WAIT_TIMEOUT) {
      result.stage = failure_stage_e::timeout;
      return terminate_and_reap(job, process_handles.process, result);
    }
    if (stop_event && wait_status == WAIT_OBJECT_0 + 1u) {
      result.stage = failure_stage_e::cancelled;
      return terminate_and_reap(job, process_handles.process, result);
    }
    if (wait_status != WAIT_OBJECT_0) {
      result.stage = failure_stage_e::reap;
      result.win32_error = GetLastError();
      return terminate_and_reap(job, process_handles.process, result);
    }

    if (!GetExitCodeProcess(process_handles.process.get(), &result.process_exit_code)) {
      result.stage = failure_stage_e::reap;
      result.win32_error = GetLastError();
      return result;
    }
    result = read_result(
      result_read,
      result.process_exit_code,
      request.operation,
      execution_disposition_e::child_resumed_may_have_executed);
    result.process_reaped = true;
    return result;
  }

}  // namespace

result_t invoke(const request_t &request) {
  const auto helper = production_helper_path();
  if (!helper) {
    return failed_result(failure_stage_e::launch, ERROR_FILE_NOT_FOUND);
  }
  return invoke_impl(request, *helper, {});
}

#if defined(SUNSHINE_AUDIO_POLICY_PROCESS_TESTING)
result_t invoke_with_helper_path(
  const request_t &request,
  const std::filesystem::path &helper_path) {
  return invoke_with_helper_path(request, helper_path, {});
}

result_t invoke_with_helper_path(
  const request_t &request,
  const std::filesystem::path &helper_path,
  const process_test_before_resume_hook_t &before_resume) {
  return invoke_impl(request, helper_path, before_resume);
}
#endif

}  // namespace platf::audio_policy
