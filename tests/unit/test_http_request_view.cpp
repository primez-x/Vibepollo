#include <gtest/gtest.h>

#include "src/http_request_view.h"

TEST(HttpRequestView, removesCapabilityAndPreservesUnrelatedRawSegments) {
  const auto result = http::client_hdr::sanitize_request_query(
    "appid=1&ClientDisplayCapabilities=abc-_&mode=2560%78%401600"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::valid);
  EXPECT_EQ(result.encoded_value, "abc-_");
  EXPECT_EQ(result.sanitized_query, "appid=1&mode=2560%78%401600");
}

TEST(HttpRequestView, mixedCaseEncodedAndRepeatedCapabilityIsInvalidAndRemoved) {
  const auto result = http::client_hdr::sanitize_request_query(
    "appid=1&%43LIENT%44ISPLAY%43APABILITIES=one&clientdisplaycapabilities=two&mode=1"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::invalid);
  EXPECT_FALSE(result.encoded_value.has_value());
  EXPECT_EQ(result.sanitized_query, "appid=1&mode=1");
}

TEST(HttpRequestView, malformedPercentIsInvalidWithoutChangingOtherFields) {
  const auto result = http::client_hdr::sanitize_request_query(
    "appid=1&clientDisplayCapabilities=%ZZ&mode=1"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::invalid);
  EXPECT_EQ(result.sanitized_query, "appid=1&mode=1");
}

TEST(HttpRequestView, percentEncodedCapabilityValueIsDecodedOnlyForCapabilityParser) {
  const auto result = http::client_hdr::sanitize_request_query(
    "appid=1&clientDisplayCapabilities=abc%2D_%3D&mode=1"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::valid);
  EXPECT_EQ(result.encoded_value, "abc-_=");
  EXPECT_EQ(result.sanitized_query, "appid=1&mode=1");
}

TEST(HttpRequestView, EmptyCapabilityStillCountsAsRepeatedWhenFollowedByValue) {
  const auto result = http::client_hdr::sanitize_request_query(
    "clientDisplayCapabilities=&clientDisplayCapabilities=abc"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::invalid);
  EXPECT_FALSE(result.encoded_value.has_value());
  EXPECT_EQ(result.sanitized_query, "");
}

TEST(HttpRequestView, MalformedEncodedCapabilityKeyIsRemoved) {
  const auto result = http::client_hdr::sanitize_request_query(
    "appid=1&clientDisplay%ZZCapabilities=secret&mode=1"
  );

  EXPECT_EQ(result.status, http::client_hdr::capability_status::invalid);
  EXPECT_EQ(result.sanitized_query, "appid=1&mode=1");
}

TEST(HttpRequestView, rawEncodedValueLimitIsCheckedBeforeDecode) {
  std::string at_limit;
  at_limit.reserve(12288);
  for (int i = 0; i < 4096; ++i) {
    at_limit += "%61";
  }
  const std::string over_limit = at_limit + "a";

  const auto accepted = http::client_hdr::sanitize_request_query(
    "clientDisplayCapabilities=" + at_limit
  );
  const auto rejected = http::client_hdr::sanitize_request_query(
    "clientDisplayCapabilities=" + over_limit
  );

  EXPECT_EQ(accepted.status, http::client_hdr::capability_status::valid);
  EXPECT_EQ(rejected.status, http::client_hdr::capability_status::invalid);
  EXPECT_EQ(rejected.sanitized_query, "");
}
