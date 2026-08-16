/**
 * @file tests/unit/platform/windows/test_audio_policy_process.cpp
 * @brief Contract and process-boundary tests for the Windows audio policy helper.
 */

#include "../../../tests_common.h"
#include "src/platform/windows/audio_policy_process.h"

#include <Windows.h>
#include <Aclapi.h>
#include <ksmedia.h>
#include <msiquery.h>
#include <mmreg.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <cwctype>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

extern "C" void sunshine_audio_policy_helper_test_set_failure_mode(
  unsigned int mode);

extern "C" bool sunshine_audio_policy_helper_test_perform_request(
  const platf::audio_policy::protocol::request_message_t *request,
  platf::audio_policy::protocol::response_message_t *response);

extern "C" bool sunshine_audio_policy_helper_test_run_request_with_com(
  const platf::audio_policy::protocol::request_message_t *request,
  platf::audio_policy::protocol::response_message_t *response);

extern "C" unsigned int sunshine_audio_policy_helper_test_get_set_count();

namespace {

  std::filesystem::path test_helper_path() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    EXPECT_GT(length, 0u);
    buffer.resize(length);
    return std::filesystem::path {buffer}.parent_path() / L"sunshine_audio_policy_helper.exe";
  }

  platf::audio_policy::request_t request(
    platf::audio_policy::operation_e operation,
    std::wstring endpoint = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds {500},
    std::optional<platf::audio_policy::wave_format_extensible_t> format = {},
    std::wstring expected_current_id = {},
    platf::audio_policy::render_role_e role = platf::audio_policy::render_role_e::console) {
    if (operation == platf::audio_policy::operation_e::set_and_readback &&
        expected_current_id.empty()) {
      expected_current_id = L"{test-current}";
    }
    return {
      operation,
      role,
      std::move(endpoint),
      timeout,
      {},
      std::move(format),
      std::move(expected_current_id),
    };
  }

  platf::audio_policy::wave_format_extensible_t valid_stereo_format() {
    platf::audio_policy::wave_format_extensible_t format {};
    format.format_tag = WAVE_FORMAT_EXTENSIBLE;
    format.channels = 2u;
    format.samples_per_sec = 48'000u;
    format.bits_per_sample = 32u;
    format.valid_bits_per_sample = 24u;
    format.block_align = 8u;
    format.avg_bytes_per_sec = 384'000u;
    format.cb_size = 22u;
    format.channel_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.sub_format = {
      0x01u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x10u, 0x00u,
      0x80u, 0x00u, 0x00u, 0xAAu,
      0x00u, 0x38u, 0x9Bu, 0x71u,
    };
    return format;
  }

  bool encodes_valid_format(
    const platf::audio_policy::wave_format_extensible_t &format) {
    std::vector<std::uint8_t> encoded;
    return platf::audio_policy::protocol::encode_request({
      platf::audio_policy::operation_e::set_device_format,
      platf::audio_policy::render_role_e::console,
      L"{endpoint}",
      format,
    }, encoded);
  }

  platf::audio_policy::result_t invoke_test_helper(
    const platf::audio_policy::request_t &request_value,
    const std::function<void()> &before_resume = {}) {
    return platf::audio_policy::invoke_with_helper_path(
      request_value,
      test_helper_path(),
      before_resume);
  }

  bool copy_test_helper_to(const std::filesystem::path &destination) {
    return CopyFileW(test_helper_path().c_str(), destination.c_str(), FALSE) != FALSE;
  }

  std::filesystem::path temporary_helper_directory(const wchar_t *suffix) {
    const auto directory = std::filesystem::temp_directory_path() /
      (L"vibepollo-audio-policy-" + std::to_wstring(GetCurrentProcessId()) + L"-" + suffix);
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
  }

  bool grant_users_write_access(
    const std::filesystem::path &path,
    const ACCESS_MASK permissions = FILE_GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER) {
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    PACL old_acl = nullptr;
    const auto query_status = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      &old_acl,
      nullptr,
      &security_descriptor);
    if (query_status != ERROR_SUCCESS) {
      return false;
    }

    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID users_sid = nullptr;
    const auto sid_created = AllocateAndInitializeSid(
      &authority,
      2u,
      SECURITY_BUILTIN_DOMAIN_RID,
      DOMAIN_ALIAS_RID_USERS,
      0u,
      0u,
      0u,
      0u,
      0u,
      0u,
      &users_sid) != FALSE;
    if (!sid_created) {
      LocalFree(security_descriptor);
      return false;
    }

    EXPLICIT_ACCESS_W entry {};
    entry.grfAccessPermissions = permissions;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = NO_INHERITANCE;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    entry.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(users_sid);
    PACL new_acl = nullptr;
    const auto acl_status = SetEntriesInAclW(1u, &entry, old_acl, &new_acl);
    const auto set_status = acl_status == ERROR_SUCCESS
      ? SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        new_acl,
        nullptr)
      : acl_status;
    if (new_acl != nullptr) {
      LocalFree(new_acl);
    }
    FreeSid(users_sid);
    LocalFree(security_descriptor);
    return set_status == ERROR_SUCCESS;
  }

  bool add_arbitrary_allow_ace(
    const std::filesystem::path &path,
    const BYTE ace_type,
    const ACCESS_MASK mask,
    const bool protect_dacl) {
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    PACL old_acl = nullptr;
    const auto query_status = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()),
      SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      &old_acl,
      nullptr,
      &security_descriptor);
    if (query_status != ERROR_SUCCESS || old_acl == nullptr) {
      if (security_descriptor != nullptr) {
        LocalFree(security_descriptor);
      }
      return false;
    }

    PSID arbitrary_sid = nullptr;
    if (!ConvertStringSidToSidW(
          L"S-1-5-21-42424242-42424242-42424242-4242",
          &arbitrary_sid)) {
      LocalFree(security_descriptor);
      return false;
    }
    const auto sid_size = GetLengthSid(arbitrary_sid);
    if (sid_size == 0u) {
      LocalFree(arbitrary_sid);
      LocalFree(security_descriptor);
      return false;
    }

    const auto object_ace =
      ace_type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
      ace_type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
    const auto callback_ace =
      ace_type == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
      ace_type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
    const auto object_flags = object_ace ? ACE_OBJECT_TYPE_PRESENT : 0u;
    const auto ace_size = static_cast<WORD>(
      sizeof(ACE_HEADER) + sizeof(ACCESS_MASK) +
      (object_ace ? sizeof(DWORD) + sizeof(GUID) : 0u) + sid_size +
      (callback_ace ? 4u : 0u));
    std::vector<std::uint8_t> ace_bytes(ace_size, 0u);
    auto *header = reinterpret_cast<ACE_HEADER *>(ace_bytes.data());
    header->AceType = ace_type;
    header->AceSize = ace_size;
    std::memcpy(
      ace_bytes.data() + sizeof(ACE_HEADER),
      &mask,
      sizeof(mask));
    auto sid_offset = sizeof(ACE_HEADER) + sizeof(ACCESS_MASK);
    if (object_ace) {
      std::memcpy(
        ace_bytes.data() + sid_offset,
        &object_flags,
        sizeof(object_flags));
      sid_offset += sizeof(object_flags);
      const GUID object_type {
        0xAABBCCDDu,
        0x1122u,
        0x3344u,
        {0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xAAu, 0xBBu, 0xCCu},
      };
      std::memcpy(
        ace_bytes.data() + sid_offset,
        &object_type,
        sizeof(object_type));
      sid_offset += sizeof(object_type);
    }
    std::memcpy(ace_bytes.data() + sid_offset, arbitrary_sid, sid_size);

    ACL_SIZE_INFORMATION acl_information {};
    if (!GetAclInformation(
          old_acl,
          &acl_information,
          sizeof(acl_information),
          AclSizeInformation)) {
      LocalFree(arbitrary_sid);
      LocalFree(security_descriptor);
      return false;
    }
    std::vector<std::uint8_t> acl_bytes(
      acl_information.AclBytesInUse + ace_bytes.size() + 64u,
      0u);
    auto *new_acl = reinterpret_cast<PACL>(acl_bytes.data());
    if (!InitializeAcl(
          new_acl,
          static_cast<DWORD>(acl_bytes.size()),
          ACL_REVISION_DS)) {
      LocalFree(arbitrary_sid);
      LocalFree(security_descriptor);
      return false;
    }
    for (DWORD index = 0u; index < old_acl->AceCount; ++index) {
      LPVOID old_ace = nullptr;
      if (!GetAce(old_acl, index, &old_ace) || old_ace == nullptr) {
        LocalFree(arbitrary_sid);
        LocalFree(security_descriptor);
        return false;
      }
      const auto *old_header = static_cast<const ACE_HEADER *>(old_ace);
      if (!AddAce(
            new_acl,
            ACL_REVISION_DS,
            MAXDWORD,
            old_ace,
            old_header->AceSize)) {
        LocalFree(arbitrary_sid);
        LocalFree(security_descriptor);
        return false;
      }
    }
    if (!AddAce(
          new_acl,
          ACL_REVISION_DS,
          MAXDWORD,
          ace_bytes.data(),
          static_cast<DWORD>(ace_bytes.size()))) {
      LocalFree(arbitrary_sid);
      LocalFree(security_descriptor);
      return false;
    }

    auto security_information = DACL_SECURITY_INFORMATION;
    if (protect_dacl) {
      security_information |= PROTECTED_DACL_SECURITY_INFORMATION;
    }
    const auto set_status = SetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()),
      SE_FILE_OBJECT,
      security_information,
      nullptr,
      nullptr,
      new_acl,
      nullptr);
    LocalFree(arbitrary_sid);
    LocalFree(security_descriptor);
    return set_status == ERROR_SUCCESS;
  }

  platf::audio_policy::result_t invoke_with_acl_ace(
    const BYTE ace_type,
    const ACCESS_MASK mask,
    const bool protect_dacl) {
    const auto directory = temporary_helper_directory(L"acl-ace");
    const auto helper = directory / L"sunshine_audio_policy_helper.exe";
    EXPECT_TRUE(copy_test_helper_to(helper));
    EXPECT_TRUE(add_arbitrary_allow_ace(helper, ace_type, mask, protect_dacl));
    const auto result = platf::audio_policy::invoke_with_helper_path(
      request(platf::audio_policy::operation_e::read),
      helper);
    std::filesystem::remove_all(directory);
    return result;
  }

  struct msi_handle_t {
    MSIHANDLE value {0};

    msi_handle_t() = default;

    ~msi_handle_t() {
      if (value != 0) {
        MsiCloseHandle(value);
      }
    }

    msi_handle_t(const msi_handle_t &) = delete;
    msi_handle_t &operator=(const msi_handle_t &) = delete;
  };

  struct msi_action_row_t {
    std::wstring condition;
    int sequence {0};
  };

  std::filesystem::path test_msi_path() {
    std::wstring buffer(32768u, L'\0');
    const auto length = GetModuleFileNameW(
      nullptr,
      buffer.data(),
      static_cast<DWORD>(buffer.size()));
    if (length == 0u) {
      return {};
    }
    buffer.resize(length);
    return std::filesystem::path {buffer}.parent_path().parent_path() /
      L"cpack_artifacts/Vibepollo.msi";
  }

  std::optional<msi_action_row_t> read_msi_action_row(
    const MSIHANDLE database,
    const std::wstring &action,
    bool &duplicate) {
    duplicate = false;
    const auto query =
      L"SELECT Condition, Sequence FROM InstallExecuteSequence WHERE Action = '" +
      action + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    msi_handle_t record;
    if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    wchar_t condition[512] {};
    DWORD condition_size = static_cast<DWORD>(std::size(condition));
    if (MsiRecordGetStringW(
          record.value,
          1u,
          condition,
          &condition_size) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    const auto sequence = MsiRecordGetInteger(record.value, 2u);
    if (sequence == MSI_NULL_INTEGER) {
      return std::nullopt;
    }

    msi_handle_t duplicate_record;
    duplicate = MsiViewFetch(view.value, &duplicate_record.value) == ERROR_SUCCESS;
    return msi_action_row_t {std::wstring {condition, condition_size}, sequence};
  }

  std::optional<std::wstring> read_msi_custom_action_target(
    const MSIHANDLE database,
    const std::wstring &action,
    bool &duplicate) {
    duplicate = false;
    const auto query =
      L"SELECT Target FROM CustomAction WHERE Action = '" + action + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    msi_handle_t record;
    if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    std::wstring target(8192u, L'\0');
    DWORD target_size = static_cast<DWORD>(target.size());
    auto status = MsiRecordGetStringW(
      record.value,
      1u,
      target.data(),
      &target_size);
    if (status != ERROR_SUCCESS) {
      return std::nullopt;
    }
    target.resize(target_size);
    msi_handle_t duplicate_record;
    duplicate = MsiViewFetch(view.value, &duplicate_record.value) == ERROR_SUCCESS;
    return target;
  }

  std::optional<int> read_msi_custom_action_integer(
    const MSIHANDLE database,
    const std::wstring &action,
    const wchar_t *column) {
    const auto query = L"SELECT `" + std::wstring {column} +
      L"` FROM `CustomAction` WHERE `Action` = '" + action + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    msi_handle_t record;
    if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    const auto value = MsiRecordGetInteger(record.value, 1u);
    return value == MSI_NULL_INTEGER ? std::nullopt : std::optional<int> {value};
  }

  std::optional<std::wstring> read_msi_custom_action_string(
    const MSIHANDLE database,
    const std::wstring &action,
    const wchar_t *column) {
    const auto query = L"SELECT `" + std::wstring {column} +
      L"` FROM `CustomAction` WHERE `Action` = '" + action + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    msi_handle_t record;
    if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    std::wstring value(32768u, L'\0');
    DWORD size = static_cast<DWORD>(value.size());
    if (MsiRecordGetStringW(record.value, 1u, value.data(), &size) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    value.resize(size);
    return value;
  }

  std::optional<std::wstring> read_msi_property_value(
    const MSIHANDLE database,
    const std::wstring &property) {
    const auto query = L"SELECT `Value` FROM `Property` WHERE `Property` = '" +
      property + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    msi_handle_t record;
    if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    std::wstring value(32768u, L'\0');
    DWORD size = static_cast<DWORD>(value.size());
    if (MsiRecordGetStringW(record.value, 1u, value.data(), &size) != ERROR_SUCCESS) {
      return std::nullopt;
    }
    value.resize(size);
    return value;
  }

  std::size_t count_msi_binary_rows(
    const MSIHANDLE database,
    const std::wstring &name) {
    const auto query = L"SELECT `Name` FROM `Binary` WHERE `Name` = '" + name + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return 0u;
    }
    std::size_t count = 0u;
    for (;;) {
      msi_handle_t record;
      if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
        break;
      }
      ++count;
    }
    return count;
  }

  std::size_t count_msi_launch_condition_rows(
    const MSIHANDLE database,
    const std::wstring &condition) {
    const auto query = L"SELECT `Condition` FROM `LaunchCondition` WHERE `Condition` = '" +
      condition + L"'";
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(database, query.c_str(), &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return 0u;
    }
    std::size_t count = 0u;
    for (;;) {
      msi_handle_t record;
      if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
        break;
      }
      ++count;
    }
    return count;
  }

  std::size_t count_msi_file_rows(
    const MSIHANDLE database,
    const std::wstring &needle) {
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(
          database,
          L"SELECT FileName FROM File",
          &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return 0u;
    }
    std::size_t count = 0u;
    for (;;) {
      msi_handle_t record;
      if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
        break;
      }
      wchar_t file_name[1024] {};
      DWORD file_name_size = static_cast<DWORD>(std::size(file_name));
      if (MsiRecordGetStringW(
            record.value,
            1u,
            file_name,
            &file_name_size) != ERROR_SUCCESS) {
        continue;
      }
      std::wstring lower_name {file_name, file_name_size};
      std::ranges::transform(
        lower_name,
        lower_name.begin(),
        [](const wchar_t character) {
          return static_cast<wchar_t>(std::towlower(character));
        });
      if (lower_name.find(needle) != std::wstring::npos) {
        ++count;
      }
    }
    return count;
  }

  bool msi_has_executable_install_root_powershell(
    const MSIHANDLE database) {
    msi_handle_t view;
    if (MsiDatabaseOpenViewW(
          database,
          L"SELECT `Target` FROM `CustomAction`",
          &view.value) != ERROR_SUCCESS ||
        MsiViewExecute(view.value, 0) != ERROR_SUCCESS) {
      return true;
    }
    for (;;) {
      msi_handle_t record;
      if (MsiViewFetch(view.value, &record.value) != ERROR_SUCCESS) {
        return false;
      }
      std::wstring target(32768u, L'\0');
      DWORD size = static_cast<DWORD>(target.size());
      if (MsiRecordGetStringW(
            record.value,
            1u,
            target.data(),
            &size) != ERROR_SUCCESS) {
        return true;
      }
      target.resize(size);
      std::ranges::transform(
        target,
        target.begin(),
        [](const wchar_t character) {
          return static_cast<wchar_t>(std::towlower(character));
        });
      if (target.find(L"powershell") != std::wstring::npos &&
          (target.find(L"-command") != std::wstring::npos ||
           target.find(L" -c ") != std::wstring::npos) &&
          target.find(L"[install_root]") != std::wstring::npos) {
        return true;
      }
    }
  }

}  // namespace

