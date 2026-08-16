/**
 * @file tests/unit/platform/windows/test_audio_policy_msi_security.cpp
 * @brief Native MSI install-tree privilege-boundary tests.
 */

#include "../../../tests_common.h"

#include <Windows.h>
#include <Aclapi.h>
#include <ShlObj.h>
#include <msiquery.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace sunshine::installer_security::testing {
  struct file_identity_t {
    ULONGLONG volume_serial {0u};
    std::array<BYTE, 16u> file_id {};
  };

  struct operation_state_t {
    bool protect_required {false};
    bool full_remove {false};
  };

  std::optional<operation_state_t> classify_operation_state(
    std::wstring_view remove,
    int product_feature_action,
    int extras_feature_action,
    int service_component_action);

  bool normalize_program_files_descendant(
    std::wstring_view program_files,
    std::wstring_view candidate,
    std::wstring &normalized);

  bool security_descriptor_is_safe(
    PSECURITY_DESCRIPTOR descriptor,
    bool is_ancestor);

  bool attributes_are_direct(const FILE_ATTRIBUTE_TAG_INFO &attributes);

  bool tree_contains_reparse_point(const std::filesystem::path &root);

  bool apply_exact_security(
    const std::filesystem::path &path,
    bool directory);

  bool has_exact_security(
    const std::filesystem::path &path,
    bool directory);

  std::optional<file_identity_t> capture_identity(
    const std::filesystem::path &path,
    bool directory);

  bool identity_matches(
    const std::filesystem::path &path,
    bool directory,
    const file_identity_t &expected);

  bool is_single_link_file(const std::filesystem::path &path);

  bool is_exclusively_pinnable(
    const std::filesystem::path &path,
    bool directory);

  std::wstring encode_custom_action_data(
    std::wstring_view root,
    std::wstring_view journal);

  bool decode_custom_action_data(
    std::wstring_view encoded,
    std::wstring &root,
    std::wstring &journal);

  bool harden_tree_with_journal(
    const std::filesystem::path &root,
    const std::filesystem::path &journal,
    std::size_t fail_after_mutations = std::numeric_limits<std::size_t>::max());

  bool restore_tree_from_journal(const std::filesystem::path &journal);

  bool validate_native_program_files_ancestry(
    const std::filesystem::path &candidate);

  bool preflight_existing_tree(const std::filesystem::path &root);

  bool journal_size_is_acceptable(
    std::uint64_t current_bytes,
    std::uint64_t additional_bytes);
}

namespace {
  constexpr auto kTrustedInstallerSid =
    L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

  struct local_sid_t {
    PSID value {nullptr};

    explicit local_sid_t(const wchar_t *text) {
      EXPECT_TRUE(ConvertStringSidToSidW(text, &value));
    }

    ~local_sid_t() {
      if (value != nullptr) {
        LocalFree(value);
      }
    }

    local_sid_t(const local_sid_t &) = delete;
    local_sid_t &operator=(const local_sid_t &) = delete;
  };

  struct descriptor_fixture_t {
    SECURITY_DESCRIPTOR descriptor {};
    std::vector<BYTE> acl_bytes;
    local_sid_t owner {L"S-1-5-32-544"};
    local_sid_t system {L"S-1-5-18"};
    local_sid_t administrators {L"S-1-5-32-544"};
    local_sid_t trusted_installer {kTrustedInstallerSid};
    local_sid_t users {L"S-1-5-32-545"};
    local_sid_t creator_owner {L"S-1-3-0"};

    descriptor_fixture_t():
        acl_bytes(4096u, BYTE {0}) {
      EXPECT_TRUE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
      EXPECT_TRUE(InitializeAcl(acl(), static_cast<DWORD>(acl_bytes.size()), ACL_REVISION_DS));
      EXPECT_TRUE(AddAccessAllowedAceEx(
        acl(), ACL_REVISION_DS, 0u, FILE_ALL_ACCESS, system.value));
      EXPECT_TRUE(AddAccessAllowedAceEx(
        acl(), ACL_REVISION_DS, 0u, FILE_ALL_ACCESS, administrators.value));
      EXPECT_TRUE(AddAccessAllowedAceEx(
        acl(), ACL_REVISION_DS, 0u, FILE_ALL_ACCESS, trusted_installer.value));
      EXPECT_TRUE(AddAccessAllowedAceEx(
        acl(),
        ACL_REVISION_DS,
        0u,
        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE,
        users.value));
      EXPECT_TRUE(SetSecurityDescriptorOwner(&descriptor, owner.value, FALSE));
      EXPECT_TRUE(SetSecurityDescriptorDacl(&descriptor, TRUE, acl(), FALSE));
    }

