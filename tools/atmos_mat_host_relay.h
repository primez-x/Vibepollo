#pragma once

#include "tools/atmos_mat_direct_protocol.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;

namespace atmos_mat_host {
  constexpr std::size_t carrier_frame_bytes = atmos_mat_direct::carrier_frame_bytes;
  using descriptor_identity = atmos_mat_direct::descriptor;
  struct tap_session_identity { std::uint64_t driver_generation {}, stream_id {}; };
  [[nodiscard]] std::optional<tap_session_identity> validate_tap_session(std::uint64_t driver_generation, std::uint64_t stream_id);
  struct carrier_block { std::uint64_t generation {}, stream_id {}, first_carrier_frame {}, host_qpc {}, host_qpc_frequency {}; descriptor_identity descriptor {}; std::uint32_t flags {}; std::vector<std::uint8_t> bytes; };
  enum class admit_result { accepted, gap_poisoned, generation_changed_poisoned, stream_changed_poisoned, descriptor_mismatch_poisoned, queue_full_poisoned, malformed_poisoned };
  enum class terminal_reason { none, malformed, gap, generation_change, stream_change, descriptor_mismatch, queue_full, cancelled, eof, transport_error };
  class relay {
  public:
    relay(descriptor_identity expected, std::size_t capacity);
    admit_result admit(const carrier_block &block);
    [[nodiscard]] std::optional<carrier_block> pop(); void reset();
    [[nodiscard]] bool poisoned() const; [[nodiscard]] terminal_reason terminal() const; [[nodiscard]] std::array<std::uint8_t, 32> transcript_sha256() const; [[nodiscard]] std::uint64_t bytes() const;
  private:
    descriptor_identity expected_; std::size_t capacity_; std::vector<carrier_block> queue_; bool initialized_ {}, poisoned_ {}; terminal_reason terminal_ {terminal_reason::none}; std::uint64_t generation_ {}, stream_ {}, next_frame_ {}, bytes_ {}; std::array<std::uint8_t, 32> digest_ {};
  };
  class byte_transport { public: virtual ~byte_transport() = default; virtual std::optional<std::vector<std::uint8_t>> read() = 0; virtual void cancel() noexcept = 0; };
  class reader { public: reader(relay &target, byte_transport &transport) : target_(target), transport_(transport) {} terminal_reason pump_once(); void cancel() noexcept; private: relay &target_; byte_transport &transport_; };
  // The caller supplies both a local identity and an explicit peer trust anchor; no anonymous TLS is permitted.
  [[nodiscard]] bool configure_mutual_tls(SSL_CTX *context, const char *certificate_pem, const char *private_key_pem, const char *peer_ca_pem);
  std::unique_ptr<byte_transport> make_windows_control_device_reader(const wchar_t *path, descriptor_identity expected_descriptor = {});
}
