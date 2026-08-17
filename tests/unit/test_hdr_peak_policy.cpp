/**
 * @file tests/unit/test_hdr_peak_policy.cpp
 * @brief Deterministic host HDR peak precedence contracts.
 */
#include "../tests_common.h"

#include <src/hdr_peak_policy.h>

#include <cstdint>
#include <optional>

namespace {
  using client_hdr::capabilities_t;
  using client_hdr::peak_t;
  using hdr::peak_policy::explanation_e;
  using hdr::peak_policy::inputs_t;
  using hdr::peak_policy::source_e;

  peak_t calibrated_peak(std::uint32_t nits) {
    return {nits, "windows-icc-mhc2"};
  }

  peak_t display_peak(std::uint32_t nits) {
    return {nits, "dxgi-output"};
  }

  capabilities_t client_caps(
    std::optional<std::uint32_t> calibrated = std::nullopt,
    std::optional<std::uint32_t> display = std::nullopt
  ) {
    capabilities_t result;
    if (calibrated) {
      result.calibrated = calibrated_peak(*calibrated);
    }
    if (display) {
      result.display_reported = display_peak(*display);
    }
    return result;
  }

  inputs_t base_inputs() {
    inputs_t result;
    result.final_effective_hdr_requested = true;
    return result;
  }

  TEST(HdrPeakPolicy, ExplicitNumericOverrideWinsEveryOtherSource) {
    auto inputs = base_inputs();
    inputs.explicit_override_peak_nits = 1700;
    inputs.manual_profile_selected = true;
    inputs.manual_profile_peak_nits = 1600;
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 1300;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 1700u);
    EXPECT_EQ(result.source, source_e::explicit_numeric_override);
    EXPECT_EQ(result.explanation, explanation_e::explicit_numeric_override);
  }

  TEST(HdrPeakPolicy, ValidSelectedManualProfileWinsClientValues) {
    auto inputs = base_inputs();
    inputs.manual_profile_selected = true;
    inputs.manual_profile_peak_nits = 1600;
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 1300;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 1600u);
    EXPECT_EQ(result.source, source_e::manual_profile);
    EXPECT_EQ(result.explanation, explanation_e::manual_profile);
  }

  TEST(HdrPeakPolicy, SelectedInvalidManualProfileSuppressesClientsAndUsesGlobalFallback) {
    auto inputs = base_inputs();
    inputs.manual_profile_selected = true;
    inputs.manual_profile_peak_nits = std::nullopt;
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 600u);
    EXPECT_EQ(result.source, source_e::global_default);
    EXPECT_EQ(result.explanation, explanation_e::invalid_manual_profile);
  }

  TEST(HdrPeakPolicy, CalibratedClientPeakWinsWhenNoHostOverrideExists) {
    auto inputs = base_inputs();
    inputs.client_capabilities = client_caps(1500);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 1500u);
    EXPECT_EQ(result.source, source_e::client_calibrated);
    EXPECT_EQ(result.explanation, explanation_e::client_calibrated);
  }

  TEST(HdrPeakPolicy, DisplayReportedClientPeakIsTheFallbackForCalibration) {
    auto inputs = base_inputs();
    inputs.client_capabilities = client_caps(std::nullopt, 1400);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 1400u);
    EXPECT_EQ(result.source, source_e::client_display_reported);
    EXPECT_EQ(result.explanation, explanation_e::client_display_reported);
  }

  TEST(HdrPeakPolicy, CalibratedClientPeakPrecedesDisplayReportedPeak) {
    auto inputs = base_inputs();
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 1500u);
    EXPECT_EQ(result.source, source_e::client_calibrated);
    EXPECT_EQ(result.explanation, explanation_e::client_calibrated);
  }

  TEST(HdrPeakPolicy, NoClientValuesUseGlobalDefault) {
    auto inputs = base_inputs();
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 600u);
    EXPECT_EQ(result.source, source_e::global_default);
    EXPECT_EQ(result.explanation, explanation_e::global_default);
  }

  TEST(HdrPeakPolicy, NoGlobalDefaultProducesNoOverride) {
    const auto result = hdr::peak_policy::resolve(base_inputs());

    EXPECT_FALSE(result.reported_peak_nits.has_value());
    EXPECT_EQ(result.source, source_e::none);
    EXPECT_EQ(result.explanation, explanation_e::no_override);
  }

  TEST(HdrPeakPolicy, ClientValuesAreIgnoredWhenFinalEffectiveHdrIsFalse) {
    auto inputs = base_inputs();
    inputs.final_effective_hdr_requested = false;
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 600u);
    EXPECT_EQ(result.source, source_e::global_default);
    EXPECT_EQ(result.explanation, explanation_e::hdr_disabled);
  }

  TEST(HdrPeakPolicy, ExplicitAndManualHostValuesKeepTheirSemanticsForSdr) {
    auto explicit_inputs = base_inputs();
    explicit_inputs.final_effective_hdr_requested = false;
    explicit_inputs.explicit_override_peak_nits = 1800;
    explicit_inputs.client_capabilities = client_caps(1500);

    const auto explicit_result = hdr::peak_policy::resolve(explicit_inputs);
    EXPECT_EQ(explicit_result.reported_peak_nits, 1800u);
    EXPECT_EQ(explicit_result.source, source_e::explicit_numeric_override);

    auto manual_inputs = base_inputs();
    manual_inputs.final_effective_hdr_requested = false;
    manual_inputs.manual_profile_selected = true;
    manual_inputs.manual_profile_peak_nits = 1700;
    manual_inputs.client_capabilities = client_caps(1500);

    const auto manual_result = hdr::peak_policy::resolve(manual_inputs);
    EXPECT_EQ(manual_result.reported_peak_nits, 1700u);
    EXPECT_EQ(manual_result.source, source_e::manual_profile);
  }

  TEST(HdrPeakPolicy, SelectedInvalidManualProfileSuppressesClientsEvenForSdr) {
    auto inputs = base_inputs();
    inputs.final_effective_hdr_requested = false;
    inputs.manual_profile_selected = true;
    inputs.client_capabilities = client_caps(1500, 1400);
    inputs.global_default_peak_nits = 600;

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 600u);
    EXPECT_EQ(result.source, source_e::global_default);
    EXPECT_EQ(result.explanation, explanation_e::invalid_manual_profile);
  }

  TEST(HdrPeakPolicy, PolicyDoesNotClampTransportPeaks) {
    auto inputs = base_inputs();
    inputs.client_capabilities = client_caps(100000);

    const auto result = hdr::peak_policy::resolve(inputs);

    EXPECT_EQ(result.reported_peak_nits, 100000u);
    EXPECT_EQ(result.source, source_e::client_calibrated);
  }
}  // namespace
