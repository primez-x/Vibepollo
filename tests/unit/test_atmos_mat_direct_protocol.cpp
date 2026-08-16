#include "../tests_common.h"
#include "tools/atmos_mat_direct_protocol.h"

TEST(AtmosMatDirectProtocol, RoundTripsFullDescriptorFlagsAndRejectsMalformedWire) {
  using namespace atmos_mat_direct;
  descriptor d {};
  d.bytes[0] = 0x10;
  d.bytes[51] = 0x51;
  record r {7, 19, 0, 1000, 10'000'000, d, 0x9, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
  const auto wire = encode(r);
  ASSERT_TRUE(wire);
  const auto decoded = decode(*wire);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->payload, r.payload);
  EXPECT_EQ(decoded->descriptor_value.bytes[51], 0x51);
  EXPECT_EQ(decoded->flags, 0x9U);
  auto malformed = *wire;
  malformed[0] ^= 1;
  EXPECT_FALSE(decode(malformed));
}