TEST(AudioPolicyProtocol, RejectsReservedBytesAndLengthDrift) {
  using namespace platf::audio_policy::protocol;
  const request_message_t message {operation_e::read, render_role_e::console, {}};
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request(message, encoded));

  request_header_t header {};
  std::memcpy(&header, encoded.data(), sizeof(header));
  header.reserved0 = 1u;
  std::memcpy(encoded.data(), &header, sizeof(header));
  request_message_t decoded {};
  EXPECT_FALSE(decode_request(encoded, decoded));

  header.reserved0 = 0u;
  header.endpoint_bytes = sizeof(wchar_t);
  std::memcpy(encoded.data(), &header, sizeof(header));
  EXPECT_FALSE(decode_request(encoded, decoded));
}

TEST(AudioPolicyProtocol, RejectsInvalidOrUnboundedUtf16Endpoint) {
  using namespace platf::audio_policy::protocol;
  std::vector<std::uint8_t> encoded;

  EXPECT_FALSE(encode_request(
    {operation_e::set_and_readback, render_role_e::console, std::wstring(1u, L'\0')},
    encoded));
  EXPECT_FALSE(encode_request(
    {operation_e::set_and_readback, render_role_e::console, std::wstring(1u, static_cast<wchar_t>(0xD800u))},
    encoded));
  EXPECT_FALSE(encode_request(
    {operation_e::set_and_readback, render_role_e::console, std::wstring(kMaxEndpointIdCodeUnits + 1u, L'x')},
    encoded));
}

TEST(AudioPolicyProtocol, PreservesExactUtf16CodeUnits) {
  using namespace platf::audio_policy::protocol;
  const std::wstring endpoint {L"{opaque-\u03bb-endpoint}"};
  request_message_t message {operation_e::set_and_readback, render_role_e::communications, endpoint};
  message.expected_current_id = L"{expected-current}";
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request(message, encoded));

  request_message_t decoded {};
  ASSERT_TRUE(decode_request(encoded, decoded));
  EXPECT_EQ(decoded.operation, operation_e::set_and_readback);
  EXPECT_EQ(decoded.role, render_role_e::communications);
  EXPECT_EQ(decoded.endpoint_id, endpoint);
}

TEST(AudioPolicyProtocol, RoundTripsCompareAndSetExpectedCurrentId) {
  using namespace platf::audio_policy::protocol;
  request_message_t message {
    operation_e::set_and_readback,
    render_role_e::communications,
    L"{target-endpoint}",
  };
  message.expected_current_id = L"{expected-current-endpoint}";

  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request(message, encoded));

  request_message_t decoded {};
  ASSERT_TRUE(decode_request(encoded, decoded));
  EXPECT_EQ(decoded.endpoint_id, message.endpoint_id);
  EXPECT_EQ(decoded.expected_current_id, message.expected_current_id);
}

TEST(AudioPolicyProtocol, RejectsMissingOrInvalidCompareAndSetExpectedId) {
  using namespace platf::audio_policy::protocol;
  std::vector<std::uint8_t> encoded;
  EXPECT_FALSE(encode_request(
    {operation_e::set_and_readback, render_role_e::console, L"{target}"},
    encoded));

  request_message_t invalid {
    operation_e::set_and_readback,
    render_role_e::console,
    L"{target}",
  };
  invalid.expected_current_id = std::wstring(1u, L'\0');
  EXPECT_FALSE(encode_request(invalid, encoded));
  invalid.expected_current_id = std::wstring(1u, static_cast<wchar_t>(0xD800u));
  EXPECT_FALSE(encode_request(invalid, encoded));
  invalid.expected_current_id = std::wstring(kMaxEndpointIdCodeUnits + 1u, L'x');
  EXPECT_FALSE(encode_request(invalid, encoded));

  invalid.expected_current_id = L"{unexpected}";
  invalid.operation = operation_e::read;
  invalid.endpoint_id.clear();
  EXPECT_FALSE(encode_request(invalid, encoded));
  invalid.operation = operation_e::set_device_format;
  invalid.endpoint_id = L"{target}";
  invalid.expected_current_id = L"{unexpected}";
  invalid.device_format = valid_stereo_format();
  EXPECT_FALSE(encode_request(invalid, encoded));
}

