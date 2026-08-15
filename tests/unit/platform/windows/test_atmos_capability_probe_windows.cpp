#include "../../../tests_common.h"
#include "tools/atmos_capability_probe_windows.h"

#include <mmdeviceapi.h>

#include <array>
#include <string>
#include <variant>

namespace {
  constexpr std::string_view official_hdmi =
    "{D1B9CC2A-F519-417F-91C9-55FA65481001}";
  constexpr std::string_view official_displayport =
    "{E47E4031-3EA6-418D-8F9B-B73843CCBA97}";
  constexpr std::string_view historical_wrong_hdmi =
    "{D9E55EA0-0C89-4692-84FF-EB3C4B0D172F}";
  constexpr std::string_view historical_wrong_displayport =
    "{E47E4031-3EA6-418D-8F9B-B73843CCB2AD}";

  PROPVARIANT empty_variant() {
    PROPVARIANT value;
    PropVariantInit(&value);
    return value;
  }

  PROPVARIANT ui4_variant(const ULONG raw) {
    auto value = empty_variant();
    value.vt = VT_UI4;
    value.ulVal = raw;
    return value;
  }

  PROPVARIANT lpwstr_variant(wchar_t *raw) {
    auto value = empty_variant();
    value.vt = VT_LPWSTR;
    value.pwszVal = raw;
    return value;
  }

