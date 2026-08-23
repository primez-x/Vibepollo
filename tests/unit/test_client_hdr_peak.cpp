#include "../../src/client_hdr_peak.h"

#include <gtest/gtest.h>

TEST(ClientHdrPeak, RejectsMalformedOrOutOfRangeValues) {
  EXPECT_FALSE(client_hdr_peak::parse_nits("").has_value());
  EXPECT_FALSE(client_hdr_peak::parse_nits("0").has_value());
  EXPECT_FALSE(client_hdr_peak::parse_nits("100001").has_value());
  EXPECT_FALSE(client_hdr_peak::parse_nits("12.5").has_value());
  EXPECT_FALSE(client_hdr_peak::parse_nits("1e3").has_value());
  EXPECT_FALSE(client_hdr_peak::parse_nits("-1").has_value());
}

TEST(ClientHdrPeak, AcceptsSupportedIntegerRange) {
  ASSERT_TRUE(client_hdr_peak::parse_nits("400").has_value());
  EXPECT_EQ(*client_hdr_peak::parse_nits("400"), 400u);
  ASSERT_TRUE(client_hdr_peak::parse_nits("2000").has_value());
  EXPECT_EQ(*client_hdr_peak::parse_nits("2000"), 2000u);
}

TEST(ClientHdrPeak, PrefersCalibratedPeakOverDisplayReportedPeak) {
  const auto result = client_hdr_peak::resolve("1600", "1000");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->peak_nits, 1600u);
  EXPECT_EQ(result->source, client_hdr_peak::source_e::calibrated);
}

TEST(ClientHdrPeak, FallsBackToDisplayReportedPeak) {
  const auto result = client_hdr_peak::resolve("invalid", "900");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->peak_nits, 900u);
  EXPECT_EQ(result->source, client_hdr_peak::source_e::display_reported);
}

TEST(ClientHdrPeak, DoesNotProduceAValueWhenBothSourcesAreInvalid) {
  EXPECT_FALSE(client_hdr_peak::resolve("invalid", "100001").has_value());
}

TEST(ClientHdrPeak, ClampsReportedValuesToHostRange) {
  const auto low = client_hdr_peak::resolve("399", "");
  ASSERT_TRUE(low.has_value());
  EXPECT_EQ(low->peak_nits, client_hdr_peak::minimum_host_nits);

  const auto high = client_hdr_peak::resolve("2001", "");
  ASSERT_TRUE(high.has_value());
  EXPECT_EQ(high->peak_nits, client_hdr_peak::maximum_host_nits);
}

TEST(ClientHdrPeak, AutomaticRequestPreservesClientPolicy) {
  const auto result = client_hdr_peak::resolve_effective_request(
    false,
    true,
    client_hdr_peak::request_override_e::automatic
  );
  EXPECT_FALSE(result.hdr_requested);
  EXPECT_TRUE(result.prefer_sdr_10bit);
  EXPECT_FALSE(result.force_sdr);
}

TEST(ClientHdrPeak, ForceOnOverridesTenBitSdrPreference) {
  const auto result = client_hdr_peak::resolve_effective_request(
    false,
    true,
    client_hdr_peak::request_override_e::force_on
  );
  EXPECT_TRUE(result.hdr_requested);
  EXPECT_FALSE(result.prefer_sdr_10bit);
  EXPECT_FALSE(result.force_sdr);
}

TEST(ClientHdrPeak, ForceOffDisablesHdrWithoutDiscardingTenBitSdrPreference) {
  const auto result = client_hdr_peak::resolve_effective_request(
    true,
    true,
    client_hdr_peak::request_override_e::force_off
  );
  EXPECT_FALSE(result.hdr_requested);
  EXPECT_TRUE(result.prefer_sdr_10bit);
  EXPECT_TRUE(result.force_sdr);
}