TEST(AudioPolicyProtocol, RejectsContradictoryResponseStatusAndHresults) {
  using namespace platf::audio_policy::protocol;
  const auto valid_set_response = [] {
    response_message_t response {};
    response.operation = operation_e::set_and_readback;
    response.status = helper_status_e::success;
    response.com_hresult = S_OK;
    response.set_hresult = S_OK;
    response.read_hresult = S_OK;
    response.readback_id = L"{current}";
    response.execution = execution_disposition_e::completed;
    return response;
  };
  const auto rejects = [](response_message_t response) {
    std::vector<std::uint8_t> encoded;
    EXPECT_FALSE(encode_response(response, encoded));
  };

  auto set_failure = valid_set_response();
  set_failure.status = helper_status_e::set_failure;
  rejects(set_failure);

  auto precondition = valid_set_response();
  precondition.status = helper_status_e::precondition_mismatch;
  precondition.set_hresult = S_OK;
  rejects(precondition);

  auto read_failure = valid_set_response();
  read_failure.status = helper_status_e::read_failure;
  read_failure.read_hresult = S_OK;
  rejects(read_failure);

  auto pre_read_failure = valid_set_response();
  pre_read_failure.status = helper_status_e::pre_read_failure;
  pre_read_failure.read_hresult = E_FAIL;
  pre_read_failure.set_hresult = E_FAIL;
  pre_read_failure.readback_id.clear();
  rejects(pre_read_failure);

  auto post_set_read_failure = valid_set_response();
  post_set_read_failure.status = helper_status_e::post_set_readback_failure;
  post_set_read_failure.com_hresult = E_FAIL;
  post_set_read_failure.read_hresult = E_FAIL;
  post_set_read_failure.readback_id.clear();
  rejects(post_set_read_failure);

  response_message_t format_failure {};
  format_failure.operation = operation_e::set_device_format;
  format_failure.status = helper_status_e::format_failure;
  format_failure.com_hresult = S_OK;
  format_failure.set_hresult = E_UNEXPECTED;
  format_failure.format_hresult = S_OK;
  format_failure.read_hresult = E_UNEXPECTED;
  format_failure.execution = execution_disposition_e::completed;
  rejects(format_failure);

  auto valid = valid_set_response();
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_response(valid, encoded));
  response_header_t header {};
  std::memcpy(&header, encoded.data(), sizeof(header));
  header.set_hresult = S_OK;
  header.status = static_cast<std::uint16_t>(helper_status_e::set_failure);
  std::memcpy(encoded.data(), &header, sizeof(header));
  response_message_t decoded {};
  EXPECT_FALSE(decode_response(encoded, decoded));
}

TEST(AudioPolicyProtocol, AcceptsExactOperationStatusMatrixOnRawWire) {
  using namespace platf::audio_policy::protocol;
  const auto round_trip = [](const response_message_t &response) {
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(encode_response(response, encoded));
    response_message_t decoded {};
    ASSERT_TRUE(decode_response(encoded, decoded));
    EXPECT_EQ(decoded.operation, response.operation);
    EXPECT_EQ(decoded.status, response.status);
    EXPECT_EQ(decoded.com_hresult, response.com_hresult);
    EXPECT_EQ(decoded.set_hresult, response.set_hresult);
    EXPECT_EQ(decoded.format_hresult, response.format_hresult);
    EXPECT_EQ(decoded.read_hresult, response.read_hresult);
    EXPECT_EQ(decoded.readback_id, response.readback_id);
  };

  response_message_t read_success {};
  read_success.operation = operation_e::read;
  read_success.status = helper_status_e::success;
  read_success.com_hresult = S_OK;
  read_success.set_hresult = E_UNEXPECTED;
  read_success.format_hresult = E_UNEXPECTED;
  read_success.read_hresult = S_OK;
  read_success.readback_id = L"{readback}";
  round_trip(read_success);

  auto read_com = read_success;
  read_com.status = helper_status_e::com_failure;
  read_com.com_hresult = E_FAIL;
  read_com.read_hresult = E_FAIL;
  read_com.readback_id.clear();
  round_trip(read_com);

  auto read_failure = read_success;
  read_failure.status = helper_status_e::read_failure;
  read_failure.read_hresult = E_FAIL;
  read_failure.readback_id.clear();
  round_trip(read_failure);

  response_message_t set_success {};
  set_success.operation = operation_e::set_and_readback;
  set_success.status = helper_status_e::success;
  set_success.com_hresult = S_OK;
  set_success.set_hresult = S_OK;
  set_success.format_hresult = E_UNEXPECTED;
  set_success.read_hresult = S_OK;
  set_success.readback_id = L"{current}";
  round_trip(set_success);

  auto set_com = set_success;
  set_com.status = helper_status_e::com_failure;
  set_com.com_hresult = E_FAIL;
  set_com.set_hresult = E_FAIL;
  set_com.read_hresult = E_UNEXPECTED;
  set_com.readback_id.clear();
  round_trip(set_com);

  auto set_post_read_com = set_success;
  set_post_read_com.status = helper_status_e::post_set_readback_failure;
  set_post_read_com.com_hresult = S_OK;
  set_post_read_com.set_hresult = S_OK;
  set_post_read_com.read_hresult = E_FAIL;
  set_post_read_com.readback_id.clear();
  round_trip(set_post_read_com);

  auto set_failure = set_success;
  set_failure.status = helper_status_e::set_failure;
  set_failure.set_hresult = E_FAIL;
  round_trip(set_failure);

  auto precondition = set_success;
  precondition.status = helper_status_e::precondition_mismatch;
  precondition.set_hresult = kPreconditionMismatchHresult;
  round_trip(precondition);

  auto set_read_failure = set_success;
  set_read_failure.status = helper_status_e::post_set_readback_failure;
  set_read_failure.read_hresult = E_FAIL;
  set_read_failure.readback_id.clear();
  round_trip(set_read_failure);

  response_message_t format_success {};
  format_success.operation = operation_e::set_device_format;
  format_success.status = helper_status_e::success;
  format_success.com_hresult = S_OK;
  format_success.set_hresult = E_UNEXPECTED;
  format_success.format_hresult = S_OK;
  format_success.read_hresult = E_UNEXPECTED;
  round_trip(format_success);

  auto format_com = format_success;
  format_com.status = helper_status_e::com_failure;
  format_com.com_hresult = E_FAIL;
  format_com.format_hresult = E_FAIL;
  round_trip(format_com);

  auto format_failure = format_success;
  format_failure.status = helper_status_e::format_failure;
  format_failure.format_hresult = E_FAIL;
  round_trip(format_failure);
}

TEST(AudioPolicyProtocol, AuthenticatesPreReadAndPostSetWriteDisposition) {
  using namespace platf::audio_policy::protocol;
  const auto pre_read_failure = helper_status_e::pre_read_failure;
  const auto post_set_read_failure = helper_status_e::post_set_readback_failure;
  const auto round_trip = [](const response_message_t &expected) {
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(encode_response(expected, encoded));
    response_message_t decoded {};
    ASSERT_TRUE(decode_response(encoded, decoded));
    EXPECT_EQ(decoded.status, expected.status);
    EXPECT_EQ(decoded.com_hresult, expected.com_hresult);
    EXPECT_EQ(decoded.set_hresult, expected.set_hresult);
    EXPECT_EQ(decoded.read_hresult, expected.read_hresult);
    EXPECT_TRUE(decoded.readback_id.empty());
  };

  response_message_t pre_read {
    pre_read_failure,
    E_FAIL,
    S_OK,
    E_FAIL,
    {},
  };
  pre_read.operation = operation_e::set_and_readback;
  pre_read.format_hresult = E_UNEXPECTED;
  round_trip(pre_read);

  response_message_t post_set {
    post_set_read_failure,
    S_OK,
    S_OK,
    E_FAIL,
    {},
  };
  post_set.operation = operation_e::set_and_readback;
  post_set.format_hresult = E_UNEXPECTED;
  round_trip(post_set);
}

TEST(AudioPolicyProtocol, RejectsContradictoryWriteDispositionHresults) {
  using namespace platf::audio_policy::protocol;
  const auto pre_read_failure = static_cast<helper_status_e>(7u);
  const auto post_set_read_failure = static_cast<helper_status_e>(8u);
  std::vector<std::uint8_t> encoded;

  response_message_t pre_read {
    pre_read_failure,
    E_FAIL,
    E_FAIL,
    E_FAIL,
    {},
  };
  pre_read.operation = operation_e::set_and_readback;
  pre_read.format_hresult = E_UNEXPECTED;
  EXPECT_FALSE(encode_response(pre_read, encoded));

  response_message_t post_set {
    post_set_read_failure,
    S_OK,
    E_FAIL,
    E_FAIL,
    {},
  };
  post_set.operation = operation_e::set_and_readback;
  post_set.format_hresult = E_UNEXPECTED;
  EXPECT_FALSE(encode_response(post_set, encoded));
}

TEST(AudioPolicyProtocol, EncodesRealReadEnumeratorComFailurePath) {
  using namespace platf::audio_policy::protocol;
  const request_message_t request {
    operation_e::read,
    render_role_e::console,
    {},
  };
  response_message_t response {};
  sunshine_audio_policy_helper_test_set_failure_mode(1u);
  ASSERT_TRUE(sunshine_audio_policy_helper_test_perform_request(&request, &response));
  sunshine_audio_policy_helper_test_set_failure_mode(0u);

  EXPECT_EQ(response.status, helper_status_e::com_failure);
  EXPECT_EQ(response.com_hresult, E_FAIL);
  EXPECT_EQ(response.read_hresult, E_FAIL);
  EXPECT_TRUE(response.readback_id.empty());
}

TEST(AudioPolicyProtocol, EncodesRealPostSetReadbackComFailurePath) {
  using namespace platf::audio_policy::protocol;
  request_message_t request {
    operation_e::set_and_readback,
    render_role_e::console,
    L"{target}",
  };
  request.expected_current_id = L"{expected-current}";
  response_message_t response {};
  sunshine_audio_policy_helper_test_set_failure_mode(2u);
  ASSERT_TRUE(sunshine_audio_policy_helper_test_perform_request(&request, &response));
  const auto set_count = sunshine_audio_policy_helper_test_get_set_count();
  sunshine_audio_policy_helper_test_set_failure_mode(0u);

  EXPECT_EQ(response.status, helper_status_e::post_set_readback_failure);
  EXPECT_EQ(response.com_hresult, S_OK);
  EXPECT_EQ(response.set_hresult, S_OK);
  EXPECT_EQ(response.read_hresult, E_FAIL);
  EXPECT_TRUE(response.readback_id.empty());
  EXPECT_EQ(set_count, 1u);
}

TEST(AudioPolicyProtocol, RealPreReadFailureProvesSetWasNotCalled) {
  using namespace platf::audio_policy::protocol;
  request_message_t request {
    operation_e::set_and_readback,
    render_role_e::console,
    L"{target}",
  };
  request.expected_current_id = L"{expected-current}";
  response_message_t response {};
  sunshine_audio_policy_helper_test_set_failure_mode(1u);
  ASSERT_TRUE(sunshine_audio_policy_helper_test_perform_request(&request, &response));
  const auto set_count = sunshine_audio_policy_helper_test_get_set_count();
  sunshine_audio_policy_helper_test_set_failure_mode(0u);

  EXPECT_EQ(response.status, helper_status_e::pre_read_failure);
  EXPECT_EQ(response.set_hresult, S_OK);
  EXPECT_TRUE(FAILED(response.read_hresult));
  EXPECT_EQ(set_count, 0u);
}

TEST(AudioPolicyProtocol, TopLevelComInitFailureAuthenticatesSetAsUnissuedPreRead) {
  using namespace platf::audio_policy::protocol;
  request_message_t request {
    operation_e::set_and_readback,
    render_role_e::console,
    L"{target}",
  };
  request.expected_current_id = L"{expected-current}";
  response_message_t response {};
  sunshine_audio_policy_helper_test_set_failure_mode(3u);
  ASSERT_TRUE(sunshine_audio_policy_helper_test_run_request_with_com(&request, &response));
  const auto set_count = sunshine_audio_policy_helper_test_get_set_count();
  sunshine_audio_policy_helper_test_set_failure_mode(0u);

  EXPECT_EQ(response.status, helper_status_e::pre_read_failure);
  EXPECT_EQ(response.com_hresult, E_FAIL);
  EXPECT_EQ(response.set_hresult, S_OK);
  EXPECT_EQ(response.format_hresult, E_UNEXPECTED);
  EXPECT_EQ(response.read_hresult, E_FAIL);
  EXPECT_TRUE(response.readback_id.empty());
  EXPECT_EQ(response.execution, execution_disposition_e::completed);
  EXPECT_EQ(set_count, 0u);
}

