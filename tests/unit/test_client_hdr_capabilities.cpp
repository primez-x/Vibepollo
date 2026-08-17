/**
 * @file tests/unit/test_client_hdr_capabilities.cpp
 * @brief Host-side v1 client HDR capability parsing contracts.
 */
#include "../tests_common.h"

#include <src/client_hdr_capabilities.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace {
  using client_hdr::parse_status_e;

  std::string encode_base64url_for_test(std::string_view input, bool padded = false) {
    constexpr std::string_view alphabet {
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
    };

    std::string encoded;
    encoded.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < input.size(); offset += 3) {
      const auto remaining = input.size() - offset;
      const auto first = static_cast<std::uint8_t>(input[offset]);
      const auto second = remaining > 1 ? static_cast<std::uint8_t>(input[offset + 1]) : 0;
      const auto third = remaining > 2 ? static_cast<std::uint8_t>(input[offset + 2]) : 0;

      encoded.push_back(alphabet[first >> 2]);
      encoded.push_back(alphabet[((first & 0x03u) << 4) | (second >> 4)]);
      if (remaining > 1) {
        encoded.push_back(alphabet[((second & 0x0fu) << 2) | (third >> 6)]);
      } else if (padded) {
        encoded.push_back('=');
      }
      if (remaining > 2) {
        encoded.push_back(alphabet[third & 0x3fu]);
      } else if (padded) {
        encoded.push_back('=');
      }
    }
    return encoded;
  }

  std::string source_json(std::string_view source, std::string_view peak) {
    return std::string {
      R"json({"version":1,"platform":"windows-desktop","calibrated":{"source":")json"
    } + std::string(source) + R"json(","peak_luminance_nits":)json" + std::string(peak) + R"json(}})json";
  }

  TEST(ClientHdrCapabilities, EmptyInputIsAbsent) {
    const auto result = client_hdr::parse({});

    EXPECT_EQ(result.status, parse_status_e::absent);
    EXPECT_FALSE(result.capabilities.has_value());
  }

  TEST(ClientHdrCapabilities, ParsesBothWireSourcesAndPreservesMetadata) {
    const auto encoded = encode_base64url_for_test(R"json({
      "version": 1,
      "platform": "windows-desktop",
      "calibrated": {
        "source": "windows-icc-mhc2",
        "peak_luminance_nits": 1000
      },
      "edid": {
        "source": "dxgi-output",
        "peak_luminance_nits": 1000
      }
    })json");

    const auto result = client_hdr::parse(encoded);

    ASSERT_EQ(result.status, parse_status_e::valid);
    ASSERT_TRUE(result.capabilities.has_value());
    ASSERT_TRUE(result.capabilities->calibrated.has_value());
    ASSERT_TRUE(result.capabilities->display_reported.has_value());
    EXPECT_EQ(result.capabilities->version, 1u);
    EXPECT_EQ(result.capabilities->platform, "windows-desktop");
    EXPECT_EQ(result.capabilities->calibrated->source, "windows-icc-mhc2");
    EXPECT_EQ(result.capabilities->calibrated->peak_luminance_nits, 1000u);
    EXPECT_EQ(result.capabilities->display_reported->source, "dxgi-output");
    EXPECT_EQ(result.capabilities->display_reported->peak_luminance_nits, 1000u);
  }

  TEST(ClientHdrCapabilities, AcceptsPaddedAndUnpaddedBase64Url) {
    constexpr std::string_view json {
      R"json({"version":1,"platform":"windows-desktop","calibrated":{"source":"windows-icc-mhc2","peak_luminance_nits":1000}})json"
    };
    const auto padded = encode_base64url_for_test(json, true);
    const auto unpadded = encode_base64url_for_test(json);

    ASSERT_TRUE(padded.ends_with('='));
    ASSERT_FALSE(unpadded.ends_with('='));
    EXPECT_EQ(client_hdr::parse(padded).status, parse_status_e::valid);
    EXPECT_EQ(client_hdr::parse(unpadded).status, parse_status_e::valid);
  }

  TEST(ClientHdrCapabilities, AcceptsUrlSafeAlphabet) {
    // The payloads contain valid JSON unknown fields; their standard base64
    // forms contain '+' and '/', respectively, which are replaced below.
    constexpr std::string_view dash_payload {
      "eyJ2ZXJzaW9uIjoxLCJwbGF0Zm9ybSI6IndpbmRvd3MtZGVza3RvcCIsIngiOiJ44KC-In0="
    };
    constexpr std::string_view underscore_payload {
      "eyJ2ZXJzaW9uIjoxLCJwbGF0Zm9ybSI6IndpbmRvd3MtZGVza3RvcCIsIngiOiJ44KC_In0="
    };

    EXPECT_EQ(client_hdr::parse(dash_payload).status, parse_status_e::valid);
    EXPECT_EQ(client_hdr::parse(underscore_payload).status, parse_status_e::valid);
  }

  TEST(ClientHdrCapabilities, IgnoresUnknownOptionalFields) {
    const auto encoded = encode_base64url_for_test(
      R"json({"version":1,"platform":"windows-desktop","unknown":{"nested":[1,true,"ignored"]},"edid":{"source":"dxgi-output","peak_luminance_nits":1200,"future":false}})json"
    );

    const auto result = client_hdr::parse(encoded);

    ASSERT_EQ(result.status, parse_status_e::valid);
    ASSERT_TRUE(result.capabilities.has_value());
    EXPECT_FALSE(result.capabilities->calibrated.has_value());
    ASSERT_TRUE(result.capabilities->display_reported.has_value());
    EXPECT_EQ(result.capabilities->display_reported->peak_luminance_nits, 1200u);
  }

  TEST(ClientHdrCapabilities, FractionalPeaksUseNearestNitRounding) {
    for (const auto &[input, expected] : {
           std::pair<std::string_view, std::uint32_t> {"999.4", 999},
           std::pair<std::string_view, std::uint32_t> {"999.5", 1000},
           std::pair<std::string_view, std::uint32_t> {"999.6", 1000},
         }) {
      SCOPED_TRACE(input);
      const auto result = client_hdr::parse(encode_base64url_for_test(source_json("windows-icc-mhc2", input)));

      ASSERT_EQ(result.status, parse_status_e::valid);
      ASSERT_TRUE(result.capabilities.has_value());
      ASSERT_TRUE(result.capabilities->calibrated.has_value());
      EXPECT_EQ(result.capabilities->calibrated->peak_luminance_nits, expected);
    }
  }

  TEST(ClientHdrCapabilities, AValidSourceSurvivesInvalidOtherSource) {
    const auto calibrated_result = client_hdr::parse(encode_base64url_for_test(R"json({
      "version":1,
      "platform":"windows-desktop",
      "calibrated":{"source":"windows-icc-mhc2","peak_luminance_nits":1000},
      "edid":{"source":"dxgi-output","peak_luminance_nits":0}
    })json"));
    const auto display_result = client_hdr::parse(encode_base64url_for_test(R"json({
      "version":1,
      "platform":"windows-desktop",
      "calibrated":{"source":"windows-icc-mhc2","peak_luminance_nits":"bad"},
      "edid":{"source":"dxgi-output","peak_luminance_nits":1500}
    })json"));

    ASSERT_EQ(calibrated_result.status, parse_status_e::valid);
    ASSERT_TRUE(calibrated_result.capabilities.has_value());
    EXPECT_TRUE(calibrated_result.capabilities->calibrated.has_value());
    EXPECT_FALSE(calibrated_result.capabilities->display_reported.has_value());
    ASSERT_EQ(display_result.status, parse_status_e::valid);
    ASSERT_TRUE(display_result.capabilities.has_value());
    EXPECT_FALSE(display_result.capabilities->calibrated.has_value());
    ASSERT_TRUE(display_result.capabilities->display_reported.has_value());
    EXPECT_EQ(display_result.capabilities->display_reported->peak_luminance_nits, 1500u);
  }

  TEST(ClientHdrCapabilities, ValidTopLevelWithoutSourcesIsEmptyButValid) {
    const auto result = client_hdr::parse(
      encode_base64url_for_test(R"json({"version":1,"platform":"windows-desktop"})json")
    );

    ASSERT_EQ(result.status, parse_status_e::valid);
    ASSERT_TRUE(result.capabilities.has_value());
    EXPECT_FALSE(result.capabilities->calibrated.has_value());
    EXPECT_FALSE(result.capabilities->display_reported.has_value());
  }

  TEST(ClientHdrCapabilities, UnknownVersionAndPlatformAreUnsupported) {
    const auto version_result = client_hdr::parse(encode_base64url_for_test(
      R"json({"version":2,"platform":"windows-desktop"})json"
    ));
    const auto platform_result = client_hdr::parse(encode_base64url_for_test(
      R"json({"version":1,"platform":"linux-desktop"})json"
    ));

    EXPECT_EQ(version_result.status, parse_status_e::unsupported);
    EXPECT_EQ(platform_result.status, parse_status_e::unsupported);
    EXPECT_FALSE(version_result.capabilities.has_value());
    EXPECT_FALSE(platform_result.capabilities.has_value());
  }

  TEST(ClientHdrCapabilities, UnknownSourceIsUnsupportedWhenNoKnownSourceRemains) {
    const auto result = client_hdr::parse(encode_base64url_for_test(R"json({
      "version":1,
      "platform":"windows-desktop",
      "calibrated":{"source":"unknown-source","peak_luminance_nits":1000}
    })json"));

    EXPECT_EQ(result.status, parse_status_e::unsupported);
    EXPECT_FALSE(result.capabilities.has_value());
  }

  TEST(ClientHdrCapabilities, WrongTypesAndMissingFieldsAreInvalid) {
    const std::string cases[] {
      R"json({"version":"1","platform":"windows-desktop"})json",
      R"json({"version":1.0,"platform":"windows-desktop"})json",
      R"json({"version":1,"platform":[]})json",
      R"json({"version":1,"platform":"windows-desktop","calibrated":[]})json",
      R"json({"version":1,"platform":"windows-desktop","calibrated":{"source":7,"peak_luminance_nits":1000}})json",
      R"json({"version":1,"platform":"windows-desktop","calibrated":{"source":"windows-icc-mhc2"}})json",
      R"json({"version":1,"platform":"windows-desktop","calibrated":{"peak_luminance_nits":1000}})json",
    };

    for (const auto &json : cases) {
      SCOPED_TRACE(json);
      EXPECT_EQ(client_hdr::parse(encode_base64url_for_test(json)).status, parse_status_e::invalid);
    }
  }

  TEST(ClientHdrCapabilities, RejectsInvalidPeakBoundariesAndNonFiniteNumbers) {
    const std::string cases[] {
      "0",
      "-1",
      "0.5",
      "100000.1",
      "100001",
      "1e999",
      "null",
      "\"NaN\"",
      "[]",
    };

    for (const auto &peak : cases) {
      SCOPED_TRACE(peak);
      EXPECT_EQ(
        client_hdr::parse(encode_base64url_for_test(source_json("windows-icc-mhc2", peak))).status,
        parse_status_e::invalid
      );
    }
  }

  TEST(ClientHdrCapabilities, DuplicateSourceMembersAreInvalid) {
    const auto result = client_hdr::parse(encode_base64url_for_test(R"json({
      "version":1,
      "platform":"windows-desktop",
      "calibrated":{
        "source":"windows-icc-mhc2",
        "source":"windows-icc-mhc2",
        "peak_luminance_nits":1000
      },
      "edid":{"source":"dxgi-output","peak_luminance_nits":1500}
    })json"));

    EXPECT_EQ(result.status, parse_status_e::invalid);
    EXPECT_FALSE(result.capabilities.has_value());
  }

  TEST(ClientHdrCapabilities, RejectsMalformedBase64AndJson) {
    const std::string malformed_base64[] {"A", "a=b", "AAAA===", "AAAA =", "AAAA\t"};
    for (const auto &encoded : malformed_base64) {
      SCOPED_TRACE(encoded);
      EXPECT_EQ(client_hdr::parse(encoded).status, parse_status_e::invalid);
    }

    EXPECT_EQ(client_hdr::parse(encode_base64url_for_test("{broken")).status, parse_status_e::invalid);
    EXPECT_EQ(client_hdr::parse(std::string(4097, 'A')).status, parse_status_e::invalid);
  }

  TEST(ClientHdrCapabilities, AcceptsDecodedJsonAtTheInclusiveLimitAndRejectsBeyondIt) {
    std::string at_limit = R"json({"version":1,"platform":"windows-desktop","unknown":")json";
    at_limit.append(3072 - at_limit.size() - 2, 'x');
    at_limit += "\"}";
    ASSERT_EQ(at_limit.size(), 3072u);
    ASSERT_EQ(encode_base64url_for_test(at_limit).size(), 4096u);

    EXPECT_EQ(client_hdr::parse(encode_base64url_for_test(at_limit)).status, parse_status_e::valid);

    at_limit.push_back('x');
    EXPECT_EQ(client_hdr::parse(encode_base64url_for_test(at_limit)).status, parse_status_e::invalid);
  }
}  // namespace