    PACL acl() {
      return reinterpret_cast<PACL>(acl_bytes.data());
    }

    bool append_allow_ace(
      BYTE type,
      ACCESS_MASK mask,
      PSID sid,
      BYTE flags = 0u,
      DWORD object_flags = 0u) {
      const auto sid_length = GetLengthSid(sid);
      DWORD sid_offset = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart);
      if (type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
          type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
        sid_offset = FIELD_OFFSET(ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
      }
      std::vector<BYTE> ace_bytes(sid_offset + sid_length, BYTE {0});
      auto *header = reinterpret_cast<ACE_HEADER *>(ace_bytes.data());
      header->AceType = type;
      header->AceFlags = flags;
      header->AceSize = static_cast<WORD>(ace_bytes.size());
      *reinterpret_cast<ACCESS_MASK *>(ace_bytes.data() + sizeof(ACE_HEADER)) = mask;
      if (type == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
          type == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
        auto *object_ace = reinterpret_cast<ACCESS_ALLOWED_OBJECT_ACE *>(ace_bytes.data());
        object_ace->Flags = object_flags;
      }
      return CopySid(
               sid_length,
               ace_bytes.data() + sid_offset,
               sid) != FALSE &&
        AddAce(
               acl(),
               ACL_REVISION_DS,
               MAXDWORD,
               ace_bytes.data(),
               static_cast<DWORD>(ace_bytes.size())) != FALSE;
    }
  };

  std::filesystem::path temporary_directory(const wchar_t *suffix) {
    const auto path = std::filesystem::temp_directory_path() /
      (L"vibepollo-msi-security-" + std::to_wstring(GetCurrentProcessId()) +
       L"-" + suffix);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    std::filesystem::create_directories(path);
    return path;
  }

  bool apply_safe_non_exact_security(
    const std::filesystem::path &path,
    bool directory,
    bool protect = true) {
    descriptor_fixture_t fixture;
    const auto flags = directory ?
      (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0u;
    if (flags != 0u) {
      // Add one harmless inherit-only rule so this valid baseline is visibly
      // different from the production exact descriptor.
      if (!fixture.append_allow_ace(
            ACCESS_ALLOWED_ACE_TYPE,
            FILE_GENERIC_READ,
            fixture.users.value,
            flags | INHERIT_ONLY_ACE)) {
        return false;
      }
    }
    return SetNamedSecurityInfoW(
             const_cast<LPWSTR>(path.c_str()),
             SE_FILE_OBJECT,
             OWNER_SECURITY_INFORMATION |
               DACL_SECURITY_INFORMATION |
               (protect ? PROTECTED_DACL_SECURITY_INFORMATION :
                          UNPROTECTED_DACL_SECURITY_INFORMATION),
             fixture.administrators.value,
             nullptr,
             fixture.acl(),
             nullptr) == ERROR_SUCCESS;
  }

  std::wstring security_sddl(const std::filesystem::path &path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()),
      SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      &descriptor);
    if (status != ERROR_SUCCESS) {
      return {};
    }
    LPWSTR text = nullptr;
    ULONG length = 0u;
    const auto converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(
      descriptor,
      SDDL_REVISION_1,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &text,
      &length);
    std::wstring result;
    if (converted && text != nullptr) {
      result.assign(text, length == 0u ? wcslen(text) : length - 1u);
    }
    if (text != nullptr) {
      LocalFree(text);
    }
    LocalFree(descriptor);
    return result;
  }
}

TEST(AudioPolicyMsiSecurity, TreatsSupportedPunctuationAsOpaquePathData) {
  using sunshine::installer_security::testing::normalize_program_files_descendant;
  constexpr std::wstring_view program_files = L"C:\\Program Files";
  const std::array candidates {
    std::wstring {L"C:\\Program Files\\Vibe'pollo"},
    std::wstring {L"C:\\Program Files\\Vibe;pollo"},
    std::wstring {L"C:\\Program Files\\Vibe#comment"},
    std::wstring {L"C:\\Program Files\\Vibe(pollo)"},
  };

  for (const auto &candidate : candidates) {
    std::wstring normalized;
    ASSERT_TRUE(normalize_program_files_descendant(
      program_files,
      candidate,
      normalized)) << std::string(candidate.begin(), candidate.end());
    EXPECT_EQ(normalized, candidate);
  }
}