TEST(AudioPolicyProtocol, TopLevelComInitFailurePreservesReadAndFormatMatrices) {
  using namespace platf::audio_policy::protocol;
  sunshine_audio_policy_helper_test_set_failure_mode(3u);

  const request_message_t read_request {
    operation_e::read,
    render_role_e::multimedia,
    {},
  };
  response_message_t read_response {};
  ASSERT_TRUE(sunshine_audio_policy_helper_test_run_request_with_com(
    &read_request,
    &read_response));
  EXPECT_EQ(read_response.status, helper_status_e::com_failure);
  EXPECT_EQ(read_response.com_hresult, E_FAIL);
  EXPECT_EQ(read_response.set_hresult, E_UNEXPECTED);
  EXPECT_EQ(read_response.format_hresult, E_UNEXPECTED);
  EXPECT_EQ(read_response.read_hresult, E_FAIL);
  EXPECT_TRUE(read_response.readback_id.empty());

  const request_message_t format_request {
    operation_e::set_device_format,
    render_role_e::communications,
    L"{format-target}",
    valid_stereo_format(),
  };
  response_message_t format_response {};
  ASSERT_TRUE(sunshine_audio_policy_helper_test_run_request_with_com(
    &format_request,
    &format_response));
  const auto set_count = sunshine_audio_policy_helper_test_get_set_count();
  sunshine_audio_policy_helper_test_set_failure_mode(0u);
  EXPECT_EQ(format_response.status, helper_status_e::com_failure);
  EXPECT_EQ(format_response.com_hresult, E_FAIL);
  EXPECT_EQ(format_response.set_hresult, E_UNEXPECTED);
  EXPECT_EQ(format_response.format_hresult, E_FAIL);
  EXPECT_EQ(format_response.read_hresult, E_UNEXPECTED);
  EXPECT_TRUE(format_response.readback_id.empty());
  EXPECT_EQ(set_count, 0u);
}

TEST(AudioPolicyProtocol, RealPostSetReadbackFailureRetainsWriteHazard) {
  using namespace platf::audio_policy::protocol;
  request_message_t request {
    operation_e::set_and_readback,
    render_role_e::console,
    L"{target}",
  };
  request.expected_current_id = L"{expected-current}";
  response_message_t response {};
  sunshine_audio_policy_helper_test_set_failure_mode(2u);
  ASSERT_TRUE(sunshine_audio_policy_helper_test_perform_request(&request, &response));
  const auto set_count = sunshine_audio_policy_helper_test_get_set_count();
  sunshine_audio_policy_helper_test_set_failure_mode(0u);

  EXPECT_EQ(response.status, helper_status_e::post_set_readback_failure);
  EXPECT_EQ(response.set_hresult, S_OK);
  EXPECT_EQ(response.read_hresult, E_FAIL);
  EXPECT_EQ(set_count, 1u);
}

TEST(AudioPolicyProtocol, AcceptsTheBoundedMaximumFrameWithoutTruncation) {
  using namespace platf::audio_policy::protocol;
  const auto endpoint = std::wstring(kMaxEndpointIdCodeUnits, L'x');
  request_message_t maximum_request {
    operation_e::set_and_readback,
    render_role_e::console,
    endpoint,
  };
  maximum_request.expected_current_id = std::wstring(kMaxEndpointIdCodeUnits, L'y');
  std::vector<std::uint8_t> encoded_request;
  ASSERT_TRUE(encode_request(maximum_request, encoded_request));
  EXPECT_EQ(
    encoded_request.size(),
    sizeof(request_header_t) + kMaxEndpointIdBytes + kMaxEndpointIdBytes);

  std::vector<std::uint8_t> encoded_response;
  response_message_t maximum_response {
    helper_status_e::success,
    S_OK,
    E_UNEXPECTED,
    S_OK,
    endpoint,
  };
  maximum_response.operation = operation_e::read;
  ASSERT_TRUE(encode_response(maximum_response, encoded_response));
  EXPECT_EQ(
    encoded_response.size(),
    sizeof(response_header_t) + kMaxEndpointIdBytes);
}

TEST(AudioPolicyProtocol, RoundTripsExactSetDeviceFormatRequest) {
  using namespace platf::audio_policy::protocol;
  const auto endpoint = std::wstring {L"{opaque-format-endpoint}"};
  const auto format = valid_stereo_format();
  const request_message_t message {
    operation_e::set_device_format,
    render_role_e::console,
    endpoint,
    format,
  };

  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request(message, encoded));
  EXPECT_EQ(
    encoded.size(),
    sizeof(request_header_t) + endpoint.size() * sizeof(wchar_t) + sizeof(format));

  request_message_t decoded {};
  ASSERT_TRUE(decode_request(encoded, decoded));
  ASSERT_TRUE(decoded.device_format.has_value());
  EXPECT_EQ(decoded.operation, operation_e::set_device_format);
  EXPECT_EQ(decoded.endpoint_id, endpoint);
  EXPECT_EQ(decoded.device_format->format_tag, format.format_tag);
  EXPECT_EQ(decoded.device_format->channels, format.channels);
  EXPECT_EQ(decoded.device_format->samples_per_sec, format.samples_per_sec);
  EXPECT_EQ(decoded.device_format->avg_bytes_per_sec, format.avg_bytes_per_sec);
  EXPECT_EQ(decoded.device_format->block_align, format.block_align);
  EXPECT_EQ(decoded.device_format->bits_per_sample, format.bits_per_sample);
  EXPECT_EQ(decoded.device_format->cb_size, format.cb_size);
  EXPECT_EQ(decoded.device_format->valid_bits_per_sample, format.valid_bits_per_sample);
  EXPECT_EQ(decoded.device_format->channel_mask, format.channel_mask);
  EXPECT_EQ(decoded.device_format->sub_format, format.sub_format);
}