  void expect_exact_mat_fields(
    const WAVEFORMATEXTENSIBLE_IEC61937 &format,
    const GUID &subformat) {
    EXPECT_EQ(format.FormatExt.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
    EXPECT_EQ(format.FormatExt.Format.nChannels, 8);
    EXPECT_EQ(format.FormatExt.Format.nSamplesPerSec, 192000u);
    EXPECT_EQ(format.FormatExt.Format.nAvgBytesPerSec, 3072000u);
    EXPECT_EQ(format.FormatExt.Format.nBlockAlign, 16);
    EXPECT_EQ(format.FormatExt.Format.wBitsPerSample, 16);
    EXPECT_EQ(format.FormatExt.Format.cbSize, 34);
    EXPECT_EQ(format.FormatExt.Samples.wValidBitsPerSample, 16);
    EXPECT_EQ(format.FormatExt.dwChannelMask, KSAUDIO_SPEAKER_7POINT1);
    EXPECT_TRUE(IsEqualGUID(format.FormatExt.SubFormat, subformat));
    EXPECT_EQ(format.dwEncodedSamplesPerSec, 96000u);
    EXPECT_EQ(format.dwEncodedChannelCount, 8u);
    EXPECT_EQ(format.dwAverageBytesPerSec, 0u);
  }
}  // namespace

static_assert(sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == 52);

// Catches MAT10 descriptor drift or inference from a newer Dolby MAT subtype.
TEST(AtmosCapabilityProbeWindows, BuildsExactMat10Descriptor) {
  const auto format = atmos_probe::make_mat_format(atmos_probe::mat_profile::mat10);
  expect_exact_mat_fields(format, atmos_probe::k_iec61937_dolby_mlp);
}

// Catches descriptor drift that would make an exact MAT21 WASAPI probe test a different format.
TEST(AtmosCapabilityProbeWindows, BuildsExactMat21Descriptor) {
  const auto format = atmos_probe::make_mat_format(atmos_probe::mat_profile::mat21);
  expect_exact_mat_fields(format, atmos_probe::k_iec61937_dolby_mat21);
}

// Catches profile inference or GUID reuse that makes MAT20 probe with the MAT21 subtype.
TEST(AtmosCapabilityProbeWindows, BuildsExactMat20Descriptor) {
  const auto format = atmos_probe::make_mat_format(atmos_probe::mat_profile::mat20);
  expect_exact_mat_fields(format, atmos_probe::k_iec61937_dolby_mat20);
}

// Catches noncanonical output that makes equivalent spatial-format GUID text compare unequal.
TEST(AtmosCapabilityProbeWindows, CanonicalizesGuidTextToUppercaseBracedForm) {
  EXPECT_EQ(
    atmos_probe::canonical_guid(L"{d1b9cc2a-f519-417f-91c9-55fa65481001}"),
    official_hdmi);
  EXPECT_EQ(atmos_probe::canonical_guid(L"not-a-guid"), std::nullopt);
}

// Catches form-factor handling that accepts missing, a different VARTYPE, or non-display UI4 values.
TEST(AtmosCapabilityProbeWindows, DecodesOnlyUi4DisplayAudioFormFactor) {
  const auto missing = atmos_probe::decode_form_factor(empty_variant());
  EXPECT_TRUE(std::holds_alternative<atmos_probe::property_missing>(missing));

  std::wstring string_value = L"9";
  const auto wrong_type = atmos_probe::decode_form_factor(lpwstr_variant(string_value.data()));
  const auto *wrong = std::get_if<atmos_probe::property_wrong_type>(&wrong_type);
  ASSERT_NE(wrong, nullptr);
  EXPECT_EQ(wrong->variant_type, VT_LPWSTR);

  const auto display = atmos_probe::decode_form_factor(
    ui4_variant(static_cast<ULONG>(DigitalAudioDisplayDevice)));
  EXPECT_TRUE(std::holds_alternative<atmos_probe::display_audio_form_factor>(display));

  const auto other = atmos_probe::decode_form_factor(ui4_variant(static_cast<ULONG>(SPDIF)));
  const auto *other_value = std::get_if<atmos_probe::other_form_factor>(&other);
  ASSERT_NE(other_value, nullptr);
  EXPECT_EQ(other_value->value, static_cast<ULONG>(SPDIF));
}

// Catches treating the documented LPWSTR endpoint property as a VT_CLSID property.
TEST(AtmosCapabilityProbeWindows, RejectsClsidJackSubtypeVariant) {
  GUID guid = atmos_probe::k_hdmi_interface;
  auto value = empty_variant();
  value.vt = VT_CLSID;
  value.puuid = &guid;

  const auto decoded = atmos_probe::decode_jack_subtype(value);
  const auto *wrong = std::get_if<atmos_probe::property_wrong_type>(&decoded);
  ASSERT_NE(wrong, nullptr);
  EXPECT_EQ(wrong->variant_type, VT_CLSID);
}

// Catches conflation of an absent property, null string, and malformed connector text.
TEST(AtmosCapabilityProbeWindows, DistinguishesMissingNullAndMalformedJackSubtype) {
  EXPECT_TRUE(std::holds_alternative<atmos_probe::property_missing>(
    atmos_probe::decode_jack_subtype(empty_variant())));

  const auto null_string = atmos_probe::decode_jack_subtype(lpwstr_variant(nullptr));
  const auto *null_malformed = std::get_if<atmos_probe::malformed_connector_guid>(&null_string);
  ASSERT_NE(null_malformed, nullptr);
  EXPECT_TRUE(null_malformed->raw.empty());

  std::wstring malformed_text = L"not-a-guid";
  const auto malformed = atmos_probe::decode_jack_subtype(lpwstr_variant(malformed_text.data()));
  const auto *malformed_value = std::get_if<atmos_probe::malformed_connector_guid>(&malformed);
  ASSERT_NE(malformed_value, nullptr);
  EXPECT_EQ(malformed_value->raw, "not-a-guid");
}

// Catches HDMI/DisplayPort confusion and regressions to two historical incorrect GUID literals.
TEST(AtmosCapabilityProbeWindows, AcceptsOnlyOfficialHdmiAndDisplayPortLiterals) {
  struct connector_case {
    std::wstring raw;
    std::string_view canonical;
    enum class expected_kind { hdmi, displayport, other } expected;
  };

  std::array<connector_case, 5> cases {{
    {L"{D1B9CC2A-F519-417F-91C9-55FA65481001}", official_hdmi, connector_case::expected_kind::hdmi},
    {L"{E47E4031-3EA6-418D-8F9B-B73843CCBA97}", official_displayport, connector_case::expected_kind::displayport},
    {L"{D9E55EA0-0C89-4692-84FF-EB3C4B0D172F}", historical_wrong_hdmi, connector_case::expected_kind::other},
    {L"{E47E4031-3EA6-418D-8F9B-B73843CCB2AD}", historical_wrong_displayport, connector_case::expected_kind::other},
    {L"{00000000-0000-0000-0000-000000000001}", "{00000000-0000-0000-0000-000000000001}", connector_case::expected_kind::other},
  }};

  for (auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.canonical});
    const auto decoded = atmos_probe::decode_jack_subtype(lpwstr_variant(test_case.raw.data()));
    switch (test_case.expected) {
      case connector_case::expected_kind::hdmi:
        EXPECT_TRUE(std::holds_alternative<atmos_probe::hdmi_connector>(decoded));
        break;
      case connector_case::expected_kind::displayport:
        EXPECT_TRUE(std::holds_alternative<atmos_probe::displayport_connector>(decoded));
        break;
      case connector_case::expected_kind::other: {
        const auto *other = std::get_if<atmos_probe::other_connector_guid>(&decoded);
        ASSERT_NE(other, nullptr);
        EXPECT_EQ(other->canonical_guid, test_case.canonical);
        break;
      }
    }
  }
}
