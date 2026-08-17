#include "../tests_common.h"

#include <src/hdr_request_policy.h>

namespace {

using hdr::request_policy::inputs_t;
using hdr::request_policy::override_e;

TEST(HdrRequestPolicy, CandidateForceOffWinsOverGlobalAutomatic) {
  const auto result = hdr::request_policy::resolve({
    .client_hdr_requested = true,
    .prefer_sdr_10bit = false,
    .force_sdr = false,
    .request_override = override_e::force_off,
  });

  EXPECT_FALSE(result.enable_hdr);
  EXPECT_TRUE(result.force_sdr);
  EXPECT_FALSE(result.effective_hdr_requested());
}

TEST(HdrRequestPolicy, CandidateForceOnWinsOverGlobalForceOff) {
  const auto result = hdr::request_policy::resolve({
    .client_hdr_requested = false,
    .prefer_sdr_10bit = true,
    .force_sdr = true,
    .request_override = override_e::force_on,
  });

  EXPECT_TRUE(result.enable_hdr);
  EXPECT_FALSE(result.prefer_sdr_10bit);
  EXPECT_FALSE(result.force_sdr);
  EXPECT_TRUE(result.effective_hdr_requested());
}

TEST(HdrRequestPolicy, AutomaticPreservesExistingClientDecision) {
  const auto result = hdr::request_policy::resolve({
    .client_hdr_requested = true,
    .prefer_sdr_10bit = true,
    .force_sdr = false,
    .request_override = override_e::automatic,
  });

  EXPECT_TRUE(result.enable_hdr);
  EXPECT_TRUE(result.prefer_sdr_10bit);
  EXPECT_FALSE(result.effective_hdr_requested());
}

TEST(HdrRequestPolicy, InvalidTextIsNotAcceptedAsAnOverride) {
  EXPECT_EQ(hdr::request_policy::parse_override("automatic"), override_e::automatic);
  EXPECT_EQ(hdr::request_policy::parse_override("auto"), override_e::automatic);
  EXPECT_EQ(hdr::request_policy::parse_override("force_on"), override_e::force_on);
  EXPECT_EQ(hdr::request_policy::parse_override("force_off"), override_e::force_off);
  EXPECT_FALSE(hdr::request_policy::parse_override("force-on").has_value());
}

}  // namespace
