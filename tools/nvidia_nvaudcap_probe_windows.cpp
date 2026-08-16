#include "tools/nvidia_nvaudcap_probe.h"

#include <windows.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _MSC_VER
  #pragma comment(lib, "wintrust.lib")
  #pragma comment(lib, "version.lib")
#endif

namespace {
  using nvidia_nvaudcap_probe::address_range;
  using nvidia_nvaudcap_probe::module_image;
  using nvidia_nvaudcap_probe::probe_observation;

  class module_handle {
  public:
    explicit module_handle(HMODULE value = nullptr): value_(value) {}
    module_handle(const module_handle &) = delete;
    module_handle &operator=(const module_handle &) = delete;
    ~module_handle() {
      if (value_ != nullptr) {
        FreeLibrary(value_);
      }
    }
    HMODULE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

  private:
    HMODULE value_;
  };

  std::string windows_error(const DWORD value) {
    std::error_code error {static_cast<int>(value), std::system_category()};
    return error.message();
  }

  std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX)) {
      return {};
    }
    const int size = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      value.data(),
      static_cast<int>(value.size()),
      nullptr,
      0,
      nullptr,
      nullptr);
    if (size <= 0) {
      return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          value.data(),
          static_cast<int>(value.size()),
          result.data(),
          size,
          nullptr,
          nullptr) != size) {
      return {};
    }
    return result;
  }

  std::filesystem::path nvaudcap_system32_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
      const UINT length = GetSystemDirectoryW(
        buffer.data(),
        static_cast<UINT>(buffer.size()));
      if (length == 0) {
        return {};
      }
      if (length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path {buffer} / L"nvaudcap64v.dll";
      }
      buffer.resize(static_cast<std::size_t>(length) + 1U);
    }
  }

  LONG verify_cached_authenticode(const std::filesystem::path &path) {
    WINTRUST_FILE_INFO file_info {};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    WINTRUST_DATA trust_data {};
    trust_data.cbStruct = sizeof(trust_data);
    trust_data.dwUIChoice = WTD_UI_NONE;
    trust_data.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust_data.dwUnionChoice = WTD_CHOICE_FILE;
    trust_data.pFile = &file_info;
    trust_data.dwStateAction = WTD_STATEACTION_VERIFY;
    trust_data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL |
                             WTD_REVOCATION_CHECK_NONE;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(
      reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
      &action,
      &trust_data);
    trust_data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(
      reinterpret_cast<HWND>(INVALID_HANDLE_VALUE),
      &action,
      &trust_data);
    return status;
  }

  std::string file_version(const std::filesystem::path &path) {
    DWORD ignored {};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
      return {};
    }
    std::vector<std::byte> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) {
      return {};
    }
    VS_FIXEDFILEINFO *info {};
    UINT info_size {};
    if (!VerQueryValueW(
          bytes.data(),
          L"\\",
          reinterpret_cast<void **>(&info),
          &info_size) ||
        info == nullptr || info_size < sizeof(*info)) {
      return {};
    }
    std::ostringstream result;
    result << HIWORD(info->dwFileVersionMS) << '.'
           << LOWORD(info->dwFileVersionMS) << '.'
           << HIWORD(info->dwFileVersionLS) << '.'
           << LOWORD(info->dwFileVersionLS);
    return result.str();
  }

  bool inspect_module_image(HMODULE module, module_image &image) {
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
      return false;
    }
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
      base + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return false;
    }
    image.base = base;
    image.size = nt->OptionalHeader.SizeOfImage;
    const auto *sections = IMAGE_FIRST_SECTION(nt);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
      const auto &section = sections[index];
      if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
        continue;
      }
      const auto size = std::max(
        section.Misc.VirtualSize,
        section.SizeOfRawData);
      image.executable_ranges.push_back(address_range {
        .begin = base + section.VirtualAddress,
        .end = base + section.VirtualAddress + size,
      });
    }
    return image.size != 0 && !image.executable_ranges.empty();
  }
}  // namespace

namespace nvidia_nvaudcap_probe {
  probe_observation collect_windows_observation() {
    probe_observation observation {};
    observation.table = make_controller_interface_table();

    const auto path = nvaudcap_system32_path();
    observation.dll_path = wide_to_utf8(path.native());
    if (path.empty()) {
      observation.errors.emplace_back(
        "GetSystemDirectoryW failed: " + windows_error(GetLastError()));
      return observation;
    }
    observation.file_version = file_version(path);

    const LONG signature_status = verify_cached_authenticode(path);
    observation.signature_status = static_cast<std::uint32_t>(signature_status);
    observation.signature_valid = signature_status == ERROR_SUCCESS;
    if (!observation.signature_valid) {
      observation.errors.emplace_back("Authenticode verification failed");
      return observation;
    }

    module_handle module {LoadLibraryExW(
      path.c_str(),
      nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    if (!module) {
      observation.errors.emplace_back(
        "LoadLibraryExW failed: " + windows_error(GetLastError()));
      return observation;
    }
    observation.module_base = reinterpret_cast<std::uintptr_t>(module.get());

    module_image image;
    if (!inspect_module_image(module.get(), image)) {
      observation.errors.emplace_back("loaded module has an invalid PE image");
      return observation;
    }

    const auto raw_factory = GetProcAddress(
      module.get(),
      "NvAudCapAPICreateInstance");
    if (raw_factory == nullptr) {
      observation.errors.emplace_back(
        "GetProcAddress failed: " + windows_error(GetLastError()));
      return observation;
    }
    observation.factory_address = reinterpret_cast<std::uintptr_t>(raw_factory);

    using create_instance_fn = std::int32_t (WINAPI *)(interface_table *);
    static_assert(sizeof(create_instance_fn) == sizeof(raw_factory));
    create_instance_fn create_instance {};
    std::memcpy(&create_instance, &raw_factory, sizeof(create_instance));
    observation.factory_result = create_instance(&observation.table);
    if (observation.factory_result != 0) {
      observation.errors.emplace_back("NvAudCapAPICreateInstance returned an error");
      return observation;
    }

    observation.validation = validate_interface_table(observation.table, image);
    if (!observation.validation.valid) {
      observation.errors.emplace_back(
        "factory returned a null, outside-module, or non-executable method pointer");
      return observation;
    }
    observation.complete = true;
    return observation;
  }
}  // namespace nvidia_nvaudcap_probe
