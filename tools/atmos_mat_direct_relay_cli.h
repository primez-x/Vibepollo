#pragma once

#include "tools/atmos_mat_client_writer.h"
#include "tools/atmos_mat_direct_protocol.h"
#include "tools/atmos_mat_host_relay.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace atmos_mat_relay_cli {
  enum class role { host, client };
  struct options {
    role mode {};
    std::string listen, connect, certificate, private_key, peer_ca, peer_identity, endpoint_id;
    std::string tap_path {R"(\\.\VibepolloMatTap)"};
    atmos_mat_direct::descriptor descriptor {};
    std::uint32_t queue_frames {8192};
  };
  [[nodiscard]] std::optional<options> parse(int argc, const char *const argv[]);

  struct relay_result {
    bool clean {}, cancelled {};
    std::uint64_t host_bytes {}, client_bytes {}, carrier_frames {};
    std::array<std::uint8_t, 32> host_sha256 {}, client_sha256 {};
  };
  class memory_channel {
  public:
    bool send(std::vector<std::uint8_t> frame);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> receive();
    void cancel() noexcept;
    [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
  private:
    std::vector<std::vector<std::uint8_t>> frames_;
    std::size_t read_ {};
    bool cancelled_ {};
  };
  [[nodiscard]] relay_result pump_in_memory(atmos_mat_host::relay &host, atmos_mat_client::writer &client,
    const atmos_mat_direct::descriptor &expected,
    const std::vector<std::vector<std::uint8_t>> &tap_records,
    memory_channel &channel);
  int run(const options &options);
  [[nodiscard]] std::string hex_digest(const std::array<std::uint8_t, 32> &digest);
}  // namespace atmos_mat_relay_cli
