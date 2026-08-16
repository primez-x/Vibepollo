/**
 * @file audio_policy_security_custom_action.cpp
 * @brief Native Windows Installer trust boundary for installed audio-policy code.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Aclapi.h>
#include <ShlObj.h>
#include <msiquery.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sunshine::installer_security {
  namespace {
    constexpr auto kTrustedInstallerSid =
      L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

    bool validate_required_payloads(const std::filesystem::path &root);

    class local_memory_t {
    public:
      local_memory_t() = default;
      explicit local_memory_t(void *value): value_(value) {}
      ~local_memory_t() {
        if (value_ != nullptr) {
          LocalFree(value_);
        }
      }
      local_memory_t(const local_memory_t &) = delete;
      local_memory_t &operator=(const local_memory_t &) = delete;
      local_memory_t(local_memory_t &&other) noexcept:
          value_(std::exchange(other.value_, nullptr)) {}
      local_memory_t &operator=(local_memory_t &&other) noexcept {
        if (this != &other) {
          if (value_ != nullptr) {
            LocalFree(value_);
          }
          value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
      }
      void *get() const { return value_; }
      void **put() {
        if (value_ != nullptr) {
          LocalFree(value_);
          value_ = nullptr;
        }
        return &value_;
      }

    private:
      void *value_ {nullptr};
    };

    class handle_t {
    public:
      handle_t() = default;
      explicit handle_t(HANDLE value): value_(value) {}
      ~handle_t() {
        if (valid()) {
          CloseHandle(value_);
        }
      }
      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;
      handle_t(handle_t &&other) noexcept:
          value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
      handle_t &operator=(handle_t &&other) noexcept {
        if (this != &other) {
          if (valid()) {
            CloseHandle(value_);
          }
          value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
      }
      bool valid() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
      }
      HANDLE get() const { return value_; }
      void reset() {
        if (valid()) {
          CloseHandle(value_);
        }
        value_ = INVALID_HANDLE_VALUE;
      }

    private:
      HANDLE value_ {INVALID_HANDLE_VALUE};
    };

    class find_handle_t {
    public:
      explicit find_handle_t(HANDLE value): value_(value) {}
      ~find_handle_t() {
        if (value_ != INVALID_HANDLE_VALUE) {
          FindClose(value_);
        }
      }
      find_handle_t(const find_handle_t &) = delete;
      find_handle_t &operator=(const find_handle_t &) = delete;
      HANDLE get() const { return value_; }

    private:
      HANDLE value_ {INVALID_HANDLE_VALUE};
    };

    bool equal_ordinal(std::wstring_view left, std::wstring_view right) {
      if (left.size() != right.size()) {
        return false;
      }
      return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
    }

    bool starts_with_ordinal(std::wstring_view value, std::wstring_view prefix) {
      return value.size() >= prefix.size() &&
        CompareStringOrdinal(
          value.data(),
          static_cast<int>(prefix.size()),
          prefix.data(),
          static_cast<int>(prefix.size()),
          TRUE) == CSTR_EQUAL;
    }

    bool contains_forbidden_alias_or_control(std::wstring_view path) {
      if (path.size() < 3u || path[1] != L':' ||
          (path[2] != L'\\' && path[2] != L'/')) {
        return true;
      }
      for (std::size_t index = 0; index < path.size(); ++index) {
        const auto character = path[index];
        // Percent expansion remains meaningful to the legacy cmd.exe batch
        // actions that run only after this native gate. Other punctuation is
        // deliberately opaque data here; this is not a shell escaping rule.
        if (character < 0x20 || character == L'%' ||
            character == L'"' || character == L'<' || character == L'>' ||
            character == L'|' || character == L'?' || character == L'*' ||
            (character == L':' && index != 1u)) {
          return true;
        }
      }
      if (path.size() == 3u) {
        return false;
      }
      std::size_t component_start = 3u;
      for (std::size_t index = 3u; index <= path.size(); ++index) {
        if (index == path.size() || path[index] == L'\\' || path[index] == L'/') {
          if (index == component_start || path[index - 1u] == L'.' ||
              path[index - 1u] == L' ') {
            return true;
          }
          auto component = std::wstring {path.substr(
            component_start,
            index - component_start)};
          const auto extension = component.find(L'.');
          if (extension != std::wstring::npos) {
            component.resize(extension);
          }
          std::ranges::transform(
            component,
            component.begin(),
            [](wchar_t character) {
              return static_cast<wchar_t>(std::towupper(character));
            });
          const bool reserved = component == L"CON" || component == L"PRN" ||
            component == L"AUX" || component == L"NUL" ||
            component == L"CLOCK$" ||
            (component.size() == 4u &&
             (component.starts_with(L"COM") || component.starts_with(L"LPT")) &&
             component[3] >= L'1' && component[3] <= L'9');
          if (reserved) {
            return true;
          }
          component_start = index + 1u;
        }
      }
      return false;
    }

    bool full_path(std::wstring_view input, std::wstring &output) {
      if (input.empty() || input.size() > 32760u ||
          contains_forbidden_alias_or_control(input) ||
          (input.size() >= 2u && input[0] == L'\\' && input[1] == L'\\')) {
        return false;
      }
      std::wstring value {input};
      const auto required = GetFullPathNameW(value.c_str(), 0u, nullptr, nullptr);
      if (required == 0u || required > 32760u) {
        return false;
      }
      std::wstring buffer(required, L'\0');
      const auto written = GetFullPathNameW(
        value.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
      if (written == 0u || written >= buffer.size()) {
        return false;
      }
      buffer.resize(written);
      std::ranges::replace(buffer, L'/', L'\\');
      while (buffer.size() > 3u && buffer.back() == L'\\') {
        buffer.pop_back();
      }
      if (contains_forbidden_alias_or_control(buffer)) {
        return false;
      }
      output = std::move(buffer);
      return true;
    }

    bool normalize_program_files_descendant_impl(
      std::wstring_view program_files,
      std::wstring_view candidate,
      std::wstring &normalized) {
      std::wstring canonical_program_files;
      std::wstring canonical_candidate;
      if (!full_path(program_files, canonical_program_files) ||
          !full_path(candidate, canonical_candidate) ||
          equal_ordinal(canonical_program_files, canonical_candidate)) {
        return false;
      }
      const auto prefix = canonical_program_files + L"\\";
      if (!starts_with_ordinal(canonical_candidate, prefix)) {
        return false;
      }
      normalized = std::move(canonical_candidate);
      return true;
    }

    struct sid_set_t {
      local_memory_t system;
      local_memory_t administrators;
      local_memory_t trusted_installer;
      local_memory_t users;
      local_memory_t creator_owner;
      bool valid {false};

      sid_set_t() {
        PSID value = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-5-18", &value)) {
          return;
        }
        system = local_memory_t {value};
        value = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-5-32-544", &value)) {
          return;
        }
        administrators = local_memory_t {value};
        value = nullptr;
        if (!ConvertStringSidToSidW(kTrustedInstallerSid, &value)) {
          return;
        }
        trusted_installer = local_memory_t {value};
        value = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-5-32-545", &value)) {
          return;
        }
        users = local_memory_t {value};
        value = nullptr;
        if (!ConvertStringSidToSidW(L"S-1-3-0", &value)) {
          return;
        }
        creator_owner = local_memory_t {value};
        valid = true;
      }

      bool trusted(PSID sid) const {
        return valid && sid != nullptr && IsValidSid(sid) &&
          (EqualSid(sid, system.get()) ||
           EqualSid(sid, administrators.get()) ||
           EqualSid(sid, trusted_installer.get()));
      }

      bool creator(PSID sid) const {
        return valid && sid != nullptr && IsValidSid(sid) &&
          EqualSid(sid, creator_owner.get());
      }
    };

    struct allow_ace_t {
      ACCESS_MASK mask {0u};
      PSID sid {nullptr};
      bool recognized {false};
    };

    allow_ace_t parse_allow_ace(const ACE_HEADER *header) {
      allow_ace_t result;
      if (header == nullptr || header->AceSize < sizeof(ACE_HEADER) + sizeof(ACCESS_MASK)) {
        return result;
      }
      const auto *bytes = reinterpret_cast<const BYTE *>(header);
      result.mask = *reinterpret_cast<const ACCESS_MASK *>(bytes + sizeof(ACE_HEADER));
      std::size_t sid_offset = 0u;
      switch (header->AceType) {
        case ACCESS_ALLOWED_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_ACE_TYPE:
          sid_offset = FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart);
          break;
        case ACCESS_ALLOWED_OBJECT_ACE_TYPE:
        case ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE: {
          if (header->AceSize < FIELD_OFFSET(ACCESS_ALLOWED_OBJECT_ACE, ObjectType)) {
            return result;
          }
          const auto *object_ace = reinterpret_cast<const ACCESS_ALLOWED_OBJECT_ACE *>(header);
          constexpr DWORD known_flags =
            ACE_OBJECT_TYPE_PRESENT | ACE_INHERITED_OBJECT_TYPE_PRESENT;
          if ((object_ace->Flags & ~known_flags) != 0u) {
            return result;
          }
          sid_offset = FIELD_OFFSET(ACCESS_ALLOWED_OBJECT_ACE, ObjectType);
          if ((object_ace->Flags & ACE_OBJECT_TYPE_PRESENT) != 0u) {
            sid_offset += sizeof(GUID);
          }
          if ((object_ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) != 0u) {
            sid_offset += sizeof(GUID);
          }
          break;
        }
        default:
          return result;
      }
      if (sid_offset >= header->AceSize) {
        return result;
      }
      constexpr std::size_t minimum_sid_size =
        FIELD_OFFSET(SID, SubAuthority);
      if (sid_offset + minimum_sid_size > header->AceSize) {
        return result;
      }
      const auto *sid_bytes = bytes + sid_offset;
      const auto sub_authority_count =
        sid_bytes[offsetof(SID, SubAuthorityCount)];
      const auto sid_size = minimum_sid_size +
        static_cast<std::size_t>(sub_authority_count) * sizeof(DWORD);
      if (sid_offset + sid_size > header->AceSize) {
        return result;
      }
      result.sid = const_cast<PSID>(reinterpret_cast<const void *>(sid_bytes));
      if (!IsValidSid(result.sid) || GetLengthSid(result.sid) != sid_size) {
        result.sid = nullptr;
        return result;
      }
      result.recognized = true;
      return result;
    }

    bool security_descriptor_is_safe_impl(
      PSECURITY_DESCRIPTOR descriptor,
      bool is_ancestor,
      ACCESS_MASK danger_override = 0u,
      bool directory_can_create_children = false) {
      if (descriptor == nullptr || !IsValidSecurityDescriptor(descriptor)) {
        return false;
      }
      sid_set_t sids;
      if (!sids.valid) {
        return false;
      }
      PSID owner = nullptr;
      BOOL owner_defaulted = FALSE;
      if (!GetSecurityDescriptorOwner(descriptor, &owner, &owner_defaulted) ||
          !sids.trusted(owner)) {
        return false;
      }
      PACL dacl = nullptr;
      BOOL dacl_present = FALSE;
      BOOL dacl_defaulted = FALSE;
      if (!GetSecurityDescriptorDacl(
            descriptor,
            &dacl_present,
            &dacl,
            &dacl_defaulted) ||
          !dacl_present || dacl == nullptr || !IsValidAcl(dacl)) {
        return false;
      }

      GENERIC_MAPPING mapping {
        FILE_GENERIC_READ,
        FILE_GENERIC_WRITE,
        FILE_GENERIC_EXECUTE,
        FILE_ALL_ACCESS,
      };
      constexpr ACCESS_MASK common_danger = DELETE | WRITE_DAC | WRITE_OWNER;
      constexpr ACCESS_MASK descendant_danger =
        common_danger | FILE_WRITE_DATA | FILE_APPEND_DATA |
        FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD;
      constexpr ACCESS_MASK ancestor_danger = descendant_danger;
      const auto dangerous = danger_override != 0u ? danger_override :
        (is_ancestor ? ancestor_danger : descendant_danger);

      ACL_SIZE_INFORMATION acl_info {};
      if (!GetAclInformation(
            dacl,
            &acl_info,
            sizeof(acl_info),
            AclSizeInformation)) {
        return false;
      }
      for (DWORD index = 0u; index < acl_info.AceCount; ++index) {
        void *raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace) || raw_ace == nullptr) {
          return false;
        }
        const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
        const bool allow_type =
          header->AceType == ACCESS_ALLOWED_ACE_TYPE ||
          header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
          header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
          header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE;
        if (!allow_type) {
          switch (header->AceType) {
            case ACCESS_DENIED_ACE_TYPE:
            case ACCESS_DENIED_OBJECT_ACE_TYPE:
            case ACCESS_DENIED_CALLBACK_ACE_TYPE:
            case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
            case SYSTEM_AUDIT_ACE_TYPE:
            case SYSTEM_AUDIT_OBJECT_ACE_TYPE:
            case SYSTEM_AUDIT_CALLBACK_ACE_TYPE:
            case SYSTEM_AUDIT_CALLBACK_OBJECT_ACE_TYPE:
            case SYSTEM_MANDATORY_LABEL_ACE_TYPE:
            case SYSTEM_RESOURCE_ATTRIBUTE_ACE_TYPE:
            case SYSTEM_SCOPED_POLICY_ID_ACE_TYPE:
            case SYSTEM_PROCESS_TRUST_LABEL_ACE_TYPE:
              continue;
            default:
              return false;
          }
        }
        auto parsed = parse_allow_ace(header);
        if (!parsed.recognized) {
          return false;
        }
        MapGenericMask(&parsed.mask, &mapping);
        if (!sids.trusted(parsed.sid)) {
          const bool inherit_only =
            (header->AceFlags & INHERIT_ONLY_ACE) != 0u;
          if (!inherit_only && (parsed.mask & dangerous) != 0u) {
            return false;
          }
          const bool inherits_to_child = directory_can_create_children &&
            (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != 0u;
          if (inherits_to_child && (parsed.mask & descendant_danger) != 0u &&
              !(inherit_only && sids.creator(parsed.sid))) {
            return false;
          }
        }
      }
      return true;
    }

    bool attributes_are_direct_impl(const FILE_ATTRIBUTE_TAG_INFO &attributes) {
      return (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0u &&
        attributes.ReparseTag == 0u;
    }

    bool tree_contains_reparse_point_impl(const std::filesystem::path &root) {
      const auto root_attributes = GetFileAttributesW(root.c_str());
      if (root_attributes == INVALID_FILE_ATTRIBUTES) {
        return true;
      }
      if ((root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        return true;
      }
      const auto pattern = root / L"*";
      WIN32_FIND_DATAW data {};
      find_handle_t search {FindFirstFileW(pattern.c_str(), &data)};
      if (search.get() == INVALID_HANDLE_VALUE) {
        return GetLastError() != ERROR_FILE_NOT_FOUND;
      }
      do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
          continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
          return true;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u &&
            tree_contains_reparse_point_impl(root / data.cFileName)) {
          return true;
        }
      } while (FindNextFileW(search.get(), &data));
      return GetLastError() != ERROR_NO_MORE_FILES;
    }

    handle_t open_security_handle(
      const std::filesystem::path &path,
      bool directory,
      bool exclusive) {
      auto access = static_cast<DWORD>(READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES);
      if (exclusive) {
        access |= GENERIC_READ;
      }
      if (directory) {
        access |= FILE_LIST_DIRECTORY;
      }
      const auto sharing = exclusive ? (directory ? FILE_SHARE_READ : 0u) :
        (FILE_SHARE_READ | FILE_SHARE_WRITE);
      const auto flags = FILE_FLAG_OPEN_REPARSE_POINT |
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0u);
      return handle_t {CreateFileW(
        path.c_str(),
        access,
        sharing,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr)};
    }

    bool query_direct_attributes(HANDLE handle, FILE_ATTRIBUTE_TAG_INFO &attributes) {
      return GetFileInformationByHandleEx(
               handle,
               FileAttributeTagInfo,
               &attributes,
               sizeof(attributes)) != FALSE &&
        attributes_are_direct_impl(attributes);
    }

    bool make_exact_acl(bool directory, const sid_set_t &sids, std::vector<BYTE> &buffer) {
      if (!sids.valid) {
        return false;
      }
      const std::array<PSID, 4u> principals {
        static_cast<PSID>(sids.system.get()),
        static_cast<PSID>(sids.administrators.get()),
        static_cast<PSID>(sids.trusted_installer.get()),
        static_cast<PSID>(sids.users.get()),
      };
      DWORD size = sizeof(ACL);
      for (auto sid : principals) {
        size += FIELD_OFFSET(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(sid);
      }
      buffer.assign(size, BYTE {0});
      auto *acl = reinterpret_cast<PACL>(buffer.data());
      if (!InitializeAcl(acl, size, ACL_REVISION)) {
        return false;
      }
      const auto flags = static_cast<DWORD>(directory ?
        (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0u);
      for (std::size_t index = 0u; index < principals.size(); ++index) {
        const auto mask = index == principals.size() - 1u ?
          (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) : FILE_ALL_ACCESS;
        if (!AddAccessAllowedAceEx(
              acl,
              ACL_REVISION,
              flags,
              mask,
              principals[index])) {
          return false;
        }
      }
      return true;
    }

    bool apply_exact_security_handle(HANDLE handle, bool directory) {
      sid_set_t sids;
      std::vector<BYTE> acl_bytes;
      if (!make_exact_acl(directory, sids, acl_bytes)) {
        return false;
      }
      return SetSecurityInfo(
               handle,
               SE_FILE_OBJECT,
               OWNER_SECURITY_INFORMATION |
                 DACL_SECURITY_INFORMATION |
                 PROTECTED_DACL_SECURITY_INFORMATION,
               static_cast<PSID>(sids.administrators.get()),
               nullptr,
               reinterpret_cast<PACL>(acl_bytes.data()),
               nullptr) == ERROR_SUCCESS;
    }

    bool has_exact_security_descriptor(PSECURITY_DESCRIPTOR descriptor, bool directory) {
      if (descriptor == nullptr || !IsValidSecurityDescriptor(descriptor)) {
        return false;
      }
      sid_set_t sids;
      PSID owner = nullptr;
      BOOL defaulted = FALSE;
      if (!sids.valid ||
          !GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) ||
          !EqualSid(owner, sids.administrators.get())) {
        return false;
      }
      SECURITY_DESCRIPTOR_CONTROL control = 0u;
      DWORD revision = 0u;
      if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
          (control & SE_DACL_PROTECTED) == 0u) {
        return false;
      }
      PACL dacl = nullptr;
      BOOL present = FALSE;
      if (!GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) ||
          !present || dacl == nullptr || !IsValidAcl(dacl)) {
        return false;
      }
      ACL_SIZE_INFORMATION info {};
      if (!GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation) ||
          info.AceCount != 4u) {
        return false;
      }
      const std::array<PSID, 4u> expected_sids {
        static_cast<PSID>(sids.system.get()),
        static_cast<PSID>(sids.administrators.get()),
        static_cast<PSID>(sids.trusted_installer.get()),
        static_cast<PSID>(sids.users.get()),
      };
      const auto expected_flags = static_cast<BYTE>(directory ?
        (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) : 0u);
      for (DWORD index = 0u; index < info.AceCount; ++index) {
        void *raw = nullptr;
        if (!GetAce(dacl, index, &raw) || raw == nullptr) {
          return false;
        }
        const auto *header = static_cast<const ACE_HEADER *>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceFlags != expected_flags) {
          return false;
        }
        const auto parsed = parse_allow_ace(header);
        const auto expected_mask = index == 3u ?
          (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE) : FILE_ALL_ACCESS;
        if (!parsed.recognized || parsed.mask != expected_mask ||
            !EqualSid(parsed.sid, expected_sids[index])) {
          return false;
        }
      }
      return true;
    }

    bool has_exact_security_handle(HANDLE handle, bool directory) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      const auto status = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
      local_memory_t storage {descriptor};
      return status == ERROR_SUCCESS &&
        has_exact_security_descriptor(descriptor, directory);
    }

    struct file_identity_internal_t {
      ULONGLONG volume_serial {0u};
      std::array<BYTE, 16u> file_id {};

      bool operator==(const file_identity_internal_t &) const = default;
    };

    bool query_identity(HANDLE handle, file_identity_internal_t &result) {
      FILE_ID_INFO identity {};
      if (!GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            &identity,
            sizeof(identity))) {
        return false;
      }
      result.volume_serial = identity.VolumeSerialNumber;
      std::copy_n(
        identity.FileId.Identifier,
        result.file_id.size(),
        result.file_id.begin());
      return true;
    }

    bool query_single_link(HANDLE handle, bool directory) {
      if (directory) {
        return true;
      }
      BY_HANDLE_FILE_INFORMATION information {};
      return GetFileInformationByHandle(handle, &information) != FALSE &&
        information.nNumberOfLinks == 1u;
    }

    bool query_safe_security(
      HANDLE handle,
      bool ancestor,
      ACCESS_MASK danger_override = 0u,
      bool directory_can_create_children = false) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      const auto status = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
      local_memory_t storage {descriptor};
      return status == ERROR_SUCCESS &&
        security_descriptor_is_safe_impl(
          descriptor,
          ancestor,
          danger_override,
          directory_can_create_children);
    }

    struct pinned_entry_t {
      std::filesystem::path path;
      bool directory {false};
      handle_t handle;
      file_identity_internal_t identity;
    };

    handle_t open_preflight_handle(
      const std::filesystem::path &path,
      bool directory) {
      auto access = static_cast<DWORD>(READ_CONTROL | FILE_READ_ATTRIBUTES);
      if (directory) {
        access |= FILE_LIST_DIRECTORY;
      }
      return handle_t {CreateFileW(
        path.c_str(),
        access,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT |
          (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0u),
        nullptr)};
    }

    bool collect_preflight_tree(
      const std::filesystem::path &path,
      bool directory,
      std::vector<pinned_entry_t> &entries) {
      auto handle = open_preflight_handle(path, directory);
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      file_identity_internal_t identity;
      if (!handle.valid() || !query_direct_attributes(handle.get(), attributes) ||
          ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) != directory ||
          !query_identity(handle.get(), identity) ||
          !query_single_link(handle.get(), directory) ||
          !query_safe_security(handle.get(), false, 0u, directory)) {
        return false;
      }
      entries.push_back(pinned_entry_t {
        path,
        directory,
        std::move(handle),
        identity,
      });
      if (!directory) {
        return true;
      }

      WIN32_FIND_DATAW find_data {};
      find_handle_t search {FindFirstFileW((path / L"*").c_str(), &find_data)};
      if (search.get() == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
      }
      do {
        const std::wstring_view name {find_data.cFileName};
        if (name == L"." || name == L"..") {
          continue;
        }
        const bool child_directory =
          (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            !collect_preflight_tree(
              path / std::wstring {name},
              child_directory,
              entries)) {
          return false;
        }
      } while (FindNextFileW(search.get(), &find_data));
      return GetLastError() == ERROR_NO_MORE_FILES;
    }

    bool preflight_existing_tree_impl(const std::filesystem::path &root) {
      const auto attributes = GetFileAttributesW(root.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ||
          GetLastError() == ERROR_PATH_NOT_FOUND;
      }
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        return false;
      }
      std::vector<pinned_entry_t> entries;
      return collect_preflight_tree(root, true, entries) && !entries.empty();
    }

    bool collect_pinned_tree(
      const std::filesystem::path &path,
      bool directory,
      std::vector<pinned_entry_t> &entries) {
      auto handle = open_security_handle(path, directory, true);
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      file_identity_internal_t identity;
      if (!handle.valid()) {
        return false;
      }
      if (!query_direct_attributes(handle.get(), attributes)) {
        return false;
      }
      if (((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) != directory) {
        return false;
      }
      if (!query_identity(handle.get(), identity)) {
        return false;
      }
      if (!query_single_link(handle.get(), directory)) {
        return false;
      }
      if (!query_safe_security(handle.get(), false, 0u, directory)) {
        return false;
      }
      entries.push_back(pinned_entry_t {
        path,
        directory,
        std::move(handle),
        identity,
      });
      if (!directory) {
        return true;
      }

      WIN32_FIND_DATAW find_data {};
      const auto pattern = path / L"*";
      find_handle_t search {FindFirstFileW(pattern.c_str(), &find_data)};
      if (search.get() == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
      }
      do {
        const std::wstring_view name {find_data.cFileName};
        if (name == L"." || name == L"..") {
          continue;
        }
        const bool child_directory =
          (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            !collect_pinned_tree(
              path / std::wstring {name},
              child_directory,
              entries)) {
          return false;
        }
      } while (FindNextFileW(search.get(), &find_data));
      return GetLastError() == ERROR_NO_MORE_FILES;
    }

    constexpr std::array<BYTE, 8u> kJournalMagic {
      'V', 'B', 'A', 'P', 'S', 'E', 'C', '1',
    };
    constexpr std::uint32_t kJournalRecordMagic = 0x31434552u;  // REC1
    constexpr std::uint64_t kMaximumJournalBytes = 64ull * 1024ull * 1024ull;

    bool journal_size_is_acceptable_impl(
      std::uint64_t current_bytes,
      std::uint64_t additional_bytes) {
      return current_bytes <= kMaximumJournalBytes &&
        additional_bytes <= kMaximumJournalBytes - current_bytes;
    }

#pragma pack(push, 1)
    struct journal_header_t {
      std::array<BYTE, 8u> magic;
      std::uint32_t version;
      std::uint32_t reserved;
    };

    struct journal_record_header_t {
      std::uint32_t magic;
      std::uint32_t path_characters;
      std::uint32_t security_descriptor_bytes;
      std::uint32_t directory;
      ULONGLONG volume_serial;
      std::array<BYTE, 16u> file_id;
    };
#pragma pack(pop)

    bool write_all(HANDLE file, const void *data, DWORD size) {
      const auto *cursor = static_cast<const BYTE *>(data);
      DWORD remaining = size;
      while (remaining != 0u) {
        DWORD written = 0u;
        if (!WriteFile(file, cursor, remaining, &written, nullptr) || written == 0u) {
          return false;
        }
        cursor += written;
        remaining -= written;
      }
      return true;
    }

    bool read_all_file(const std::filesystem::path &path, std::vector<BYTE> &bytes) {
      handle_t file {CreateFileW(
        path.c_str(),
        GENERIC_READ,
        0u,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
      LARGE_INTEGER size {};
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      if (!file.valid() || !query_direct_attributes(file.get(), attributes) ||
          !query_single_link(file.get(), false) ||
          !has_exact_security_handle(file.get(), false) ||
          !GetFileSizeEx(file.get(), &size) ||
          size.QuadPart < 0 ||
          static_cast<std::uint64_t>(size.QuadPart) > kMaximumJournalBytes) {
        return false;
      }
      bytes.assign(static_cast<std::size_t>(size.QuadPart), BYTE {0});
      DWORD total = 0u;
      while (total < bytes.size()) {
        DWORD read = 0u;
        const auto remaining = static_cast<DWORD>(bytes.size() - total);
        if (!ReadFile(file.get(), bytes.data() + total, remaining, &read, nullptr) ||
            read == 0u) {
          return false;
        }
        total += read;
      }
      return true;
    }

    handle_t create_secure_journal(const std::filesystem::path &path) {
      sid_set_t sids;
      std::vector<BYTE> acl_bytes;
      SECURITY_DESCRIPTOR descriptor {};
      if (!make_exact_acl(false, sids, acl_bytes) ||
          !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorOwner(
            &descriptor,
            static_cast<PSID>(sids.administrators.get()),
            FALSE) ||
          !SetSecurityDescriptorDacl(
            &descriptor,
            TRUE,
            reinterpret_cast<PACL>(acl_bytes.data()),
            FALSE) ||
          !SetSecurityDescriptorControl(
            &descriptor,
            SE_DACL_PROTECTED,
            SE_DACL_PROTECTED)) {
        return {};
      }
      SECURITY_ATTRIBUTES attributes {
        sizeof(attributes),
        &descriptor,
        FALSE,
      };
      return handle_t {CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0u,
        &attributes,
        CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_WRITE_THROUGH |
          FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    }

    bool append_journal_record(HANDLE journal, const pinned_entry_t &entry) {
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      const auto status = GetSecurityInfo(
        entry.handle.get(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &descriptor);
      local_memory_t storage {descriptor};
      if (status != ERROR_SUCCESS || descriptor == nullptr ||
          !IsValidSecurityDescriptor(descriptor)) {
        return false;
      }
      const auto descriptor_size = GetSecurityDescriptorLength(descriptor);
      const auto path_text = entry.path.wstring();
      if (descriptor_size == 0u || path_text.size() > 32760u) {
        return false;
      }
      journal_record_header_t header {
        kJournalRecordMagic,
        static_cast<std::uint32_t>(path_text.size()),
        descriptor_size,
        entry.directory ? 1u : 0u,
        entry.identity.volume_serial,
        entry.identity.file_id,
      };
      LARGE_INTEGER journal_size {};
      const auto additional_size = sizeof(header) +
        static_cast<std::uint64_t>(path_text.size()) * sizeof(wchar_t) +
        descriptor_size;
      if (!GetFileSizeEx(journal, &journal_size) || journal_size.QuadPart < 0 ||
          !journal_size_is_acceptable_impl(
            static_cast<std::uint64_t>(journal_size.QuadPart),
            additional_size)) {
        return false;
      }
      return write_all(journal, &header, sizeof(header)) &&
        write_all(
          journal,
          path_text.data(),
          static_cast<DWORD>(path_text.size() * sizeof(wchar_t))) &&
        write_all(journal, descriptor, descriptor_size) &&
        FlushFileBuffers(journal) != FALSE;
    }

    struct journal_record_t {
      std::filesystem::path path;
      bool directory {false};
      file_identity_internal_t identity;
      std::vector<BYTE> descriptor;
    };

    bool parse_journal(
      const std::vector<BYTE> &bytes,
      std::vector<journal_record_t> &records) {
      if (bytes.size() < sizeof(journal_header_t)) {
        return false;
      }
      journal_header_t header {};
      std::memcpy(&header, bytes.data(), sizeof(header));
      if (header.magic != kJournalMagic || header.version != 1u ||
          header.reserved != 0u) {
        return false;
      }
      std::size_t offset = sizeof(header);
      while (offset < bytes.size()) {
        if (bytes.size() - offset < sizeof(journal_record_header_t)) {
          break;
        }
        journal_record_header_t record_header {};
        std::memcpy(&record_header, bytes.data() + offset, sizeof(record_header));
        if (record_header.magic != kJournalRecordMagic ||
            record_header.path_characters == 0u ||
            record_header.path_characters > 32760u ||
            record_header.security_descriptor_bytes == 0u ||
            record_header.security_descriptor_bytes > 1024u * 1024u ||
            record_header.directory > 1u) {
          return false;
        }
        const auto path_bytes =
          static_cast<std::size_t>(record_header.path_characters) * sizeof(wchar_t);
        const auto payload_bytes = path_bytes +
          record_header.security_descriptor_bytes;
        offset += sizeof(record_header);
        if (payload_bytes > bytes.size() - offset) {
          break;
        }
        journal_record_t record;
        record.path = std::wstring {
          reinterpret_cast<const wchar_t *>(bytes.data() + offset),
          record_header.path_characters,
        };
        record.directory = record_header.directory != 0u;
        record.identity.volume_serial = record_header.volume_serial;
        record.identity.file_id = record_header.file_id;
        offset += path_bytes;
        record.descriptor.assign(
          bytes.begin() + static_cast<std::ptrdiff_t>(offset),
          bytes.begin() + static_cast<std::ptrdiff_t>(
            offset + record_header.security_descriptor_bytes));
        if (!IsValidSecurityDescriptor(record.descriptor.data()) ||
            GetSecurityDescriptorLength(record.descriptor.data()) !=
              record.descriptor.size()) {
          return false;
        }
        offset += record_header.security_descriptor_bytes;
        records.push_back(std::move(record));
      }
      return true;
    }

    bool restore_record(HANDLE handle, const journal_record_t &record) {
      auto *descriptor = static_cast<PSECURITY_DESCRIPTOR>(
        const_cast<BYTE *>(record.descriptor.data()));
      PSID owner = nullptr;
      BOOL defaulted = FALSE;
      PACL dacl = nullptr;
      BOOL present = FALSE;
      SECURITY_DESCRIPTOR_CONTROL control = 0u;
      DWORD revision = 0u;
      if (!GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) ||
          !GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) ||
          !present || dacl == nullptr ||
          !GetSecurityDescriptorControl(descriptor, &control, &revision)) {
        return false;
      }
      const auto protection = (control & SE_DACL_PROTECTED) != 0u ?
        PROTECTED_DACL_SECURITY_INFORMATION :
        UNPROTECTED_DACL_SECURITY_INFORMATION;
      return SetSecurityInfo(
               handle,
               SE_FILE_OBJECT,
               OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | protection,
               owner,
               nullptr,
               dacl,
               nullptr) == ERROR_SUCCESS;
    }

    bool descriptor_matches_record(HANDLE handle, const journal_record_t &record) {
      PSECURITY_DESCRIPTOR current = nullptr;
      const auto status = GetSecurityInfo(
        handle,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &current);
      local_memory_t current_storage {current};
      if (status != ERROR_SUCCESS || current == nullptr) {
        return false;
      }
      auto *expected = static_cast<PSECURITY_DESCRIPTOR>(
        const_cast<BYTE *>(record.descriptor.data()));
      PSID current_owner = nullptr;
      PSID expected_owner = nullptr;
      BOOL defaulted = FALSE;
      PACL current_dacl = nullptr;
      PACL expected_dacl = nullptr;
      BOOL present = FALSE;
      SECURITY_DESCRIPTOR_CONTROL current_control = 0u;
      SECURITY_DESCRIPTOR_CONTROL expected_control = 0u;
      DWORD revision = 0u;
      if (!GetSecurityDescriptorOwner(current, &current_owner, &defaulted) ||
          !GetSecurityDescriptorOwner(expected, &expected_owner, &defaulted) ||
          !EqualSid(current_owner, expected_owner) ||
          !GetSecurityDescriptorDacl(current, &present, &current_dacl, &defaulted) ||
          !present || current_dacl == nullptr ||
          !GetSecurityDescriptorDacl(expected, &present, &expected_dacl, &defaulted) ||
          !present || expected_dacl == nullptr ||
          !GetSecurityDescriptorControl(current, &current_control, &revision) ||
          !GetSecurityDescriptorControl(expected, &expected_control, &revision) ||
          ((current_control ^ expected_control) & SE_DACL_PROTECTED) != 0u) {
        return false;
      }
      ACL_SIZE_INFORMATION current_info {};
      ACL_SIZE_INFORMATION expected_info {};
      return GetAclInformation(
               current_dacl,
               &current_info,
               sizeof(current_info),
               AclSizeInformation) != FALSE &&
        GetAclInformation(
               expected_dacl,
               &expected_info,
               sizeof(expected_info),
               AclSizeInformation) != FALSE &&
        current_info.AclBytesInUse == expected_info.AclBytesInUse &&
        std::memcmp(
          current_dacl,
          expected_dacl,
          current_info.AclBytesInUse) == 0;
    }

    bool harden_tree_with_journal_impl(
      const std::filesystem::path &root,
      const std::filesystem::path &journal_path,
      std::size_t fail_after_mutations) {
      std::vector<pinned_entry_t> entries;
      if (!collect_pinned_tree(root, true, entries) || entries.empty()) {
        return false;
      }
      auto journal = create_secure_journal(journal_path);
      const journal_header_t header {kJournalMagic, 1u, 0u};
      if (!journal.valid() || !write_all(journal.get(), &header, sizeof(header)) ||
          !FlushFileBuffers(journal.get())) {
        return false;
      }
      for (auto &entry : entries) {
        if (!append_journal_record(journal.get(), entry)) {
          return false;
        }
      }
      LARGE_INTEGER final_journal_size {};
      if (!GetFileSizeEx(journal.get(), &final_journal_size) ||
          final_journal_size.QuadPart < 0 ||
          static_cast<std::uint64_t>(final_journal_size.QuadPart) >
            kMaximumJournalBytes ||
          !FlushFileBuffers(journal.get())) {
        return false;
      }
      std::size_t mutations = 0u;
      for (auto &entry : entries) {
        if (!apply_exact_security_handle(entry.handle.get(), entry.directory) ||
            !has_exact_security_handle(entry.handle.get(), entry.directory)) {
          return false;
        }
        ++mutations;
        if (mutations == fail_after_mutations) {
          return false;
        }
      }
      journal.reset();

      // Exact root/parent DACLs now prevent untrusted replacement. Release the
      // original pins, reopen every path, and compare stable volume/FileId.
      for (auto &entry : entries) {
        entry.handle.reset();
      }
      for (const auto &entry : entries) {
        auto reopened = open_security_handle(entry.path, entry.directory, true);
        FILE_ATTRIBUTE_TAG_INFO attributes {};
        file_identity_internal_t identity;
        if (!reopened.valid() ||
            !query_direct_attributes(reopened.get(), attributes) ||
            !query_identity(reopened.get(), identity) ||
            identity != entry.identity ||
            !query_single_link(reopened.get(), entry.directory) ||
            !has_exact_security_handle(reopened.get(), entry.directory)) {
          return false;
        }
      }
      return true;
    }

    bool verify_exact_tree_impl(const std::filesystem::path &root) {
      std::vector<pinned_entry_t> entries;
      if (!collect_pinned_tree(root, true, entries) || entries.empty()) {
        return false;
      }
      for (const auto &entry : entries) {
        if (!has_exact_security_handle(entry.handle.get(), entry.directory) ||
            !query_single_link(entry.handle.get(), entry.directory)) {
          return false;
        }
      }
      for (auto &entry : entries) {
        entry.handle.reset();
      }
      for (const auto &entry : entries) {
        auto reopened = open_security_handle(entry.path, entry.directory, true);
        FILE_ATTRIBUTE_TAG_INFO attributes {};
        file_identity_internal_t identity;
        if (!reopened.valid() || !query_direct_attributes(reopened.get(), attributes) ||
            !query_identity(reopened.get(), identity) ||
            identity != entry.identity ||
            !query_single_link(reopened.get(), entry.directory) ||
            !has_exact_security_handle(reopened.get(), entry.directory)) {
          return false;
        }
      }
      return validate_required_payloads(root);
    }

    bool restore_tree_from_journal_impl(
      const std::filesystem::path &journal_path,
      const std::optional<std::filesystem::path> &expected_root = std::nullopt) {
      std::vector<BYTE> bytes;
      std::vector<journal_record_t> records;
      if (!read_all_file(journal_path, bytes) || !parse_journal(bytes, records)) {
        return false;
      }
      if (expected_root.has_value()) {
        std::wstring canonical_root;
        if (!full_path(expected_root->wstring(), canonical_root)) {
          return false;
        }
        const auto prefix = canonical_root + L"\\";
        for (const auto &record : records) {
          std::wstring canonical_record;
          if (!full_path(record.path.wstring(), canonical_record) ||
              (!equal_ordinal(canonical_record, canonical_root) &&
               !starts_with_ordinal(canonical_record, prefix))) {
            return false;
          }
        }
      }
      std::vector<handle_t> pins;
      pins.reserve(records.size());
      for (const auto &record : records) {
        auto handle = open_security_handle(record.path, record.directory, true);
        FILE_ATTRIBUTE_TAG_INFO attributes {};
        file_identity_internal_t identity;
        if (!handle.valid() || !query_direct_attributes(handle.get(), attributes) ||
            !query_identity(handle.get(), identity) || identity != record.identity) {
          return false;
        }
        pins.push_back(std::move(handle));
      }
      // Restore ancestors first, then descendants. Any inheritance propagation
      // from an ancestor is overwritten by the descendant's own journal record.
      for (std::size_t index = 0u; index < records.size(); ++index) {
        if (!restore_record(pins[index].get(), records[index])) {
          return false;
        }
      }
      for (std::size_t index = 0u; index < records.size(); ++index) {
        if (!descriptor_matches_record(pins[index].get(), records[index])) {
          return false;
        }
      }
      return DeleteFileW(journal_path.c_str()) != FALSE;
    }

    std::wstring encode_custom_action_data_impl(
      std::wstring_view root,
      std::wstring_view journal) {
      return std::to_wstring(root.size()) + L":" + std::wstring {root} +
        std::wstring {journal};
    }

    bool decode_custom_action_data_impl(
      std::wstring_view encoded,
      std::wstring &root,
      std::wstring &journal) {
      const auto separator = encoded.find(L':');
      if (separator == std::wstring_view::npos || separator == 0u ||
          separator > 10u) {
        return false;
      }
      std::size_t root_size = 0u;
      for (std::size_t index = 0u; index < separator; ++index) {
        if (encoded[index] < L'0' || encoded[index] > L'9') {
          return false;
        }
        root_size = root_size * 10u +
          static_cast<std::size_t>(encoded[index] - L'0');
        if (root_size > 32760u) {
          return false;
        }
      }
      const auto payload = encoded.substr(separator + 1u);
      if (root_size == 0u || root_size >= payload.size()) {
        return false;
      }
      root.assign(payload.substr(0u, root_size));
      journal.assign(payload.substr(root_size));
      return !journal.empty();
    }

    void log_message(MSIHANDLE install, INSTALLMESSAGE kind, std::wstring_view message) {
      const auto record = MsiCreateRecord(1u);
      if (record == 0u) {
        return;
      }
      MsiRecordSetStringW(record, 0u, L"Vibepollo install-tree security: [1]");
      MsiRecordSetStringW(record, 1u, std::wstring {message}.c_str());
      MsiProcessMessage(install, kind, record);
      MsiCloseHandle(record);
    }

    bool read_msi_property(
      MSIHANDLE install,
      const wchar_t *name,
      std::wstring &value) {
      DWORD size = 0u;
      wchar_t empty[1] {};
      auto status = MsiGetPropertyW(install, name, empty, &size);
      if (status == ERROR_SUCCESS && size == 0u) {
        value.clear();
        return true;
      }
      if (status != ERROR_MORE_DATA) {
        return false;
      }
      std::wstring buffer(static_cast<std::size_t>(size) + 1u, L'\0');
      auto capacity = static_cast<DWORD>(buffer.size());
      status = MsiGetPropertyW(install, name, buffer.data(), &capacity);
      if (status != ERROR_SUCCESS) {
        return false;
      }
      buffer.resize(capacity);
      value = std::move(buffer);
      return true;
    }

    bool read_msi_target_path(
      MSIHANDLE install,
      const wchar_t *directory,
      std::wstring &value) {
      DWORD size = 0u;
      wchar_t empty[1] {};
      auto status = MsiGetTargetPathW(install, directory, empty, &size);
      if (status != ERROR_MORE_DATA && status != ERROR_SUCCESS) {
        return false;
      }
      std::wstring buffer(static_cast<std::size_t>(size) + 2u, L'\0');
      auto capacity = static_cast<DWORD>(buffer.size());
      status = MsiGetTargetPathW(
        install,
        directory,
        buffer.data(),
        &capacity);
      if (status != ERROR_SUCCESS) {
        return false;
      }
      buffer.resize(capacity);
      value = std::move(buffer);
      return true;
    }

    std::optional<std::wstring> known_folder(REFKNOWNFOLDERID folder) {
      PWSTR raw = nullptr;
      const auto status = SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &raw);
      if (FAILED(status) || raw == nullptr) {
        if (raw != nullptr) {
          CoTaskMemFree(raw);
        }
        return std::nullopt;
      }
      std::wstring value {raw};
      CoTaskMemFree(raw);
      return value;
    }

    bool canonical_runtime_roots(
      std::wstring_view candidate,
      std::wstring &program_files,
      std::wstring &root) {
      static_assert(sizeof(void *) == 8u, "The MSI security custom action must be x64");
      const auto known_program_files = known_folder(FOLDERID_ProgramFilesX64);
      if (!known_program_files.has_value() ||
          !full_path(*known_program_files, program_files) ||
          !normalize_program_files_descendant_impl(program_files, candidate, root)) {
        return false;
      }
      wchar_t volume[MAX_PATH] {};
      return GetVolumePathNameW(program_files.c_str(), volume, MAX_PATH) != FALSE &&
        GetDriveTypeW(volume) == DRIVE_FIXED;
    }

    handle_t open_ancestor_validation_handle(const std::filesystem::path &path) {
      return handle_t {CreateFileW(
        path.c_str(),
        GENERIC_READ | READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
    }

    bool handle_final_path_matches(HANDLE handle, const std::filesystem::path &expected) {
      const auto required = GetFinalPathNameByHandleW(
        handle,
        nullptr,
        0u,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (required == 0u || required > 32768u) {
        return false;
      }
      std::wstring final_path(required, L'\0');
      const auto written = GetFinalPathNameByHandleW(
        handle,
        final_path.data(),
        static_cast<DWORD>(final_path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
      if (written == 0u || written >= final_path.size()) {
        return false;
      }
      final_path.resize(written);
      if (final_path.starts_with(L"\\\\?\\")) {
        final_path.erase(0u, 4u);
      }
      std::wstring canonical_expected;
      std::wstring canonical_final;
      return full_path(expected.wstring(), canonical_expected) &&
        full_path(final_path, canonical_final) &&
        equal_ordinal(canonical_expected, canonical_final);
    }

    bool validate_existing_ancestors(
      const std::wstring &trusted_boundary,
      const std::wstring &root) {
      std::wstring canonical_boundary;
      std::wstring canonical_root;
      wchar_t volume_path[MAX_PATH] {};
      wchar_t filesystem_name[MAX_PATH] {};
      DWORD filesystem_flags = 0u;
      if (!full_path(trusted_boundary, canonical_boundary) ||
          !full_path(root, canonical_root) ||
          (!equal_ordinal(canonical_root, canonical_boundary) &&
           !starts_with_ordinal(canonical_root, canonical_boundary + L"\\")) ||
          GetVolumePathNameW(
            canonical_boundary.c_str(),
            volume_path,
            MAX_PATH) == FALSE ||
          GetDriveTypeW(volume_path) != DRIVE_FIXED ||
          GetVolumeInformationW(
            volume_path,
            nullptr,
            0u,
            nullptr,
            nullptr,
            &filesystem_flags,
            filesystem_name,
            MAX_PATH) == FALSE ||
          (filesystem_flags & FILE_PERSISTENT_ACLS) == 0u) {
        return false;
      }
      std::vector<std::filesystem::path> paths;
      auto cursor = std::filesystem::path {canonical_root};
      const auto boundary = std::filesystem::path {volume_path};
      for (;;) {
        paths.push_back(cursor);
        if (equal_ordinal(cursor.wstring(), boundary.wstring())) {
          break;
        }
        const auto parent = cursor.parent_path();
        if (parent.empty() || equal_ordinal(parent.wstring(), cursor.wstring())) {
          return false;
        }
        cursor = parent;
      }
      std::ranges::reverse(paths);
      std::vector<handle_t> pins;
      bool validated_trusted_boundary = false;
      const auto trusted_prefix = canonical_boundary + L"\\";
      for (const auto &path : paths) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
          if (GetLastError() == ERROR_FILE_NOT_FOUND ||
              GetLastError() == ERROR_PATH_NOT_FOUND) {
            continue;
          }
          return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
          return false;
        }
        auto pin = open_ancestor_validation_handle(path);
        FILE_ATTRIBUTE_TAG_INFO tag_info {};
        const auto path_text = path.wstring();
        const bool at_or_below_trusted_boundary =
          equal_ordinal(path_text, canonical_boundary) ||
          starts_with_ordinal(path_text, trusted_prefix);
        constexpr ACCESS_MASK namespace_replacement_danger =
          DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
        if (!pin.valid() || !query_direct_attributes(pin.get(), tag_info) ||
            !query_safe_security(
              pin.get(),
              true,
              at_or_below_trusted_boundary ? 0u :
                namespace_replacement_danger,
              at_or_below_trusted_boundary) ||
            !handle_final_path_matches(pin.get(), path)) {
          return false;
        }
        if (equal_ordinal(path_text, canonical_boundary)) {
          validated_trusted_boundary = true;
        }
        pins.push_back(std::move(pin));
      }
      return validated_trusted_boundary;
    }

    bool validate_required_payloads(const std::filesystem::path &root) {
      constexpr std::array<std::wstring_view, 3u> required {
        L"sunshine.exe",
        L"tools\\sunshinesvc.exe",
        L"tools\\sunshine_audio_policy_helper.exe",
      };
      std::vector<handle_t> pins;
      for (const auto relative : required) {
        const auto path = root / std::wstring {relative};
        auto pin = open_security_handle(path, false, true);
        FILE_ATTRIBUTE_TAG_INFO attributes {};
        if (!pin.valid() || !query_direct_attributes(pin.get(), attributes) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
            !query_single_link(pin.get(), false) ||
            !query_safe_security(pin.get(), false)) {
          return false;
        }
        pins.push_back(std::move(pin));
      }
      return true;
    }

    std::optional<std::wstring> installed_component_root_for_product(
      std::wstring_view product_code_value) {
      constexpr auto component_code =
        L"{CBA8A3D9-5A2A-4F76-9923-5B23D1EDFBB6}";
      const std::wstring product_code {product_code_value};
      GUID parsed_product_code {};
      if (product_code.empty() ||
          FAILED(CLSIDFromString(product_code.c_str(), &parsed_product_code))) {
        return std::nullopt;
      }
      DWORD size = 0u;
      wchar_t empty[1] {};
      auto state = MsiGetComponentPathW(
        product_code.c_str(),
        component_code,
        empty,
        &size);
      if (state != INSTALLSTATE_MOREDATA && state != INSTALLSTATE_LOCAL) {
        return std::nullopt;
      }
      std::wstring path(static_cast<std::size_t>(size) + 2u, L'\0');
      auto capacity = static_cast<DWORD>(path.size());
      state = MsiGetComponentPathW(
        product_code.c_str(),
        component_code,
        path.data(),
        &capacity);
      if (state != INSTALLSTATE_LOCAL) {
        return std::nullopt;
      }
      path.resize(capacity);
      const auto service_path = std::filesystem::path {path};
      const auto tools = service_path.parent_path();
      const auto root = tools.parent_path();
      if (root.empty() || tools.filename() != L"tools" ||
          !equal_ordinal(service_path.filename().wstring(), L"sunshinesvc.exe")) {
        return std::nullopt;
      }
      return root.wstring();
    }

    std::optional<std::wstring> installed_component_root(MSIHANDLE install) {
      std::wstring product_code;
      if (!read_msi_property(install, L"ProductCode", product_code)) {
        return std::nullopt;
      }
      return installed_component_root_for_product(product_code);
    }

    bool validate_upgrade_source_trees(MSIHANDLE install) {
      std::wstring related_products;
      if (!read_msi_property(
            install,
            L"WIX_UPGRADE_DETECTED",
            related_products) ||
          related_products.empty()) {
        return false;
      }
      bool found = false;
      std::size_t start = 0u;
      while (start <= related_products.size()) {
        const auto end = related_products.find(L';', start);
        auto product_code = related_products.substr(
          start,
          end == std::wstring::npos ? std::wstring::npos : end - start);
        product_code.erase(0u, product_code.find_first_not_of(L" \t"));
        const auto last = product_code.find_last_not_of(L" \t");
        if (last != std::wstring::npos) {
          product_code.resize(last + 1u);
        }
        if (product_code.empty()) {
          if (end == std::wstring::npos) {
            break;
          }
          start = end + 1u;
          continue;
        }
        const auto outgoing_root =
          installed_component_root_for_product(product_code);
        std::wstring program_files;
        std::wstring root;
        if (!outgoing_root.has_value() ||
            !canonical_runtime_roots(*outgoing_root, program_files, root) ||
            !validate_existing_ancestors(program_files, root) ||
            !preflight_existing_tree_impl(root)) {
          return false;
        }
        found = true;
        if (end == std::wstring::npos) {
          break;
        }
        start = end + 1u;
      }
      return found;
    }

    struct operation_state_t {
      bool protect_required {false};
      bool full_remove {false};
    };

    std::optional<operation_state_t> classify_operation_state_impl(
      std::wstring_view remove_value,
      int product_feature_action,
      int extras_feature_action,
      int service_component_action) {
      std::wstring remove {remove_value};
      std::ranges::transform(
        remove,
        remove.begin(),
        [](wchar_t character) {
          return static_cast<wchar_t>(std::towupper(character));
        });
      bool remove_all = false;
      bool remove_product_feature = false;
      bool remove_extras_feature = false;
      std::size_t start = 0u;
      while (start <= remove.size()) {
        const auto end = remove.find(L',', start);
        auto token = remove.substr(
          start,
          end == std::wstring::npos ? std::wstring::npos : end - start);
        token.erase(0u, token.find_first_not_of(L" \t"));
        const auto last = token.find_last_not_of(L" \t");
        if (last != std::wstring::npos) {
          token.resize(last + 1u);
        }
        remove_all |= token == L"ALL";
        remove_product_feature |= token == L"PRODUCTFEATURE";
        remove_extras_feature |= token == L"VIBEPOLLOEXTRAS";
        if (end == std::wstring::npos) {
          break;
        }
        start = end + 1u;
      }
      const auto state_is_actionable = [](int state) {
        return state == INSTALLSTATE_ABSENT ||
          state == INSTALLSTATE_LOCAL ||
          state == INSTALLSTATE_SOURCE ||
          state == INSTALLSTATE_DEFAULT;
      };
      if (!state_is_actionable(product_feature_action) ||
          !state_is_actionable(extras_feature_action) ||
          !state_is_actionable(service_component_action)) {
        return std::nullopt;
      }
      const bool full_remove_requested = remove_all ||
        (remove_product_feature && remove_extras_feature);
      const bool all_payload_states_absent =
        product_feature_action == INSTALLSTATE_ABSENT &&
        extras_feature_action == INSTALLSTATE_ABSENT &&
        service_component_action == INSTALLSTATE_ABSENT;
      if (full_remove_requested && !all_payload_states_absent) {
        return std::nullopt;
      }
      return operation_state_t {
        .protect_required =
          product_feature_action != INSTALLSTATE_ABSENT ||
          extras_feature_action != INSTALLSTATE_ABSENT ||
          service_component_action != INSTALLSTATE_ABSENT,
        .full_remove = full_remove_requested && all_payload_states_absent,
      };
    }

    std::optional<operation_state_t> read_operation_state(MSIHANDLE install) {
      std::wstring remove;
      INSTALLSTATE product_installed = INSTALLSTATE_UNKNOWN;
      INSTALLSTATE product_action = INSTALLSTATE_UNKNOWN;
      INSTALLSTATE extras_installed = INSTALLSTATE_UNKNOWN;
      INSTALLSTATE extras_action = INSTALLSTATE_UNKNOWN;
      INSTALLSTATE service_installed = INSTALLSTATE_UNKNOWN;
      INSTALLSTATE service_action = INSTALLSTATE_UNKNOWN;
      if (!read_msi_property(install, L"REMOVE", remove) ||
          MsiGetFeatureStateW(
            install,
            L"ProductFeature",
            &product_installed,
            &product_action) != ERROR_SUCCESS ||
          MsiGetFeatureStateW(
            install,
            L"VibepolloExtras",
            &extras_installed,
            &extras_action) != ERROR_SUCCESS ||
          MsiGetComponentStateW(
            install,
            L"ApolloSvc",
            &service_installed,
            &service_action) != ERROR_SUCCESS) {
        return std::nullopt;
      }
      return classify_operation_state_impl(
        remove,
        static_cast<int>(product_action),
        static_cast<int>(extras_action),
        static_cast<int>(service_action));
    }

    std::optional<std::wstring> new_journal_path() {
      const auto windows = known_folder(FOLDERID_Windows);
      GUID guid {};
      std::array<wchar_t, 40u> guid_text {};
      if (!windows.has_value() || FAILED(CoCreateGuid(&guid)) ||
          StringFromGUID2(guid, guid_text.data(), guid_text.size()) == 0) {
        return std::nullopt;
      }
      return (std::filesystem::path {*windows} / L"SystemTemp" /
              (L"VibepolloInstallerSecurity-" + std::wstring {guid_text.data()} +
               L".journal"))
        .wstring();
    }

    bool validate_journal_path(std::wstring_view candidate) {
      const auto windows_value = known_folder(FOLDERID_Windows);
      std::wstring system_temp;
      std::wstring journal;
      if (!windows_value.has_value() ||
          !full_path(
            (std::filesystem::path {*windows_value} / L"SystemTemp").wstring(),
            system_temp) ||
          !full_path(candidate, journal)) {
        return false;
      }
      const auto path = std::filesystem::path {journal};
      if (!equal_ordinal(path.parent_path().wstring(), system_temp) ||
          !path.filename().wstring().starts_with(L"VibepolloInstallerSecurity-") ||
          path.extension() != L".journal") {
        return false;
      }
      std::wstring windows;
      if (!full_path(*windows_value, windows)) {
        return false;
      }
      return validate_existing_ancestors(windows, system_temp);
    }

    bool set_prepared_properties(
      MSIHANDLE install,
      const std::wstring &encoded) {
      constexpr std::array<const wchar_t *, 3u> actions {
        L"RollbackAudioPolicyInstallTreeSecurity",
        L"ProtectAudioPolicyInstallTreeSecurity",
        L"CommitAudioPolicyInstallTreeSecurity",
      };
      for (const auto action : actions) {
        if (MsiSetPropertyW(install, action, encoded.c_str()) != ERROR_SUCCESS) {
          return false;
        }
      }
      return true;
    }

    bool clear_prepared_properties(MSIHANDLE install) {
      constexpr std::array<const wchar_t *, 7u> properties {
        L"RollbackAudioPolicyInstallTreeSecurity",
        L"ProtectAudioPolicyInstallTreeSecurity",
        L"CommitAudioPolicyInstallTreeSecurity",
        L"VerifyAudioPolicyUninstallTreeSecurity",
        L"AudioPolicyTrustedInstallRoot",
        L"AudioPolicyProtectRequired",
        L"AudioPolicyFullRemove",
      };
      for (const auto property : properties) {
        if (MsiSetPropertyW(install, property, L"") != ERROR_SUCCESS) {
          return false;
        }
      }
      return true;
    }

    bool is_installed(MSIHANDLE install) {
      std::wstring installed;
      return read_msi_property(install, L"Installed", installed) &&
        !installed.empty();
    }

    UINT prepare_action(MSIHANDLE install) {
      const bool maintenance = is_installed(install);
      if (!clear_prepared_properties(install)) {
        return ERROR_INSTALL_FAILURE;
      }
      const auto operation = read_operation_state(install);
      if (!operation.has_value()) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"MSI feature/component action states are missing or contradict REMOVE.");
        return ERROR_INSTALL_FAILURE;
      }
      const auto fail = [&](std::wstring_view reason) -> UINT {
        log_message(install, INSTALLMESSAGE_ERROR, reason);
        return ERROR_INSTALL_FAILURE;
      };

      if (operation->protect_required &&
          !MsiGetMode(install, MSIRUNMODE_ROLLBACKENABLED)) {
        return fail(L"Windows Installer rollback is disabled.");
      }
      std::wstring candidate;
      std::wstring target_path;
      if (!read_msi_target_path(install, L"INSTALL_ROOT", target_path)) {
        return ERROR_INSTALL_FAILURE;
      }
      if (maintenance) {
        const auto installed_root = installed_component_root(install);
        if (!installed_root.has_value()) {
          log_message(
            install,
            INSTALLMESSAGE_ERROR,
            L"The registered Apollo service component path could not be resolved.");
          return ERROR_INSTALL_FAILURE;
        }
        candidate = *installed_root;
      } else {
        candidate = target_path;
      }

      std::wstring program_files;
      std::wstring root;
      std::wstring target_program_files;
      std::wstring target_root;
      if (!canonical_runtime_roots(candidate, program_files, root) ||
          !canonical_runtime_roots(target_path, target_program_files, target_root) ||
          (maintenance && !equal_ordinal(root, target_root)) ||
          !validate_existing_ancestors(program_files, root) ||
          !preflight_existing_tree_impl(root)) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"The install root is not a direct, fixed-local 64-bit Program Files descendant.");
        return ERROR_INSTALL_FAILURE;
      }
      if (operation->full_remove && !verify_exact_tree_impl(root)) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"The registered install tree is not structurally safe for removal.");
        return ERROR_INSTALL_FAILURE;
      }
      if (operation->full_remove) {
        if (MsiSetPropertyW(
              install,
              L"VerifyAudioPolicyUninstallTreeSecurity",
              root.c_str()) != ERROR_SUCCESS ||
            MsiSetPropertyW(
              install,
              L"AudioPolicyTrustedInstallRoot",
              L"1") != ERROR_SUCCESS ||
            MsiSetPropertyW(
              install,
              L"AudioPolicyProtectRequired",
              L"0") != ERROR_SUCCESS ||
            MsiSetPropertyW(
              install,
              L"AudioPolicyFullRemove",
              L"1") != ERROR_SUCCESS) {
          return ERROR_INSTALL_FAILURE;
        }
        return ERROR_SUCCESS;
      }
      if (operation->protect_required) {
        const auto journal = new_journal_path();
        if (!journal.has_value() ||
            !set_prepared_properties(
              install,
              encode_custom_action_data_impl(root, *journal))) {
          return fail(L"Secure CustomActionData could not be prepared.");
        }
      }
      if (MsiSetPropertyW(
            install,
            L"AudioPolicyTrustedInstallRoot",
            L"1") != ERROR_SUCCESS ||
          MsiSetPropertyW(
            install,
            L"AudioPolicyProtectRequired",
            operation->protect_required ? L"1" : L"0") != ERROR_SUCCESS ||
          MsiSetPropertyW(
            install,
            L"AudioPolicyFullRemove",
            L"0") != ERROR_SUCCESS) {
        return fail(L"Canonical MSI operation-state properties could not be published.");
      }
      log_message(install, INSTALLMESSAGE_INFO, L"Native install-tree preflight passed.");
      return ERROR_SUCCESS;
    }

    UINT prepare_upgrade_action(MSIHANDLE install) {
      if (!validate_upgrade_source_trees(install)) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"An outgoing product tree is not safe for transactional replacement.");
        return ERROR_INSTALL_FAILURE;
      }
      return prepare_action(install);
    }

    bool read_deferred_data(
      MSIHANDLE install,
      std::wstring &root,
      std::wstring &journal) {
      std::wstring encoded;
      return read_msi_property(install, L"CustomActionData", encoded) &&
        decode_custom_action_data_impl(encoded, root, journal);
    }

    UINT protect_action(MSIHANDLE install) {
      std::wstring root_data;
      std::wstring journal_data;
      std::wstring program_files;
      std::wstring root;
      if (!read_deferred_data(install, root_data, journal_data) ||
          !canonical_runtime_roots(root_data, program_files, root) ||
          !equal_ordinal(root_data, root) ||
          !validate_journal_path(journal_data) ||
          !validate_existing_ancestors(program_files, root) ||
          !validate_required_payloads(root) ||
          !harden_tree_with_journal_impl(root, journal_data, SIZE_MAX)) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"Installed payload validation or transactional hardening failed.");
        return ERROR_INSTALL_FAILURE;
      }
      log_message(
        install,
        INSTALLMESSAGE_INFO,
        L"Installed payload identities and exact protected DACLs were verified.");
      return ERROR_SUCCESS;
    }

    UINT verify_uninstall_action(MSIHANDLE install) {
      std::wstring root_data;
      std::wstring program_files;
      std::wstring root;
      if (!read_msi_property(install, L"CustomActionData", root_data) ||
          !canonical_runtime_roots(root_data, program_files, root) ||
          !equal_ordinal(root_data, root) ||
          !validate_existing_ancestors(program_files, root) ||
          !verify_exact_tree_impl(root)) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"The installed cleanup payload tree changed after preflight.");
        return ERROR_INSTALL_FAILURE;
      }
      return ERROR_SUCCESS;
    }

    UINT rollback_action(MSIHANDLE install) {
      std::wstring root;
      std::wstring journal;
      if (!read_deferred_data(install, root, journal) ||
          !validate_journal_path(journal)) {
        return ERROR_INSTALL_FAILURE;
      }
      const auto attributes = GetFileAttributesW(journal.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES &&
          (GetLastError() == ERROR_FILE_NOT_FOUND ||
           GetLastError() == ERROR_PATH_NOT_FOUND)) {
        return ERROR_SUCCESS;
      }
      if (!restore_tree_from_journal_impl(journal, std::filesystem::path {root})) {
        log_message(
          install,
          INSTALLMESSAGE_ERROR,
          L"The install-tree security rollback journal could not be restored.");
        return ERROR_INSTALL_FAILURE;
      }
      return ERROR_SUCCESS;
    }

    UINT commit_action(MSIHANDLE install) {
      std::wstring root;
      std::wstring journal;
      if (!read_deferred_data(install, root, journal) ||
          !validate_journal_path(journal)) {
        return ERROR_INSTALL_FAILURE;
      }
      if (DeleteFileW(journal.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND ||
          GetLastError() == ERROR_PATH_NOT_FOUND) {
        return ERROR_SUCCESS;
      }
      log_message(
        install,
        INSTALLMESSAGE_ERROR,
        L"The committed install-tree security journal could not be removed.");
      return ERROR_INSTALL_FAILURE;
    }

  }

  namespace testing {
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
      int service_component_action) {
      const auto state = classify_operation_state_impl(
        remove,
        product_feature_action,
        extras_feature_action,
        service_component_action);
      if (!state.has_value()) {
        return std::nullopt;
      }
      return operation_state_t {
        .protect_required = state->protect_required,
        .full_remove = state->full_remove,
      };
    }

    bool normalize_program_files_descendant(
      std::wstring_view program_files,
      std::wstring_view candidate,
      std::wstring &normalized) {
      return normalize_program_files_descendant_impl(
        program_files,
        candidate,
        normalized);
    }

    bool security_descriptor_is_safe(
      PSECURITY_DESCRIPTOR descriptor,
      bool is_ancestor) {
      return security_descriptor_is_safe_impl(
        descriptor,
        is_ancestor,
        0u,
        is_ancestor);
    }

    bool attributes_are_direct(const FILE_ATTRIBUTE_TAG_INFO &attributes) {
      return attributes_are_direct_impl(attributes);
    }

    bool tree_contains_reparse_point(const std::filesystem::path &root) {
      return tree_contains_reparse_point_impl(root);
    }

    bool apply_exact_security(const std::filesystem::path &path, bool directory) {
      auto handle = open_security_handle(path, directory, false);
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      return handle.valid() && query_direct_attributes(handle.get(), attributes) &&
        apply_exact_security_handle(handle.get(), directory);
    }

    bool has_exact_security(const std::filesystem::path &path, bool directory) {
      auto handle = open_security_handle(path, directory, false);
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      return handle.valid() && query_direct_attributes(handle.get(), attributes) &&
        has_exact_security_handle(handle.get(), directory);
    }

    std::optional<file_identity_t> capture_identity(
      const std::filesystem::path &path,
      bool directory) {
      const auto flags = FILE_FLAG_OPEN_REPARSE_POINT |
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0u);
      handle_t handle {CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr)};
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      FILE_ID_INFO identity {};
      if (!handle.valid() || !query_direct_attributes(handle.get(), attributes) ||
          !GetFileInformationByHandleEx(
            handle.get(),
            FileIdInfo,
            &identity,
            sizeof(identity))) {
        return std::nullopt;
      }
      file_identity_t result;
      result.volume_serial = identity.VolumeSerialNumber;
      std::copy_n(
        identity.FileId.Identifier,
        result.file_id.size(),
        result.file_id.begin());
      return result;
    }

    bool identity_matches(
      const std::filesystem::path &path,
      bool directory,
      const file_identity_t &expected) {
      const auto current = capture_identity(path, directory);
      return current.has_value() &&
        current->volume_serial == expected.volume_serial &&
        current->file_id == expected.file_id;
    }

    bool is_single_link_file(const std::filesystem::path &path) {
      handle_t handle {CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr)};
      BY_HANDLE_FILE_INFORMATION info {};
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      return handle.valid() && query_direct_attributes(handle.get(), attributes) &&
        GetFileInformationByHandle(handle.get(), &info) &&
        info.nNumberOfLinks == 1u;
    }

    bool is_exclusively_pinnable(
      const std::filesystem::path &path,
      bool directory) {
      auto handle = open_security_handle(path, directory, true);
      FILE_ATTRIBUTE_TAG_INFO attributes {};
      return handle.valid() && query_direct_attributes(handle.get(), attributes);
    }

    std::wstring encode_custom_action_data(
      std::wstring_view root,
      std::wstring_view journal) {
      return encode_custom_action_data_impl(root, journal);
    }

    bool decode_custom_action_data(
      std::wstring_view encoded,
      std::wstring &root,
      std::wstring &journal) {
      return decode_custom_action_data_impl(encoded, root, journal);
    }

    bool harden_tree_with_journal(
      const std::filesystem::path &root,
      const std::filesystem::path &journal,
      std::size_t fail_after_mutations) {
      return harden_tree_with_journal_impl(root, journal, fail_after_mutations);
    }

    bool restore_tree_from_journal(const std::filesystem::path &journal) {
      return restore_tree_from_journal_impl(journal);
    }

    bool validate_native_program_files_ancestry(
      const std::filesystem::path &candidate) {
      const auto program_files = known_folder(FOLDERID_ProgramFilesX64);
      std::wstring canonical_program_files;
      std::wstring canonical_candidate;
      return program_files.has_value() &&
        full_path(*program_files, canonical_program_files) &&
        normalize_program_files_descendant_impl(
          canonical_program_files,
          candidate.wstring(),
          canonical_candidate) &&
        validate_existing_ancestors(
          canonical_program_files,
          canonical_candidate);
    }

    bool preflight_existing_tree(const std::filesystem::path &root) {
      return preflight_existing_tree_impl(root);
    }

    bool journal_size_is_acceptable(
      std::uint64_t current_bytes,
      std::uint64_t additional_bytes) {
      return journal_size_is_acceptable_impl(current_bytes, additional_bytes);
    }
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
PrepareAudioPolicyInstallTreeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::prepare_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
PrepareAudioPolicyUpgradeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::prepare_upgrade_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
ProtectAudioPolicyInstallTreeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::protect_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
VerifyAudioPolicyUninstallTreeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::verify_uninstall_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
RollbackAudioPolicyInstallTreeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::rollback_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}

extern "C" __declspec(dllexport) UINT __stdcall
CommitAudioPolicyInstallTreeSecurity(MSIHANDLE install) {
  try {
    return sunshine::installer_security::commit_action(install);
  } catch (...) {
    return ERROR_INSTALL_FAILURE;
  }
}