TEST(AudioPolicyProtocol, RejectsInconsistentSetDeviceFormats) {
  using namespace platf::audio_policy::protocol;
  std::vector<std::uint8_t> encoded;
  EXPECT_FALSE(encode_request(
    {operation_e::set_device_format, render_role_e::console, L"{endpoint}"},
    encoded));
  EXPECT_FALSE(encode_request(
    {operation_e::read, render_role_e::console, L"", valid_stereo_format()},
    encoded));

  const auto invalid = [format = valid_stereo_format()](const auto &mutate) {
    auto hostile = format;
    mutate(hostile);
    std::vector<std::uint8_t> encoded;
    EXPECT_FALSE(encode_request(
      {operation_e::set_device_format, render_role_e::console, L"{endpoint}", hostile},
      encoded));
  };

  invalid([](auto &format) { format.format_tag = WAVE_FORMAT_PCM; });
  invalid([](auto &format) { format.cb_size = 0u; });
  invalid([](auto &format) { format.samples_per_sec = 44'100u; });
  invalid([](auto &format) { format.channels = 6u; });
  invalid([](auto &format) { format.bits_per_sample = 24u; });
  invalid([](auto &format) {
    format.bits_per_sample = 16u;
    format.valid_bits_per_sample = 32u;
    format.block_align = 4u;
    format.avg_bytes_per_sec = 192'000u;
  });
  invalid([](auto &format) { format.block_align = 4u; });
  invalid([](auto &format) { format.avg_bytes_per_sec = 192'000u; });
  invalid([](auto &format) { format.channel_mask = SPEAKER_FRONT_LEFT; });
  invalid([](auto &format) {
    format.sub_format = {
      0x03u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x10u, 0x00u,
      0x80u, 0x00u, 0x00u, 0xAAu,
      0x00u, 0x38u, 0x9Bu, 0x71u,
    };
  });
}

TEST(AudioPolicyProtocol, MaximumEndpointAndFormatFrameRemainsBounded) {
  using namespace platf::audio_policy::protocol;
  const auto endpoint = std::wstring(kMaxEndpointIdCodeUnits, L'x');
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request({
    operation_e::set_device_format,
    render_role_e::console,
    endpoint,
    valid_stereo_format(),
  }, encoded));
  EXPECT_EQ(
    encoded.size(),
    sizeof(request_header_t) + kMaxEndpointIdBytes + sizeof(wave_format_extensible_t));
}

TEST(AudioPolicyProtocol, MaximumCompareAndSetFrameRemainsBounded) {
  using namespace platf::audio_policy::protocol;
  request_message_t request {
    operation_e::set_and_readback,
    render_role_e::console,
    std::wstring(kMaxEndpointIdCodeUnits, L'x'),
  };
  request.expected_current_id = std::wstring(kMaxEndpointIdCodeUnits, L'y');
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request(request, encoded));
  EXPECT_EQ(
    encoded.size(),
    sizeof(request_header_t) + kMaxEndpointIdBytes + kMaxEndpointIdBytes);
}

TEST(AudioPolicyProtocol, RejectsHostileWireFormatPayload) {
  using namespace platf::audio_policy::protocol;
  std::vector<std::uint8_t> encoded;
  ASSERT_TRUE(encode_request({
    operation_e::set_device_format,
    render_role_e::console,
    L"{endpoint}",
    valid_stereo_format(),
  }, encoded));

  const auto format_offset = sizeof(request_header_t) +
    std::wstring {L"{endpoint}"}.size() * sizeof(wchar_t);
  wave_format_extensible_t hostile {};
  std::memcpy(
    &hostile,
    encoded.data() + format_offset,
    sizeof(hostile));
  hostile.avg_bytes_per_sec = 1u;
  std::memcpy(
    encoded.data() + format_offset,
    &hostile,
    sizeof(hostile));

  request_message_t decoded {};
  EXPECT_FALSE(decode_request(encoded, decoded));
}

TEST(AudioPolicyProtocol, AcceptsEverySupportedLayoutAndContainer) {
  using namespace platf::audio_policy::protocol;
  auto stereo16 = valid_stereo_format();
  stereo16.bits_per_sample = 16u;
  stereo16.valid_bits_per_sample = 16u;
  stereo16.block_align = 4u;
  stereo16.avg_bytes_per_sec = 192'000u;
  EXPECT_TRUE(encodes_valid_format(stereo16));

  auto stereo32_float = valid_stereo_format();
  stereo32_float.valid_bits_per_sample = 32u;
  stereo32_float.sub_format = {
    0x03u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x10u, 0x00u,
    0x80u, 0x00u, 0x00u, 0xAAu,
    0x00u, 0x38u, 0x9Bu, 0x71u,
  };
  EXPECT_TRUE(encodes_valid_format(stereo32_float));

  auto surround51_back = valid_stereo_format();
  surround51_back.channels = 6u;
  surround51_back.bits_per_sample = 24u;
  surround51_back.valid_bits_per_sample = 24u;
  surround51_back.block_align = 18u;
  surround51_back.avg_bytes_per_sec = 864'000u;
  surround51_back.channel_mask =
    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
    SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
  EXPECT_TRUE(encodes_valid_format(surround51_back));

  auto surround51_side = surround51_back;
  surround51_side.channel_mask =
    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
    SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
    SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  EXPECT_TRUE(encodes_valid_format(surround51_side));

  auto surround71 = valid_stereo_format();
  surround71.channels = 8u;
  surround71.bits_per_sample = 32u;
  surround71.valid_bits_per_sample = 32u;
  surround71.block_align = 32u;
  surround71.avg_bytes_per_sec = 1'536'000u;
  surround71.channel_mask =
    SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
    SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
    SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
    SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  EXPECT_TRUE(encodes_valid_format(surround71));
}

TEST(AudioPolicyProcess, ReadsOneExactResponseThroughTheHelperBoundary) {
  const auto result = invoke_test_helper(request(platf::audio_policy::operation_e::read));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
  EXPECT_EQ(result.readback_id, L"helper-readback");
  EXPECT_TRUE(result.process_reaped);
  EXPECT_FALSE(result.kill_attempted);
}

TEST(AudioPolicyProcess, SetAndReadbackCarriesExactOpaqueUtf16Endpoint) {
  const std::wstring endpoint = L"{opaque-\u03bb-endpoint}";
  const auto result = invoke_test_helper(
    request(
      platf::audio_policy::operation_e::set_and_readback,
      endpoint,
      std::chrono::milliseconds {500},
      {},
      L"{expected-current}",
      platf::audio_policy::render_role_e::console));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
  EXPECT_EQ(result.readback_id, endpoint);
  EXPECT_EQ(result.set_hresult, S_OK);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::completed);
}

TEST(AudioPolicyProcess, CompareAndSetReturnsPreconditionMismatchWithoutSet) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::set_and_readback,
    L"precondition-mismatch",
    std::chrono::milliseconds {500},
    {},
    L"{last-known-current}",
    platf::audio_policy::render_role_e::console));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::precondition);
  EXPECT_EQ(result.readback_id, L"{new-current-after-precondition-change}");
  EXPECT_EQ(
    result.set_hresult,
    platf::audio_policy::kPreconditionMismatchHresult);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::completed);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, SetsExactDeviceFormatWithoutReadback) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::set_device_format,
    L"{opaque-format-endpoint}",
    std::chrono::milliseconds {500},
    valid_stereo_format()));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
  EXPECT_EQ(result.format_hresult, S_OK);
  EXPECT_EQ(result.process_exit_code, 0u);
  EXPECT_EQ(result.win32_error, ERROR_SUCCESS);
  EXPECT_TRUE(result.readback_id.empty());
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, AcceptsMaximumSetDeviceFormatFrameThroughPipes) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::set_device_format,
    std::wstring(platf::audio_policy::protocol::kMaxEndpointIdCodeUnits, L'x'),
    std::chrono::milliseconds {500},
    valid_stereo_format()));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
  EXPECT_EQ(result.format_hresult, S_OK);
  EXPECT_TRUE(result.readback_id.empty());
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsSetDeviceFormatFailureAndExactHresult) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::set_device_format,
    L"format-error",
    std::chrono::milliseconds {500},
    valid_stereo_format()));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::format);
  EXPECT_EQ(result.format_hresult, E_FAIL);
  EXPECT_TRUE(result.readback_id.empty());
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsMalformedShortResponseFailClosed) {
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::set_and_readback, L"short"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::protocol);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsMalformedExtraResponseFailClosed) {
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::set_and_readback, L"extra"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::protocol);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, KeepsStderrOutOfTheBinaryResponseChannel) {
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::set_and_readback, L"stderr"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
  EXPECT_EQ(result.readback_id, L"stderr");
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsHelperComStyleFailureSeparately) {
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::set_and_readback, L"com-error"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::com);
  EXPECT_EQ(result.com_hresult, E_FAIL);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsPreReadFailureAsDefinitelyUnissued) {
  const auto result = invoke_test_helper(
    request(
      platf::audio_policy::operation_e::set_and_readback,
      L"pre-read-failure"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::pre_read);
  EXPECT_EQ(result.com_hresult, E_FAIL);
  EXPECT_EQ(result.set_hresult, S_OK);
  EXPECT_EQ(result.read_hresult, E_FAIL);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::completed);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsPostSetReadbackFailureAsWriteHazard) {
  const auto result = invoke_test_helper(
    request(
      platf::audio_policy::operation_e::set_and_readback,
      L"post-set-read-failure"));

  EXPECT_EQ(
    result.stage,
    platf::audio_policy::failure_stage_e::post_set_readback);
  EXPECT_EQ(result.com_hresult, S_OK);
  EXPECT_EQ(result.set_hresult, S_OK);
  EXPECT_EQ(result.read_hresult, E_FAIL);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::completed);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsReadEnumeratorComFailureWithExactPhaseHresult) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::read,
    {},
    std::chrono::milliseconds {500},
    {},
    {},
    platf::audio_policy::render_role_e::multimedia));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::com);
  EXPECT_EQ(result.com_hresult, E_FAIL);
  EXPECT_EQ(result.read_hresult, E_FAIL);
  EXPECT_TRUE(result.readback_id.empty());
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReportsPostSetReadbackComFailureWithExactPhaseHresult) {
  const auto result = invoke_test_helper(
    request(
      platf::audio_policy::operation_e::set_and_readback,
      L"post-read-com-error"));

  EXPECT_EQ(
    result.stage,
    platf::audio_policy::failure_stage_e::post_set_readback);
  EXPECT_EQ(result.com_hresult, S_OK);
  EXPECT_EQ(result.set_hresult, S_OK);
  EXPECT_EQ(result.read_hresult, E_FAIL);
  EXPECT_TRUE(result.readback_id.empty());
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsNonzeroHelperExitEvenWithAWellFormedResponse) {
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::set_and_readback, L"nonzero"));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::protocol);
  EXPECT_EQ(result.process_exit_code, 9u);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, TimeoutTerminatesAndReapsBlockingHelper) {
  const auto started = std::chrono::steady_clock::now();
  const auto result = invoke_test_helper(
    request(
      platf::audio_policy::operation_e::set_and_readback,
      L"block",
      std::chrono::milliseconds {100}));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::timeout);
  EXPECT_TRUE(result.kill_attempted);
  EXPECT_TRUE(result.process_reaped);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::child_resumed_may_have_executed);
  EXPECT_LT(elapsed, std::chrono::seconds {3});
}

TEST(AudioPolicyProcess, StopTokenCancelsAndReapsBlockingHelper) {
  std::stop_source stop_source;
  auto operation = request(
    platf::audio_policy::operation_e::set_and_readback,
    L"block",
    std::chrono::seconds {10});
  operation.stop_token = stop_source.get_token();

  std::jthread canceller([&stop_source](std::stop_token) {
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    stop_source.request_stop();
  });
  const auto result = invoke_test_helper(operation);

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::cancelled);
  EXPECT_TRUE(result.kill_attempted);
  EXPECT_TRUE(result.process_reaped);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::child_resumed_may_have_executed);
}

TEST(AudioPolicyProcess, ReadTimeoutRemainsAmbiguousAfterResume) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::read,
    {},
    std::chrono::milliseconds {100},
    {},
    {},
    platf::audio_policy::render_role_e::communications));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::timeout);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::child_resumed_may_have_executed);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, ReadStopTokenRemainsAmbiguousAfterResume) {
  std::stop_source stop_source;
  auto operation = request(
    platf::audio_policy::operation_e::read,
    {},
    std::chrono::seconds {10},
    {},
    {},
    platf::audio_policy::render_role_e::communications);
  operation.stop_token = stop_source.get_token();

  std::jthread canceller([&stop_source](std::stop_token) {
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    stop_source.request_stop();
  });
  const auto result = invoke_test_helper(operation);

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::cancelled);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::child_resumed_may_have_executed);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, CancellationBeforeResumeProvesNotStarted) {
  std::stop_source stop_source;
  stop_source.request_stop();
  auto operation = request(platf::audio_policy::operation_e::read);
  operation.stop_token = stop_source.get_token();
  const auto result = invoke_test_helper(operation);

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::cancelled);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.kill_attempted);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, CancellationAtResumeBoundaryProvesNotStarted) {
  std::stop_source stop_source;
  auto operation = request(platf::audio_policy::operation_e::read);
  operation.stop_token = stop_source.get_token();
  const auto result = invoke_test_helper(operation, [&stop_source] {
    stop_source.request_stop();
  });

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::cancelled);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_TRUE(result.process_reaped);
  EXPECT_TRUE(result.kill_attempted);
}