TEST(AudioPolicyMsiSecurity, RejectsRootTraversalNetworkAndOtherVolumePaths) {
  using sunshine::installer_security::testing::normalize_program_files_descendant;
  constexpr std::wstring_view program_files = L"C:\\Program Files";
  const std::array rejected {
    std::wstring {L"C:\\Program Files"},
    std::wstring {L"C:\\Program Files\\..\\Windows"},
    std::wstring {L"C:\\Program FilesMalicious\\Apollo"},
    std::wstring {L"D:\\Program Files\\Apollo"},
    std::wstring {L"\\\\server\\share\\Apollo"},
    std::wstring {L"C:\\Program Files\\Vibe\r\npollo"},
    std::wstring {L"C:\\Program Files\\Apollo:stream"},
    std::wstring {L"\\\\?\\C:\\Program Files\\Apollo"},
    std::wstring {L"C:Program Files\\Apollo"},
    std::wstring {L"C:\\Program Files\\Apollo. "},
    std::wstring {L"C:\\Program Files\\%SystemRoot%\\Apollo"},
    std::wstring {L"C:\\Program Files\\Apollo|child"},
    std::wstring {L"C:\\Program Files\\Apollo*child"},
    std::wstring {L"C:\\Program Files\\CON\\Apollo"},
    std::wstring {L"C:\\Program Files\\LPT1.txt\\Apollo"},
  };

  for (const auto &candidate : rejected) {
    std::wstring normalized;
    EXPECT_FALSE(normalize_program_files_descendant(
      program_files,
      candidate,
      normalized)) << std::string(candidate.begin(), candidate.end());
  }
}

TEST(AudioPolicyMsiSecurity, RejectsWeakDescendantAcl) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    FILE_WRITE_DATA,
    fixture.users.value));

  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    false));
}

TEST(AudioPolicyMsiSecurity, RejectsNullDaclAndUntrustedOwner) {
  descriptor_fixture_t null_dacl;
  ASSERT_TRUE(SetSecurityDescriptorDacl(
    &null_dacl.descriptor,
    TRUE,
    nullptr,
    FALSE));
  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &null_dacl.descriptor,
    false));

  descriptor_fixture_t untrusted_owner;
  ASSERT_TRUE(SetSecurityDescriptorOwner(
    &untrusted_owner.descriptor,
    untrusted_owner.users.value,
    FALSE));
  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &untrusted_owner.descriptor,
    false));
}

TEST(AudioPolicyMsiSecurity, RejectsHostileParentDeleteChildRight) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    FILE_DELETE_CHILD,
    fixture.users.value));

  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    true));
}

TEST(AudioPolicyMsiSecurity, RejectsHostileAncestorCreateChildRight) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    FILE_WRITE_DATA | FILE_APPEND_DATA,
    fixture.users.value));

  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    true));
}

TEST(AudioPolicyMsiSecurity, RejectsGenericObjectAndCallbackWriteAces) {
  const std::array cases {
    std::pair {ACCESS_ALLOWED_ACE_TYPE, static_cast<ACCESS_MASK>(GENERIC_WRITE)},
    std::pair {ACCESS_ALLOWED_OBJECT_ACE_TYPE, static_cast<ACCESS_MASK>(FILE_WRITE_DATA)},
    std::pair {ACCESS_ALLOWED_CALLBACK_ACE_TYPE, static_cast<ACCESS_MASK>(FILE_WRITE_DATA)},
    std::pair {ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE, static_cast<ACCESS_MASK>(FILE_WRITE_DATA)},
  };

  for (const auto &[type, mask] : cases) {
    descriptor_fixture_t fixture;
    ASSERT_TRUE(fixture.append_allow_ace(type, mask, fixture.users.value));
    EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
      &fixture.descriptor,
      false)) << static_cast<unsigned int>(type);
  }
}

TEST(AudioPolicyMsiSecurity, RejectsObjectAceWithUnknownLayoutFlags) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_OBJECT_ACE_TYPE,
    FILE_WRITE_DATA,
    fixture.users.value,
    0u,
    0x80000000u));
  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    false));
}

TEST(AudioPolicyMsiSecurity, RejectsUntrustedInheritOnlyGenericAllOnDirectory) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    GENERIC_ALL,
    fixture.users.value,
    OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE));

  EXPECT_FALSE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    true));
}

