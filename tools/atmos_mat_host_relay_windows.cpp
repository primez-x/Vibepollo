#include "tools/atmos_mat_host_relay.h"

#ifdef _WIN32
#include <windows.h>

#include <algorithm>
#include <atomic>

namespace atmos_mat_host {
  namespace {
    // Field-for-field public user-mode projection of mat10tapabi.h. Keep this
    // pointer-free and packed; kernel headers are intentionally not included.
#pragma pack(push, 1)
    struct tap_request {
      std::uint32_t magic; std::uint16_t major, minor, header_bytes, reserved0;
      std::uint32_t total_bytes; std::uint64_t expected_driver_generation, stream_id;
      std::uint32_t reserved1;
    };
    struct tap_status {
      std::uint32_t magic; std::uint16_t major, minor, header_bytes, reserved0;
      std::uint32_t total_bytes; std::uint64_t driver_generation, stream_id, dropped_bytes, discontinuity_count, producer_cursor, consumer_cursor;
      std::uint32_t terminal_reason, reserved1;
    };
    struct tap_caps {
      std::uint32_t magic; std::uint16_t major, minor, header_bytes, reserved0;
      std::uint32_t total_bytes; std::uint64_t driver_generation, stream_id;
      std::uint32_t format_generation, format_bytes;
      std::array<std::uint8_t, 52> format;
      std::uint32_t flags, reserved1;
    };
#pragma pack(pop)
    static_assert(sizeof(tap_request) == 36);
    static_assert(sizeof(tap_status) == 72);
    static_assert(sizeof(tap_caps) == 100);
    constexpr DWORD ioctl_vibe_mat_tap_query_caps = 0x00226000UL;
    constexpr DWORD ioctl_vibe_mat_tap_status = 0x00226004UL;
    constexpr DWORD ioctl_vibe_mat_tap_read = 0x0022600aUL;
    constexpr wchar_t default_device_path[] = L"\\\\.\\VibepolloMatTap";
    struct ioctl_result { bool ok {}; DWORD error {ERROR_GEN_FAILURE}; DWORD bytes {}; };
    std::uint16_t u16(const std::vector<std::uint8_t> &v, std::size_t o) { return v[o] | (std::uint16_t(v[o+1]) << 8); }
    std::uint32_t u32(const std::vector<std::uint8_t> &v, std::size_t o) { std::uint32_t x {}; for (int i=3;i>=0;--i) x=(x<<8)|v[o+i]; return x; }
    std::uint64_t u64(const std::vector<std::uint8_t> &v, std::size_t o) { std::uint64_t x {}; for (int i=7;i>=0;--i) x=(x<<8)|v[o+i]; return x; }
    std::optional<std::vector<std::uint8_t>> frame_driver_record(const std::vector<std::uint8_t> &raw, const descriptor_identity &expected) {
      if (raw.size() < 80 || u32(raw,0)!=0x3154414dU || u16(raw,4)!=1 || u16(raw,6)>0 || u16(raw,8)!=80 || u16(raw,10)!=0 || u32(raw,16)&~0x0fU || !u64(raw,20) || !u64(raw,28) || !u64(raw,68)) return std::nullopt;
      const auto payload=u32(raw,76); if(!payload || payload>32*1024 || payload%16 || 80+payload!=raw.size()) return std::nullopt;
      if (!std::equal(raw.begin()+36,raw.begin()+52,expected.bytes.begin())) return std::nullopt;
      atmos_mat_direct::record framed {u64(raw,20),u64(raw,28),u64(raw,52),u64(raw,60),u64(raw,68),expected,u32(raw,16),std::vector<std::uint8_t>(raw.begin()+80,raw.end())}; return atmos_mat_direct::encode(framed);
    }
    class windows_control_device_reader final : public byte_transport {
    public:
      explicit windows_control_device_reader(const wchar_t *path, descriptor_identity expected) : expected_(expected), handle_(CreateFileW(path ? path : default_device_path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)) {}
      ~windows_control_device_reader() override { cancel(); if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
      std::optional<std::vector<std::uint8_t>> read() override {
        while (handle_ != INVALID_HANDLE_VALUE && !cancelled_.load()) {
          if (!session_ && !refresh_session()) return std::nullopt;
          std::vector<std::uint8_t> buffer(atmos_mat_direct::header_bytes + atmos_mat_direct::max_payload_bytes);
          const tap_request request {0x3154414dU, 1, 0, sizeof(tap_request), 0, sizeof(tap_request), session_->driver_generation, session_->stream_id, 0};
          const auto result = invoke_ioctl(ioctl_vibe_mat_tap_read, request, buffer.data(), static_cast<DWORD>(buffer.size()));
          if (cancelled_.load()) return std::nullopt;
          if (!result.ok && (result.error == ERROR_NO_MORE_ITEMS || result.error == ERROR_NO_MORE_FILES)) { Sleep(1); continue; }
          if (!result.ok && result.error == ERROR_RETRY) { session_.reset(); continue; }
          if (!result.ok || !result.bytes) return std::nullopt;
          buffer.resize(result.bytes); return frame_driver_record(buffer, expected_);
        }
        return std::nullopt;
      }
      void cancel() noexcept override { cancelled_.store(true); if(handle_!=INVALID_HANDLE_VALUE) CancelIoEx(handle_,nullptr); }
    private:
      ioctl_result invoke_ioctl(DWORD code, const tap_request &request, void *output, DWORD output_bytes) {
        OVERLAPPED pending {};
        pending.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pending.hEvent) return {};
        DWORD bytes {};
        BOOL completed = DeviceIoControl(handle_, code, const_cast<tap_request *>(&request), sizeof(request), output, output_bytes, &bytes, &pending);
        DWORD error = completed ? ERROR_SUCCESS : GetLastError();
        if (!completed && error == ERROR_IO_PENDING) {
          if (WaitForSingleObject(pending.hEvent, INFINITE) == WAIT_OBJECT_0 && GetOverlappedResult(handle_, &pending, &bytes, FALSE)) {
            completed = TRUE;
            error = ERROR_SUCCESS;
          } else {
            error = GetLastError();
          }
        }
        CloseHandle(pending.hEvent);
        return {completed == TRUE, error, bytes};
      }
      bool refresh_session() {
        tap_caps caps {};
        const tap_request request {0x3154414dU, 1, 0, sizeof(tap_request), 0, sizeof(tap_request), 0, 0, 0};
        const auto result = invoke_ioctl(ioctl_vibe_mat_tap_query_caps, request, &caps, sizeof(caps));
        if (!result.ok || result.bytes != sizeof(caps) || caps.magic != 0x3154414dU || caps.major != 1 || caps.minor > 0 ||
            caps.header_bytes != sizeof(caps) || caps.reserved0 != 0 || caps.total_bytes != sizeof(caps) ||
            caps.format_bytes != caps.format.size() || caps.reserved1 != 0) return false;
        if (std::any_of(expected_.bytes.begin(), expected_.bytes.end(), [](const auto byte) { return byte != 0; }) &&
            caps.format != expected_.bytes) return false;
        session_ = validate_tap_session(caps.driver_generation, caps.stream_id);
        return session_.has_value();
      }
      descriptor_identity expected_ {};
      HANDLE handle_ {INVALID_HANDLE_VALUE};
      std::atomic_bool cancelled_ {};
      std::optional<tap_session_identity> session_;
    };
  }
  std::unique_ptr<byte_transport> make_windows_control_device_reader(const wchar_t *path, descriptor_identity expected) { return std::make_unique<windows_control_device_reader>(path, expected); }
}
#else
namespace atmos_mat_host { std::unique_ptr<byte_transport> make_windows_control_device_reader(const wchar_t *, descriptor_identity) { return {}; } }
#endif