TEST(AudioPolicyProcess, SetDeviceFormatTimeoutTerminatesAndReapsBlockingHelper) {
  const auto result = invoke_test_helper(request(
    platf::audio_policy::operation_e::set_device_format,
    L"block",
    std::chrono::milliseconds {100},
    valid_stereo_format()));

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::timeout);
  EXPECT_TRUE(result.kill_attempted);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, SetDeviceFormatStopTokenCancelsAndReapsBlockingHelper) {
  std::stop_source stop_source;
  auto operation = request(
    platf::audio_policy::operation_e::set_device_format,
    L"block",
    std::chrono::seconds {10},
    valid_stereo_format());
  operation.stop_token = stop_source.get_token();

  std::jthread canceller([&stop_source](std::stop_token) {
    std::this_thread::sleep_for(std::chrono::milliseconds {50});
    stop_source.request_stop();
  });
  const auto result = invoke_test_helper(operation);

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::cancelled);
  EXPECT_TRUE(result.kill_attempted);
  EXPECT_TRUE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsRelativeAndReparseHelperPaths) {
  auto operation = request(platf::audio_policy::operation_e::read);
  const auto relative_result = platf::audio_policy::invoke_with_helper_path(
    operation,
    std::filesystem::path {L"sunshine_audio_policy_helper.exe"});
  EXPECT_EQ(relative_result.stage, platf::audio_policy::failure_stage_e::launch);

  const auto module_path = test_helper_path();
  const auto link_directory = module_path.parent_path() /
    (L"audio-policy-helper-link-" + std::to_wstring(GetCurrentProcessId()));
  const auto reparse_path = link_directory / module_path.filename();
  if (GetFileAttributesW(link_directory.c_str()) != INVALID_FILE_ATTRIBUTES) {
    GTEST_SKIP() << "temporary reparse test path already exists";
  }

  const auto link_created = CreateSymbolicLinkW(
    link_directory.c_str(),
    module_path.parent_path().c_str(),
    SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  if (link_created == 0) {
    GTEST_SKIP() << "symbolic-link privilege unavailable";
  }
  const auto reparse_result = platf::audio_policy::invoke_with_helper_path(operation, reparse_path);
  EXPECT_EQ(reparse_result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    reparse_result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  RemoveDirectoryW(link_directory.c_str());
}

TEST(AudioPolicyProcess, RejectsCanonicalNetworkHelperPathBeforeLaunch) {
  const auto result = platf::audio_policy::invoke_with_helper_path(
    request(platf::audio_policy::operation_e::read),
    std::filesystem::path {L"\\\\server\\share\\sunshine_audio_policy_helper.exe"});

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
}

TEST(AudioPolicyProcess, RejectsNonFixedDriveHelperPathBeforeLaunch) {
  const auto result = platf::audio_policy::invoke_with_helper_path(
    request(platf::audio_policy::operation_e::read),
    std::filesystem::path {L"A:\\sunshine_audio_policy_helper.exe"});

  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
}

TEST(AudioPolicyPackaging, CliOperationPolicyRejectsMixedAndDuplicateTargets) {
  const auto is_operation = [](const std::string &token) {
    static const std::array<const char *, 8> operations {
      "/i", "/package", "/a", "/x", "/uninstall", "/f", "/update", "/p",
    };
    return std::any_of(operations.begin(), operations.end(), [&](const auto operation) {
      return _stricmp(token.c_str(), operation) == 0;
    });
  };
  const auto has_suffix = [](const std::string &value, const char *suffix) {
    const auto suffix_length = std::strlen(suffix);
    return value.size() >= suffix_length &&
      _stricmp(value.c_str() + value.size() - suffix_length, suffix) == 0;
  };
  const auto is_extra_target = [&](const std::string &token) {
    return (token.size() == 38 && token.front() == '{' && token.back() == '}') ||
      has_suffix(token, ".msi") || has_suffix(token, ".msp") || has_suffix(token, ".mst");
  };
  const auto rejected = [&](const std::vector<std::string> &arguments) {
    std::vector<std::size_t> operation_indices;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (is_operation(arguments[index])) {
        operation_indices.push_back(index);
      }
    }
    if (operation_indices.size() != 1) {
      return true;
    }
    const auto operation_index = operation_indices.front();
    const auto target_index = operation_index + 1;
    if (target_index >= arguments.size()) {
      return true;
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      if (index != operation_index && index != target_index &&
          (is_operation(arguments[index]) || is_extra_target(arguments[index]))) {
        return true;
      }
    }
    return false;
  };

  constexpr auto trusted = "{11111111-1111-1111-1111-111111111111}";
  constexpr auto other = "{22222222-2222-2222-2222-222222222222}";
  EXPECT_TRUE(rejected({"/x", trusted, "/i", R"(C:\attacker.msi)"}));
  EXPECT_TRUE(rejected({"/f", trusted, "/package", R"(C:\attacker.msi)"}));
  EXPECT_TRUE(rejected({"/x", trusted, other}));
  EXPECT_TRUE(rejected({"/f", trusted, R"(C:\attacker.msi)"}));
  EXPECT_TRUE(rejected({"/x", trusted, R"(TRANSFORMS=C:\attacker.mst)"}));
}

TEST(AudioPolicyPackaging, UsesNativeMsiTrustBoundaryInsteadOfExecutablePowerShell) {
#if defined(SUNSHINE_SOURCE_DIR)
  const auto source_root = std::filesystem::path {SUNSHINE_SOURCE_DIR};
  const auto actions_path = source_root / L"packaging/windows/wix/custom_actions.wxs";
  std::ifstream actions {actions_path, std::ios::binary};
  ASSERT_TRUE(actions.is_open()) << actions_path.string();
  const std::string xml {
    std::istreambuf_iterator<char> {actions},
    std::istreambuf_iterator<char> {},
  };

  EXPECT_NE(xml.find("AudioPolicyInstallTreeSecurityCA"), std::string::npos);
  EXPECT_NE(xml.find("PrepareAudioPolicyInstallTreeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("PrepareAudioPolicyUpgradeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("RollbackAudioPolicyInstallTreeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("ProtectAudioPolicyInstallTreeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("CommitAudioPolicyInstallTreeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("VerifyAudioPolicyUninstallTreeSecurity"), std::string::npos);
  EXPECT_NE(xml.find("Execute=\"deferred\""), std::string::npos);
  EXPECT_NE(xml.find("Execute=\"rollback\""), std::string::npos);
  EXPECT_NE(xml.find("Execute=\"commit\""), std::string::npos);
  EXPECT_NE(xml.find("Impersonate=\"no\""), std::string::npos);
  EXPECT_NE(xml.find("HideTarget=\"yes\""), std::string::npos);
  EXPECT_EQ(xml.find("SetValidateLocalInstallRoot"), std::string::npos);
  EXPECT_EQ(xml.find("ValidateLocalInstallRoot"), std::string::npos);
  EXPECT_EQ(xml.find("SetProtectAudioPolicyHelper"), std::string::npos);
  EXPECT_EQ(xml.find("ProtectAudioPolicyHelper"), std::string::npos);
  EXPECT_EQ(xml.find("SetAccessRuleProtection($true,$false)"), std::string::npos);
  EXPECT_EQ(xml.find("Get-ChildItem"), std::string::npos);
  EXPECT_EQ(xml.find("Id=\"ResetAcls\""), std::string::npos);
  EXPECT_EQ(xml.find("RemoveConflictingProductsVbs"), std::string::npos);
  EXPECT_EQ(xml.find("Id=\"RemoveConflictingProducts\""), std::string::npos);
  EXPECT_EQ(xml.find("AskUninstallLegacySunshineVbs"), std::string::npos);
  EXPECT_EQ(xml.find("WaitForLegacyUninstallVbs"), std::string::npos);
  EXPECT_EQ(xml.find("Id=\"AskUninstallLegacySunshine\""), std::string::npos);
  EXPECT_EQ(xml.find("Id=\"WaitForLegacyUninstall\""), std::string::npos);

  const auto sequence_path =
    source_root / L"packaging/windows/wix/patch_custom_actions.wxs";
  std::ifstream sequence_source {sequence_path, std::ios::binary};
  ASSERT_TRUE(sequence_source.is_open()) << sequence_path.string();
  const std::string sequence_text {
    std::istreambuf_iterator<char> {sequence_source},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(
    sequence_text.find("Installed OR REMOVE OR NOT LEGACY_SUNSHINE_PRESENT"),
    std::string::npos);
  EXPECT_NE(sequence_text.find("AudioPolicyProtectRequired"), std::string::npos);
  EXPECT_NE(sequence_text.find("AudioPolicyFullRemove"), std::string::npos);
  EXPECT_EQ(
    sequence_text.find("NOT REMOVE AND AudioPolicyTrustedInstallRoot"),
    std::string::npos);
  EXPECT_EQ(
    sequence_text.find("REMOVE=\"ALL\" AND NOT UPGRADINGPRODUCTCODE AND AudioPolicyTrustedInstallRoot"),
    std::string::npos);
  EXPECT_EQ(
    sequence_text.find("SetKillProcsQuietImmediate\" Before=\"InstallValidate"),
    std::string::npos);
  EXPECT_EQ(
    sequence_text.find("((Installed OR WIX_UPGRADE_DETECTED) AND NOT REMOVE"),
    std::string::npos);
  EXPECT_NE(
    sequence_text.find("SetKillProcsQuietImmediate\" Before=\"StopServices"),
    std::string::npos);
  EXPECT_EQ(sequence_text.find("AskUninstallLegacySunshine"), std::string::npos);
  EXPECT_EQ(sequence_text.find("WaitForLegacyUninstall"), std::string::npos);

  const auto bootstrapper_path =
    source_root / L"packaging/windows/bootstrapper/VibeshineInstaller.cs";
  std::ifstream bootstrapper {bootstrapper_path, std::ios::binary};
  ASSERT_TRUE(bootstrapper.is_open()) << bootstrapper_path.string();
  const std::string bootstrapper_source {
    std::istreambuf_iterator<char> {bootstrapper},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(bootstrapper_source.find("NormalizeInstallPath"), std::string::npos);
  EXPECT_NE(bootstrapper_source.find("RegistryView.Registry64"), std::string::npos);
  EXPECT_NE(bootstrapper_source.find("ProgramFilesDir"), std::string::npos);
  EXPECT_NE(bootstrapper_source.find("ReparsePoint"), std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("ManualLegacyRemovalRequired"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("MsiPackageContainsNativeTrustBoundary"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("OpenPinnedMsiPayload"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("GetTrustedInstallerCacheRoot"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("ComputeMsiBinaryStreamSha256Hex"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("The bootstrapper Authenticode signature is missing or invalid"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("HasMatchingReleasedSigner"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("RejectUntrustedCliPayloadRequest"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("FileShare.Read"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("VibepolloInstallerCache_"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("OpenPinnedMsiForExecution"),
    std::string::npos);
  const auto cli_maintenance_pin = bootstrapper_source.find(
    "OpenPinnedRegisteredMaintenancePayload(arguments.ForwardedArguments)");
  const auto cli_core_call = bootstrapper_source.find(
    "RunCliWithPinnedMaintenancePayload(arguments, pinnedMaintenancePayload)");
  const auto cli_cache_sweep = bootstrapper_source.find(
    "SweepStaleInstallerRecoveryDirectories();",
    cli_core_call);
  ASSERT_NE(cli_maintenance_pin, std::string::npos);
  ASSERT_NE(cli_core_call, std::string::npos);
  ASSERT_NE(cli_cache_sweep, std::string::npos);
  EXPECT_LT(cli_maintenance_pin, cli_core_call);
  EXPECT_LT(cli_core_call, cli_cache_sweep);
  EXPECT_NE(
    bootstrapper_source.find(
      "RunMsiexec(cliArgs, arguments.IsCliQuietMode(), true, pinnedMaintenancePayload)"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find(
      "RunMsiexec(args, hiddenWindow, requestElevationIfNeeded, releasedPayload)"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("TryGetSoleMsiOperationTarget"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("operationCount != 1"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("LooksLikeAdditionalMsiTarget(token)"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find(
      "Exactly one MSI operation and one corresponding target are required"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find(
      "A second MSI operation, ProductCode, or package target is not accepted"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("TryValidatePinnedMsiExecutionTarget"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("pinnedPayload.ProductCode"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("pinnedPayload.Path"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("MSI patches and transforms are not accepted"),
    std::string::npos);
  EXPECT_EQ(
    bootstrapper_source.find("CommonApplicationData"),
    std::string::npos);
  EXPECT_EQ(bootstrapper_source.find("FindSidecarMsi"), std::string::npos);
  EXPECT_EQ(
    bootstrapper_source.find("Sidecar remains a fallback"),
    std::string::npos);
  EXPECT_EQ(
    bootstrapper_source.find("Use a specific MSI payload instead of the embedded one"),
    std::string::npos);
  EXPECT_NE(
    bootstrapper_source.find("product.IsPerUser || !product.IsWindowsInstaller"),
    std::string::npos);
  EXPECT_EQ(bootstrapper_source.find("ProgramW6432"), std::string::npos);
  EXPECT_EQ(bootstrapper_source.find("D:\\\\Vibepollo"), std::string::npos);

  const auto windows_ci_path = source_root / L".github/workflows/ci-windows.yml";
  std::ifstream windows_ci {windows_ci_path, std::ios::binary};
  ASSERT_TRUE(windows_ci.is_open()) << windows_ci_path.string();
  const std::string windows_ci_source {
    std::istreambuf_iterator<char> {windows_ci},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(
    windows_ci_source.find("GetManifestResourceStream('Payload.msi')"),
    std::string::npos);
  EXPECT_NE(
    windows_ci_source.find("embedded MSI digest differs from the signed MSI selected for this release"),
    std::string::npos);
  EXPECT_NE(
    windows_ci_source.find("outer setup and embedded MSI signer thumbprints differ"),
    std::string::npos);
  EXPECT_NE(
    windows_ci_source.find("Certificate rollover is intentionally fail-closed"),
    std::string::npos);

  const auto native_ca_path =
    source_root / L"packaging/windows/wix/audio_policy_security_custom_action.cpp";
  std::ifstream native_ca {native_ca_path, std::ios::binary};
  ASSERT_TRUE(native_ca.is_open()) << native_ca_path.string();
  const std::string native_ca_source {
    std::istreambuf_iterator<char> {native_ca},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(native_ca_source.find("MsiGetTargetPathW"), std::string::npos);
  EXPECT_NE(native_ca_source.find("MsiGetComponentPathW"), std::string::npos);
  EXPECT_NE(native_ca_source.find("MsiGetFeatureStateW"), std::string::npos);
  EXPECT_NE(native_ca_source.find("MsiGetComponentStateW"), std::string::npos);
  EXPECT_NE(native_ca_source.find("AudioPolicyProtectRequired"), std::string::npos);
  EXPECT_NE(native_ca_source.find("AudioPolicyFullRemove"), std::string::npos);
  EXPECT_NE(native_ca_source.find("FOLDERID_ProgramFilesX64"), std::string::npos);
  EXPECT_NE(native_ca_source.find("GetFinalPathNameByHandleW"), std::string::npos);
  EXPECT_NE(native_ca_source.find("FILE_PERSISTENT_ACLS"), std::string::npos);
  EXPECT_EQ(native_ca_source.find("MsiSetTargetPathW"), std::string::npos);
  EXPECT_EQ(native_ca_source.find("powershell"), std::string::npos);

  const auto process_path =
    source_root / L"src/platform/windows/audio_policy_process.cpp";
  std::ifstream process_source {process_path, std::ios::binary};
  ASSERT_TRUE(process_source.is_open()) << process_path.string();
  const std::string process_text {
    std::istreambuf_iterator<char> {process_source},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(process_text.find("FOLDERID_ProgramFiles"), std::string::npos);
  EXPECT_NE(process_text.find("Program Files subtree"), std::string::npos);
#else
  GTEST_SKIP() << "source root is not defined";
#endif
}

TEST(AudioPolicyPackaging, ActualMsiContainsNativeTransactionalTrustBoundary) {
  const auto msi_path = test_msi_path();
  if (msi_path.empty() || !std::filesystem::exists(msi_path)) {
    GTEST_SKIP() << "package_msi artifact is not present: " << msi_path.string();
  }

  msi_handle_t database;
  ASSERT_EQ(
    MsiOpenDatabaseW(
      msi_path.c_str(),
      nullptr,
      &database.value),
    ERROR_SUCCESS);

  bool duplicate = false;
  const auto cost_finalize = read_msi_action_row(
    database.value,
    L"CostFinalize",
    duplicate);
  ASSERT_TRUE(cost_finalize.has_value());
  EXPECT_FALSE(duplicate);
  const auto install_initialize = read_msi_action_row(
    database.value,
    L"InstallInitialize",
    duplicate);
  ASSERT_TRUE(install_initialize.has_value());
  EXPECT_FALSE(duplicate);
  const auto install_files = read_msi_action_row(
    database.value,
    L"InstallFiles",
    duplicate);
  ASSERT_TRUE(install_files.has_value());
  EXPECT_FALSE(duplicate);
  const auto remove_existing_products = read_msi_action_row(
    database.value,
    L"RemoveExistingProducts",
    duplicate);
  ASSERT_TRUE(remove_existing_products.has_value());
  EXPECT_FALSE(duplicate);
  const auto remove_files = read_msi_action_row(database.value, L"RemoveFiles", duplicate);
  ASSERT_TRUE(remove_files.has_value());
  EXPECT_FALSE(duplicate);
  const auto start_services = read_msi_action_row(
    database.value,
    L"StartServices",
    duplicate);
  ASSERT_TRUE(start_services.has_value());
  EXPECT_FALSE(duplicate);

  const auto prepare = read_msi_action_row(
    database.value,
    L"PrepareAudioPolicyInstallTreeSecurity",
    duplicate);
  ASSERT_TRUE(prepare.has_value());
  EXPECT_FALSE(duplicate);
  const auto prepare_upgrade = read_msi_action_row(
    database.value,
    L"PrepareAudioPolicyUpgradeSecurity",
    duplicate);
  ASSERT_TRUE(prepare_upgrade.has_value());
  EXPECT_FALSE(duplicate);
  const auto rollback = read_msi_action_row(
    database.value,
    L"RollbackAudioPolicyInstallTreeSecurity",
    duplicate);
  ASSERT_TRUE(rollback.has_value());
  EXPECT_FALSE(duplicate);
  const auto protect = read_msi_action_row(
    database.value,
    L"ProtectAudioPolicyInstallTreeSecurity",
    duplicate);
  ASSERT_TRUE(protect.has_value());
  EXPECT_FALSE(duplicate);
  const auto commit = read_msi_action_row(
    database.value,
    L"CommitAudioPolicyInstallTreeSecurity",
    duplicate);
  ASSERT_TRUE(commit.has_value());
  EXPECT_FALSE(duplicate);
  const auto verify_uninstall = read_msi_action_row(
    database.value,
    L"VerifyAudioPolicyUninstallTreeSecurity",
    duplicate);
  ASSERT_TRUE(verify_uninstall.has_value());
  EXPECT_FALSE(duplicate);
  const auto restore_nv = read_msi_action_row(
    database.value,
    L"RestoreNvPrefsUndo",
    duplicate);
  ASSERT_TRUE(restore_nv.has_value());
  EXPECT_FALSE(duplicate);
  const auto install_sudovda = read_msi_action_row(
    database.value,
    L"InstallSudovda",
    duplicate);
  ASSERT_TRUE(install_sudovda.has_value());
  EXPECT_FALSE(duplicate);
  const auto quiet_kill = read_msi_action_row(
    database.value,
    L"SetKillProcsQuietImmediate",
    duplicate);
  ASSERT_TRUE(quiet_kill.has_value());
  EXPECT_FALSE(duplicate);

  EXPECT_NE(prepare->condition.find(L"NOT WIX_UPGRADE_DETECTED"), std::wstring::npos);
  EXPECT_NE(prepare_upgrade->condition.find(L"WIX_UPGRADE_DETECTED"), std::wstring::npos);
  EXPECT_NE(rollback->condition.find(L"AudioPolicyTrustedInstallRoot"), std::wstring::npos);
  EXPECT_NE(rollback->condition.find(L"AudioPolicyProtectRequired"), std::wstring::npos);
  EXPECT_NE(protect->condition.find(L"AudioPolicyTrustedInstallRoot"), std::wstring::npos);
  EXPECT_NE(protect->condition.find(L"AudioPolicyProtectRequired"), std::wstring::npos);
  EXPECT_NE(commit->condition.find(L"AudioPolicyTrustedInstallRoot"), std::wstring::npos);
  EXPECT_NE(commit->condition.find(L"AudioPolicyProtectRequired"), std::wstring::npos);
  EXPECT_NE(verify_uninstall->condition.find(L"AudioPolicyTrustedInstallRoot"), std::wstring::npos);
  EXPECT_NE(verify_uninstall->condition.find(L"AudioPolicyFullRemove"), std::wstring::npos);
  EXPECT_EQ(rollback->condition.find(L"REMOVE"), std::wstring::npos);
  EXPECT_EQ(protect->condition.find(L"REMOVE"), std::wstring::npos);
  EXPECT_EQ(commit->condition.find(L"REMOVE"), std::wstring::npos);
  EXPECT_EQ(verify_uninstall->condition.find(L"REMOVE ="), std::wstring::npos);
  EXPECT_NE(quiet_kill->condition.find(L"AudioPolicyProtectRequired"), std::wstring::npos);
  EXPECT_NE(quiet_kill->condition.find(L"AudioPolicyFullRemove"), std::wstring::npos);
  EXPECT_NE(quiet_kill->condition.find(L"AudioPolicyTrustedInstallRoot"), std::wstring::npos);
  EXPECT_EQ(quiet_kill->condition.find(L"NOT REMOVE"), std::wstring::npos);
  EXPECT_EQ(quiet_kill->condition.find(L"REMOVE ="), std::wstring::npos);
  EXPECT_GT(prepare->sequence, cost_finalize->sequence);
  EXPECT_GT(prepare->sequence, install_initialize->sequence);
  EXPECT_LT(prepare->sequence, install_files->sequence);
  EXPECT_GT(prepare_upgrade->sequence, install_initialize->sequence);
  EXPECT_LT(prepare_upgrade->sequence, remove_existing_products->sequence);
  EXPECT_GT(quiet_kill->sequence, prepare->sequence);
  EXPECT_GT(quiet_kill->sequence, prepare_upgrade->sequence);
  EXPECT_LT(quiet_kill->sequence, start_services->sequence);
  EXPECT_LT(remove_existing_products->sequence, install_files->sequence);
  EXPECT_GT(rollback->sequence, install_files->sequence);
  EXPECT_GT(protect->sequence, rollback->sequence);
  EXPECT_GT(commit->sequence, protect->sequence);
  EXPECT_GT(install_sudovda->sequence, commit->sequence);
  EXPECT_LT(protect->sequence, start_services->sequence);
  EXPECT_LT(verify_uninstall->sequence, restore_nv->sequence);
  EXPECT_LT(restore_nv->sequence, remove_files->sequence);
  for (const auto *installed_payload_action : {
         L"InstallSudovda",
         L"InstallVirtualDisplayDriver",
         L"RegisterVulkanHdrLayer",
         L"MigrateConfig",
         L"RunInstallerMigrations",
         L"UpdatePathAdd"}) {
    const auto row = read_msi_action_row(
      database.value,
      installed_payload_action,
      duplicate);
    ASSERT_TRUE(row.has_value()) << std::string(
      installed_payload_action,
      installed_payload_action + wcslen(installed_payload_action));
    EXPECT_FALSE(duplicate);
    EXPECT_GT(row->sequence, protect->sequence);
    EXPECT_NE(
      row->condition.find(L"AudioPolicyTrustedInstallRoot"),
      std::wstring::npos);
    EXPECT_NE(
      row->condition.find(L"AudioPolicyProtectRequired"),
      std::wstring::npos);
    EXPECT_EQ(row->condition.find(L"NOT REMOVE"), std::wstring::npos);
  }
  for (const auto *uninstall_payload_action : {
         L"RestoreNvPrefsUndo",
         L"UnregisterVulkanHdrLayer",
         L"UninstallSudovda",
         L"UninstallVirtualDisplayDriver",
         L"FactoryResetAppData"}) {
    const auto row = read_msi_action_row(
      database.value,
      uninstall_payload_action,
      duplicate);
    ASSERT_TRUE(row.has_value()) << std::string(
      uninstall_payload_action,
      uninstall_payload_action + wcslen(uninstall_payload_action));
    EXPECT_FALSE(duplicate);
    EXPECT_GT(row->sequence, verify_uninstall->sequence);
    EXPECT_LT(row->sequence, remove_files->sequence);
    EXPECT_NE(
      row->condition.find(L"AudioPolicyTrustedInstallRoot"),
      std::wstring::npos);
    EXPECT_NE(
      row->condition.find(L"AudioPolicyFullRemove"),
      std::wstring::npos);
    EXPECT_EQ(row->condition.find(L"REMOVE ="), std::wstring::npos);
  }
  EXPECT_EQ(read_msi_property_value(
    database.value, L"AudioPolicyProtectRequired"), std::optional<std::wstring> {L"0"});
  EXPECT_EQ(read_msi_property_value(
    database.value, L"AudioPolicyFullRemove"), std::optional<std::wstring> {L"0"});
  const auto ca_hash = read_msi_property_value(
    database.value, L"AudioPolicySecurityCABinarySha256");
  ASSERT_TRUE(ca_hash.has_value());
  EXPECT_EQ(ca_hash->size(), 64u);
  EXPECT_TRUE(std::ranges::all_of(*ca_hash, [](const wchar_t character) {
    return (character >= L'0' && character <= L'9') ||
      (character >= L'a' && character <= L'f');
  }));
  EXPECT_EQ(
    count_msi_file_rows(database.value, L"sunshine_audio_policy_helper.exe"),
    1u);
  EXPECT_EQ(
    count_msi_file_rows(database.value, L"audio_policy_security_custom_action.dll"),
    0u);
  EXPECT_EQ(
    count_msi_binary_rows(database.value, L"AudioPolicyInstallTreeSecurityCA"),
    1u);
  EXPECT_FALSE(msi_has_executable_install_root_powershell(database.value));

  constexpr int type_dll = 0x0001;
  constexpr int type_rollback = 0x0100;
  constexpr int type_commit = 0x0200;
  constexpr int type_in_script = 0x0400;
  constexpr int type_no_impersonate = 0x0800;
  constexpr int type_hide_target = 0x2000;
  const auto prepare_type = read_msi_custom_action_integer(
    database.value, L"PrepareAudioPolicyInstallTreeSecurity", L"Type");
  const auto rollback_type = read_msi_custom_action_integer(
    database.value, L"RollbackAudioPolicyInstallTreeSecurity", L"Type");
  const auto protect_type = read_msi_custom_action_integer(
    database.value, L"ProtectAudioPolicyInstallTreeSecurity", L"Type");
  const auto commit_type = read_msi_custom_action_integer(
    database.value, L"CommitAudioPolicyInstallTreeSecurity", L"Type");
  const auto verify_type = read_msi_custom_action_integer(
    database.value, L"VerifyAudioPolicyUninstallTreeSecurity", L"Type");
  const auto prepare_upgrade_type = read_msi_custom_action_integer(
    database.value, L"PrepareAudioPolicyUpgradeSecurity", L"Type");
  ASSERT_TRUE(prepare_type.has_value());
  ASSERT_TRUE(rollback_type.has_value());
  ASSERT_TRUE(protect_type.has_value());
  ASSERT_TRUE(commit_type.has_value());
  ASSERT_TRUE(verify_type.has_value());
  ASSERT_TRUE(prepare_upgrade_type.has_value());
  constexpr int type_continue = 0x0040;
  EXPECT_EQ(*prepare_type, type_dll);
  EXPECT_EQ(*prepare_upgrade_type, type_dll);
  EXPECT_EQ(
    *rollback_type,
    type_dll | type_continue | type_rollback | type_in_script |
      type_no_impersonate | type_hide_target);
  EXPECT_EQ(
    *protect_type,
    type_dll | type_in_script | type_no_impersonate | type_hide_target);
  EXPECT_EQ(
    *commit_type,
    type_dll | type_continue | type_commit | type_in_script |
      type_no_impersonate | type_hide_target);
  EXPECT_EQ(
    *verify_type,
    type_dll | type_in_script | type_no_impersonate | type_hide_target);

  for (const auto *action : {
         L"PrepareAudioPolicyInstallTreeSecurity",
         L"PrepareAudioPolicyUpgradeSecurity",
         L"RollbackAudioPolicyInstallTreeSecurity",
         L"ProtectAudioPolicyInstallTreeSecurity",
         L"CommitAudioPolicyInstallTreeSecurity",
         L"VerifyAudioPolicyUninstallTreeSecurity"}) {
    const auto source = read_msi_custom_action_string(database.value, action, L"Source");
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(*source, L"AudioPolicyInstallTreeSecurityCA");
    const auto target = read_msi_custom_action_string(database.value, action, L"Target");
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(*target, action);
  }

  for (const auto *removed_action : {
         L"ResetAcls",
         L"SetResetAcls",
         L"SetValidateLocalInstallRoot",
         L"ValidateLocalInstallRoot",
         L"SetProtectAudioPolicyHelper",
         L"ProtectAudioPolicyHelper"}) {
    EXPECT_FALSE(read_msi_custom_action_integer(
      database.value, removed_action, L"Type").has_value());
  }
  EXPECT_FALSE(read_msi_custom_action_integer(
    database.value, L"RemoveConflictingProducts", L"Type").has_value());
  for (const auto *legacy_ui_action : {
         L"AskUninstallLegacySunshine",
         L"WaitForLegacyUninstall",
         L"BlockLegacySunshineStillPresent",
         L"BlockUserCancelledLegacy"}) {
    EXPECT_FALSE(read_msi_custom_action_integer(
      database.value, legacy_ui_action, L"Type").has_value());
  }
  EXPECT_EQ(count_msi_binary_rows(
    database.value, L"AskUninstallLegacySunshineVbs"), 0u);
  EXPECT_EQ(count_msi_binary_rows(
    database.value, L"WaitForLegacyUninstallVbs"), 0u);
  EXPECT_EQ(
    count_msi_launch_condition_rows(
      database.value,
      L"Installed OR REMOVE OR NOT LEGACY_SUNSHINE_PRESENT"),
    1u);
}

TEST(AudioPolicyProcess, RejectsNonSingleHardlinkHelperBeforeResume) {
  const auto directory = temporary_helper_directory(L"hardlink");
  const auto helper = directory / L"sunshine_audio_policy_helper.exe";
  const auto hardlink_directory = directory / L"linked-parent";
  std::filesystem::create_directories(hardlink_directory);
  const auto hardlink = hardlink_directory / helper.filename();
  ASSERT_TRUE(copy_test_helper_to(helper));
  ASSERT_TRUE(CreateHardLinkW(hardlink.c_str(), helper.c_str(), nullptr));

  const auto result = platf::audio_policy::invoke_with_helper_path(
    request(platf::audio_policy::operation_e::read),
    hardlink);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
  std::filesystem::remove_all(directory);
}

TEST(AudioPolicyProcess, RejectsWeakHelperAclBeforeResume) {
  const auto directory = temporary_helper_directory(L"weak-acl");
  const auto helper = directory / L"sunshine_audio_policy_helper.exe";
  ASSERT_TRUE(copy_test_helper_to(helper));
  ASSERT_TRUE(grant_users_write_access(helper));

  const auto result = platf::audio_policy::invoke_with_helper_path(
    request(platf::audio_policy::operation_e::read),
    helper);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
  std::filesystem::remove_all(directory);
}

TEST(AudioPolicyProcess, RejectsParentDeleteChildBeforeCreate) {
  const auto directory = temporary_helper_directory(L"parent-delete-child");
  const auto helper = directory / L"sunshine_audio_policy_helper.exe";
  ASSERT_TRUE(copy_test_helper_to(helper));
  ASSERT_TRUE(grant_users_write_access(directory, FILE_DELETE_CHILD));

  const auto result = platf::audio_policy::invoke_with_helper_path(
    request(platf::audio_policy::operation_e::read),
    helper);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
  std::filesystem::remove_all(directory);
}

TEST(AudioPolicyProcess, RejectsArbitrarySidGenericWriteBeforeCreate) {
  const auto result = invoke_with_acl_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    GENERIC_WRITE,
    false);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsArbitrarySidGenericAllBeforeCreate) {
  const auto result = invoke_with_acl_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    GENERIC_ALL,
    false);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsArbitrarySidObjectAllowAceBeforeCreate) {
  const auto result = invoke_with_acl_ace(
    ACCESS_ALLOWED_OBJECT_ACE_TYPE,
    GENERIC_WRITE,
    false);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsArbitrarySidCallbackAllowAceBeforeCreate) {
  const auto result = invoke_with_acl_ace(
    ACCESS_ALLOWED_CALLBACK_ACE_TYPE,
    GENERIC_WRITE,
    false);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, RejectsProtectedArbitraryGenericAllAceBeforeCreate) {
  const auto result = invoke_with_acl_ace(
    ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE,
    GENERIC_ALL,
    true);
  EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
  EXPECT_EQ(
    result.execution_disposition,
    platf::audio_policy::execution_disposition_e::not_started);
  EXPECT_FALSE(result.process_reaped);
}

TEST(AudioPolicyProcess, PinsHelperAndAncestorsAgainstRenameAndReparseSwap) {
  bool helper_rename_succeeded = false;
  bool parent_rename_succeeded = false;
  bool helper_write_succeeded = false;
  const auto helper = test_helper_path();
  const auto helper_backup = helper.parent_path() /
    L"sunshine_audio_policy_helper.swap-backup.exe";
  const auto parent_backup = helper.parent_path().parent_path() /
    L"audio-policy-tools.swap-backup";
  const auto result = invoke_test_helper(
    request(platf::audio_policy::operation_e::read),
    [&] {
      helper_rename_succeeded = MoveFileExW(
        helper.c_str(),
        helper_backup.c_str(),
        MOVEFILE_REPLACE_EXISTING) != FALSE;
      parent_rename_succeeded = MoveFileExW(
        helper.parent_path().c_str(),
        parent_backup.c_str(),
        MOVEFILE_REPLACE_EXISTING) != FALSE;
      const auto write_handle = CreateFileW(
        helper.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
      helper_write_succeeded = write_handle != INVALID_HANDLE_VALUE;
      if (write_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(write_handle);
      }
    });

  EXPECT_FALSE(helper_write_succeeded);
  if (helper_rename_succeeded || parent_rename_succeeded) {
    // A rename race is allowed to win the filesystem operation, but the
    // suspended child must then fail identity validation and never resume.
    EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::launch);
    EXPECT_EQ(
      result.execution_disposition,
      platf::audio_policy::execution_disposition_e::not_started);
    EXPECT_TRUE(result.process_reaped);
  } else {
    EXPECT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
    EXPECT_EQ(
      result.execution_disposition,
      platf::audio_policy::execution_disposition_e::completed);
  }
  if (parent_rename_succeeded) {
    MoveFileExW(
      parent_backup.c_str(),
      helper.parent_path().c_str(),
      MOVEFILE_REPLACE_EXISTING);
  }
  if (helper_rename_succeeded) {
    MoveFileExW(
      helper_backup.c_str(),
      helper.c_str(),
      MOVEFILE_REPLACE_EXISTING);
  }
  DeleteFileW(helper_backup.c_str());
  RemoveDirectoryW(parent_backup.c_str());
}

TEST(AudioPolicyProcess, RepeatedCallsReapEveryChild) {
  DWORD handles_before = 0u;
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
  for (int index = 0; index < 16; ++index) {
    const auto result = invoke_test_helper(request(platf::audio_policy::operation_e::read));
    ASSERT_EQ(result.stage, platf::audio_policy::failure_stage_e::success);
    ASSERT_TRUE(result.process_reaped);
  }
  DWORD handles_after = 0u;
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &handles_after));
  EXPECT_LE(handles_after, handles_before + 4u);
}