TEST(AudioPolicyMsiSecurity, AcceptsStandardCreatorOwnerInheritOnlyGenericAll) {
  descriptor_fixture_t fixture;
  ASSERT_TRUE(fixture.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    GENERIC_ALL,
    fixture.creator_owner.value,
    OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE));

  EXPECT_TRUE(sunshine::installer_security::testing::security_descriptor_is_safe(
    &fixture.descriptor,
    true));
}

TEST(AudioPolicyMsiSecurity, AcceptsNativeProgramFilesDirectorySecurity) {
  static_assert(sizeof(void *) == 8u);
  PWSTR program_files = nullptr;
  ASSERT_TRUE(SUCCEEDED(SHGetKnownFolderPath(
    FOLDERID_ProgramFilesX64,
    KF_FLAG_DEFAULT,
    nullptr,
    &program_files)));
  ASSERT_NE(program_files, nullptr);
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto status = GetNamedSecurityInfoW(
    program_files,
    SE_FILE_OBJECT,
    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &descriptor);
  CoTaskMemFree(program_files);
  ASSERT_EQ(status, ERROR_SUCCESS);
  ASSERT_NE(descriptor, nullptr);
  EXPECT_TRUE(sunshine::installer_security::testing::security_descriptor_is_safe(
    descriptor,
    true));
  LocalFree(descriptor);
}

TEST(AudioPolicyMsiSecurity, AcceptsFullNativeProgramFilesAncestryToMissingChild) {
  PWSTR program_files = nullptr;
  ASSERT_TRUE(SUCCEEDED(SHGetKnownFolderPath(
    FOLDERID_ProgramFilesX64,
    KF_FLAG_DEFAULT,
    nullptr,
    &program_files)));
  ASSERT_NE(program_files, nullptr);
  const auto candidate = std::filesystem::path {program_files} /
    (L"VibepolloAncestryProbe-" + std::to_wstring(GetCurrentProcessId()));
  CoTaskMemFree(program_files);
  ASSERT_FALSE(std::filesystem::exists(candidate));
  EXPECT_TRUE(
    sunshine::installer_security::testing::validate_native_program_files_ancestry(
      candidate));
}

