#include "../tests_common.h"
#include "tools/atmos_mat_host_relay.h"

namespace {
  class fake_transport final : public atmos_mat_host::byte_transport {
  public:
    explicit fake_transport(std::vector<std::uint8_t> record) : record_(std::move(record)) {}
    std::optional<std::vector<std::uint8_t>> read() override { if (cancelled_ || sent_) return std::nullopt; sent_ = true; return record_; }
    void cancel() noexcept override { cancelled_ = true; }
    bool cancelled() const { return cancelled_; }
  private: std::vector<std::uint8_t> record_; bool sent_ {}, cancelled_ {};
  };
}

TEST(AtmosMatHostRelay, PoisonsGapsAndCapacityAndDistinguishesCleanEof) {
  using namespace atmos_mat_host;
  descriptor_identity d {};
  d.bytes[0] = 0x10;
  d.bytes[51] = 0x51;
  relay r {d, 1};
  EXPECT_EQ(r.admit({3, 9, 0, 1, 10, d, 0, std::vector<std::uint8_t>(16, 0xa5)}), admit_result::accepted);
  EXPECT_EQ(r.admit({3, 9, 2, 2, 10, d, 0, std::vector<std::uint8_t>(16, 0xa5)}), admit_result::gap_poisoned);
  EXPECT_TRUE(r.poisoned());
  r.reset();
  EXPECT_EQ(r.admit({4, 9, 0, 1, 10, d, 0, std::vector<std::uint8_t>(16, 0xa5)}), admit_result::accepted);
  EXPECT_EQ(r.admit({4, 9, 1, 2, 10, d, 0, std::vector<std::uint8_t>(16, 0xa5)}), admit_result::queue_full_poisoned);

  atmos_mat_direct::record wire_record {8, 10, 0, 3, 10, d, 0, std::vector<std::uint8_t>(16, 0x5a)};
  auto wire = atmos_mat_direct::encode(wire_record);
  ASSERT_TRUE(wire);
  relay eof_target {d, 2};
  fake_transport device {std::move(*wire)};
  reader read {eof_target, device};
  EXPECT_EQ(read.pump_once(), terminal_reason::none);
  EXPECT_EQ(read.pump_once(), terminal_reason::eof); // A clean EOF is distinct from a poisoned relay.
  EXPECT_FALSE(eof_target.poisoned());
  read.cancel();
  EXPECT_TRUE(device.cancelled());
}
