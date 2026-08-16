#include "../tests_common.h"
#include "tools/atmos_mat_direct_relay_cli.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
  atmos_mat_direct::descriptor relay_descriptor() {
    atmos_mat_direct::descriptor value {};
    value.bytes[0] = 0x19;
    value.bytes[51] = 0x52;
    return value;
  }

  atmos_mat_client::descriptor_identity writer_descriptor() {
    atmos_mat_client::descriptor_identity value {};
    value.bytes[0] = 0x19;
    value.bytes[51] = 0x52;
    return value;
  }

  std::vector<std::uint8_t> record(const std::uint64_t frame, const std::uint8_t byte) {
    atmos_mat_direct::record value {7, 9, frame, 100, 10'000'000, relay_descriptor(), frame == 0 ? atmos_mat_direct::start : atmos_mat_direct::stop,
      std::vector<std::uint8_t>(atmos_mat_direct::carrier_frame_bytes, byte)};
    const auto encoded = atmos_mat_direct::encode(value);
    return encoded.value_or(std::vector<std::uint8_t> {});
  }

  TEST(AtmosMatDirectRelayCli, ParserRejectsMissingAuthAndEndpoint) {
    const std::string descriptor(104, '0');
    const char *host_missing_ca[] = {"relay", "host", "--listen", "127.0.0.1:4444", "--cert", "a", "--key", "b", "--peer-identity", "client", "--descriptor", descriptor.c_str()};
    EXPECT_FALSE(atmos_mat_relay_cli::parse(12, host_missing_ca));
    const char *client_missing_endpoint[] = {"relay", "client", "--connect", "localhost:4444", "--cert", "a", "--key", "b", "--peer-ca", "c", "--peer-identity", "host", "--descriptor", descriptor.c_str()};
    EXPECT_FALSE(atmos_mat_relay_cli::parse(14, client_missing_endpoint));
  }

  TEST(AtmosMatDirectRelayCli, InMemoryRelayPreservesExactCarrierBytes) {
    atmos_mat_relay_cli::memory_channel channel;
    atmos_mat_host::relay host {relay_descriptor(), 4};
    atmos_mat_client::writer client {writer_descriptor(), 4};
    const std::vector<std::vector<std::uint8_t>> source {record(0, 0xa1), record(1, 0x3e)};
    const auto result = atmos_mat_relay_cli::pump_in_memory(host, client, relay_descriptor(), source, channel);
    EXPECT_TRUE(result.clean);
    EXPECT_EQ(result.host_bytes, 32U);
    EXPECT_EQ(result.client_bytes, 32U);
    EXPECT_EQ(result.host_sha256, result.client_sha256);
    std::array<std::uint8_t, 32> out {};
    EXPECT_EQ(client.copy_next(out).frames, 2U);
    EXPECT_EQ(out[0], 0xa1);
    EXPECT_EQ(out[16], 0x3e);
  }

  TEST(AtmosMatDirectRelayCli, GapEofAndCancelFailClosed) {
    atmos_mat_host::relay host {relay_descriptor(), 4};
    atmos_mat_client::writer client {writer_descriptor(), 4};
    atmos_mat_relay_cli::memory_channel channel;
    const auto gap = atmos_mat_relay_cli::pump_in_memory(host, client, relay_descriptor(), {record(0, 1), record(2, 2)}, channel);
    EXPECT_FALSE(gap.clean);
    EXPECT_TRUE(gap.cancelled);
    EXPECT_TRUE(host.poisoned());

    host.reset();
    client.reset(0);
    const auto eof = atmos_mat_relay_cli::pump_in_memory(host, client, relay_descriptor(), {}, channel);
    EXPECT_FALSE(eof.clean);
    EXPECT_TRUE(eof.cancelled);

    host.reset();
    client.reset(0);
    channel.cancel();
    const auto cancelled = atmos_mat_relay_cli::pump_in_memory(host, client, relay_descriptor(), {record(0, 3)}, channel);
    EXPECT_FALSE(cancelled.clean);
    EXPECT_TRUE(cancelled.cancelled);
    EXPECT_TRUE(channel.cancelled());
  }
}