TEST(AudioPolicyMsiSecurity, RejectsNestedDirectoryWithDangerousInheritedChildren) {
  using namespace sunshine::installer_security::testing;
  const auto root = temporary_directory(L"preflight-inheritance");
  const auto nested = root / L"drivers";
  std::filesystem::create_directories(nested);
  descriptor_fixture_t nested_security;
  ASSERT_TRUE(nested_security.append_allow_ace(
    ACCESS_ALLOWED_ACE_TYPE,
    GENERIC_ALL,
    nested_security.users.value,
    OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE));
  ASSERT_EQ(
    SetNamedSecurityInfoW(
      const_cast<LPWSTR>(nested.c_str()),
      SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
        PROTECTED_DACL_SECURITY_INFORMATION,
      nested_security.administrators.value,
      nullptr,
      nested_security.acl(),
      nullptr),
    ERROR_SUCCESS);
  ASSERT_TRUE(apply_safe_non_exact_security(root, true));

  EXPECT_FALSE(preflight_existing_tree(root));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, RejectsChildReparsePointBeforeHardening) {
  using namespace sunshine::installer_security::testing;
  FILE_ATTRIBUTE_TAG_INFO attributes {};
  attributes.FileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
  attributes.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  EXPECT_FALSE(attributes_are_direct(attributes));

  const auto root = temporary_directory(L"reparse");
  const auto target = temporary_directory(L"reparse-target");
  const auto link = root / L"child-junction";
  const auto created = CreateSymbolicLinkW(
    link.c_str(),
    target.c_str(),
    SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  if (created == 0) {
    std::filesystem::remove_all(root);
    std::filesystem::remove_all(target);
    GTEST_SKIP() << "symbolic-link privilege unavailable";
  }
  EXPECT_TRUE(tree_contains_reparse_point(root));
  RemoveDirectoryW(link.c_str());
  std::filesystem::remove_all(root);
  std::filesystem::remove_all(target);
}

TEST(AudioPolicyMsiSecurity, ExactSecurityApplicationIsCleanAndIdempotent) {
  using namespace sunshine::installer_security::testing;
  const auto root = temporary_directory(L"idempotent");
  const auto child = root / L"payload.exe";
  const auto file = CreateFileW(
    child.c_str(),
    GENERIC_WRITE,
    0u,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);

  ASSERT_TRUE(apply_exact_security(child, false));
  ASSERT_TRUE(has_exact_security(child, false));
  ASSERT_TRUE(apply_exact_security(child, false));
  EXPECT_TRUE(has_exact_security(child, false));

  ASSERT_TRUE(apply_exact_security(root, true));
  ASSERT_TRUE(has_exact_security(root, true));
  ASSERT_TRUE(apply_exact_security(root, true));
  EXPECT_TRUE(has_exact_security(root, true));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, RejectsHardlinkedPayload) {
  using sunshine::installer_security::testing::is_single_link_file;
  const auto root = temporary_directory(L"hardlink");
  const auto payload = root / L"payload.exe";
  const auto second_name = root / L"payload-second-name.exe";
  const auto file = CreateFileW(
    payload.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  ASSERT_TRUE(CreateHardLinkW(second_name.c_str(), payload.c_str(), nullptr));

  EXPECT_FALSE(is_single_link_file(payload));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, RejectsPreopenedWriterBeforePinning) {
  using sunshine::installer_security::testing::is_exclusively_pinnable;
  const auto root = temporary_directory(L"writer");
  const auto payload = root / L"payload.exe";
  const auto writer = CreateFileW(
    payload.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(writer, INVALID_HANDLE_VALUE);

  EXPECT_FALSE(is_exclusively_pinnable(payload, false));
  CloseHandle(writer);
  EXPECT_TRUE(is_exclusively_pinnable(payload, false));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, RepinDetectsPayloadIdentityReplacement) {
  using namespace sunshine::installer_security::testing;
  const auto root = temporary_directory(L"repin");
  const auto payload = root / L"payload.exe";
  auto file = CreateFileW(
    payload.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  const auto identity = capture_identity(payload, false);
  ASSERT_TRUE(identity.has_value());
  EXPECT_TRUE(identity_matches(payload, false, *identity));

  ASSERT_TRUE(DeleteFileW(payload.c_str()));
  file = CreateFileW(
    payload.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  EXPECT_FALSE(identity_matches(payload, false, *identity));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, CustomActionDataLengthPrefixKeepsPunctuationOpaque) {
  using namespace sunshine::installer_security::testing;
  const std::wstring root {L"C:\\Program Files\\Vibe'pollo;#(data)"};
  const std::wstring journal {L"C:\\Windows\\Temp\\journal;#(data).bin"};
  const auto encoded = encode_custom_action_data(root, journal);
  std::wstring decoded_root;
  std::wstring decoded_journal;
  ASSERT_TRUE(decode_custom_action_data(encoded, decoded_root, decoded_journal));
  EXPECT_EQ(decoded_root, root);
  EXPECT_EQ(decoded_journal, journal);
  EXPECT_FALSE(decode_custom_action_data(
    L"999:C:\\truncated",
    decoded_root,
    decoded_journal));
}

TEST(AudioPolicyMsiSecurity, JournalRestoresReverseOrderAfterPartialForwardFailure) {
  using namespace sunshine::installer_security::testing;
  const auto root = temporary_directory(L"journal-partial");
  const auto child = root / L"payload.exe";
  const auto nested = root / L"nested";
  const auto grandchild = nested / L"grandchild.bin";
  const auto journal = root.parent_path() /
    (root.filename().wstring() + L".journal");
  auto file = CreateFileW(
    child.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  std::filesystem::create_directories(nested);
  file = CreateFileW(
    grandchild.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  ASSERT_TRUE(apply_safe_non_exact_security(child, false));
  ASSERT_TRUE(apply_safe_non_exact_security(grandchild, false, false));
  ASSERT_TRUE(apply_safe_non_exact_security(nested, true, false));
  ASSERT_TRUE(apply_safe_non_exact_security(root, true));
  const auto root_before = security_sddl(root);
  const auto child_before = security_sddl(child);
  const auto nested_before = security_sddl(nested);
  const auto grandchild_before = security_sddl(grandchild);
  ASSERT_FALSE(root_before.empty());
  ASSERT_FALSE(child_before.empty());

  EXPECT_FALSE(harden_tree_with_journal(root, journal, 1u)) << GetLastError();
  ASSERT_TRUE(std::filesystem::exists(journal));
  EXPECT_TRUE(restore_tree_from_journal(journal));
  EXPECT_EQ(security_sddl(root), root_before);
  EXPECT_EQ(security_sddl(child), child_before);
  EXPECT_EQ(security_sddl(nested), nested_before);
  EXPECT_EQ(security_sddl(grandchild), grandchild_before);
  EXPECT_FALSE(std::filesystem::exists(journal));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, JournalIgnoresIncompleteTailAndRestoresCompleteRecords) {
  using namespace sunshine::installer_security::testing;
  const auto root = temporary_directory(L"journal-tail");
  const auto child = root / L"payload.exe";
  const auto journal = root.parent_path() /
    (root.filename().wstring() + L".journal");
  auto file = CreateFileW(
    child.c_str(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    nullptr,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  ASSERT_TRUE(apply_safe_non_exact_security(child, false));
  ASSERT_TRUE(apply_safe_non_exact_security(root, true));
  const auto root_before = security_sddl(root);
  const auto child_before = security_sddl(child);

  ASSERT_TRUE(harden_tree_with_journal(root, journal)) << GetLastError();
  EXPECT_TRUE(has_exact_security(root, true));
  EXPECT_TRUE(has_exact_security(child, false));
  {
    std::ofstream tail {journal, std::ios::binary | std::ios::app};
    ASSERT_TRUE(tail.is_open());
    const std::array corrupt_tail {BYTE {0x44}, BYTE {0x55}, BYTE {0x66}};
    tail.write(
      reinterpret_cast<const char *>(corrupt_tail.data()),
      static_cast<std::streamsize>(corrupt_tail.size()));
  }
  EXPECT_TRUE(restore_tree_from_journal(journal));
  EXPECT_EQ(security_sddl(root), root_before);
  EXPECT_EQ(security_sddl(child), child_before);
  EXPECT_FALSE(std::filesystem::exists(journal));
  std::filesystem::remove_all(root);
}

TEST(AudioPolicyMsiSecurity, RejectsJournalLargerThanReaderLimitBeforeMutation) {
  using sunshine::installer_security::testing::journal_size_is_acceptable;
  constexpr std::uint64_t limit = 64ull * 1024ull * 1024ull;
  EXPECT_TRUE(journal_size_is_acceptable(limit - 1u, 1u));
  EXPECT_FALSE(journal_size_is_acceptable(limit, 1u));
  EXPECT_FALSE(journal_size_is_acceptable(
    std::numeric_limits<std::uint64_t>::max(),
    1u));
}

TEST(AudioPolicyMsiSecurity, ClassifiesRepairAndPartialRemoveFromActionStates) {
  using sunshine::installer_security::testing::classify_operation_state;

  const auto repair = classify_operation_state(
    L"",
    INSTALLSTATE_LOCAL,
    INSTALLSTATE_LOCAL,
    INSTALLSTATE_LOCAL);
  ASSERT_TRUE(repair.has_value());
  EXPECT_TRUE(repair->protect_required);
  EXPECT_FALSE(repair->full_remove);

  const auto partial = classify_operation_state(
    L"ProductFeature",
    INSTALLSTATE_ABSENT,
    INSTALLSTATE_LOCAL,
    INSTALLSTATE_LOCAL);
  ASSERT_TRUE(partial.has_value());
  EXPECT_TRUE(partial->protect_required);
  EXPECT_FALSE(partial->full_remove);
}

TEST(AudioPolicyMsiSecurity, ClassifiesCaseInsensitiveCommaDelimitedFullRemove) {
  using sunshine::installer_security::testing::classify_operation_state;

  for (const auto remove : {
         L"all",
         L"Other, ALL ,Ignored",
         L"productfeature, vibepolloextras"}) {
    const auto state = classify_operation_state(
      remove,
      INSTALLSTATE_ABSENT,
      INSTALLSTATE_ABSENT,
      INSTALLSTATE_ABSENT);
    ASSERT_TRUE(state.has_value()) << std::wstring {remove};
    EXPECT_FALSE(state->protect_required) << std::wstring {remove};
    EXPECT_TRUE(state->full_remove) << std::wstring {remove};
  }
}

TEST(AudioPolicyMsiSecurity, RejectsRemoveIntentContradictingComponentActionState) {
  using sunshine::installer_security::testing::classify_operation_state;

  EXPECT_FALSE(classify_operation_state(
    L"ALL",
    INSTALLSTATE_ABSENT,
    INSTALLSTATE_ABSENT,
    INSTALLSTATE_LOCAL).has_value());
  EXPECT_FALSE(classify_operation_state(
    L"ProductFeature,VibepolloExtras",
    INSTALLSTATE_LOCAL,
    INSTALLSTATE_ABSENT,
    INSTALLSTATE_ABSENT).has_value());
}
