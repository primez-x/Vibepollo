/**
 * @file tests/unit/test_audio_visibility_recovery.cpp
 * @brief Deterministic tests for the Steam audio endpoint visibility recovery decisions.
 */
#include "../tests_common.h"

#include <src/platform/windows/audio_visibility_recovery.h>
#include <string>

using namespace platf::audio::visibility_recovery;

TEST(AudioVisibilityRecovery, MarkerRoundTripsDeviceId) {
  const std::string device_id = "{0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}";
  const auto parsed = parse_marker(make_marker(device_id));
  ASSERT_TRUE(parsed);
  EXPECT_EQ(*parsed, device_id);
}

TEST(AudioVisibilityRecovery, ParseToleratesLineEndingsAndWhitespace) {
  const auto crlf = parse_marker("  {device-id} \r\n");
  ASSERT_TRUE(crlf);
  EXPECT_EQ(*crlf, "{device-id}");

  const auto multi_line = parse_marker("{device-id}\r\nextra trailing data");
  ASSERT_TRUE(multi_line);
  EXPECT_EQ(*multi_line, "{device-id}");
}

TEST(AudioVisibilityRecovery, ParseRejectsUnusableContent) {
  EXPECT_FALSE(parse_marker(""));
  EXPECT_FALSE(parse_marker("\r\n"));
  EXPECT_FALSE(parse_marker("   \t\n"));
  EXPECT_FALSE(parse_marker(std::string(max_marker_size + 1, 'x')));
}

TEST(AudioVisibilityRecovery, ClassifyHealDecisions) {
  // Without a marker no transition was interrupted, whatever the device state.
  EXPECT_EQ(classify_heal(false, false, false), heal_action_e::none);
  EXPECT_EQ(classify_heal(false, true, false), heal_action_e::none);
  EXPECT_EQ(classify_heal(false, true, true), heal_action_e::none);

  // A missing endpoint may appear later; the marker must survive until then.
  EXPECT_EQ(classify_heal(true, false, false), heal_action_e::retain_marker);

  // An active endpoint means the transition completed; the marker is stale.
  EXPECT_EQ(classify_heal(true, true, true), heal_action_e::clear_marker);

  // A present but inactive endpoint is the interrupted hide to undo.
  EXPECT_EQ(classify_heal(true, true, false), heal_action_e::reshow_endpoint);
}

TEST(AudioVisibilityRecovery, NoMoreItemsRequiresConfirmedExistingEndpoint) {
  using platf::audio::visibility_recovery::should_recover_existing_steam_endpoint;

  EXPECT_TRUE(should_recover_existing_steam_endpoint(true, true, true, true));
  EXPECT_FALSE(should_recover_existing_steam_endpoint(true, false, true, true));
  EXPECT_FALSE(should_recover_existing_steam_endpoint(true, true, false, true));
  EXPECT_FALSE(should_recover_existing_steam_endpoint(true, true, true, false));
  EXPECT_FALSE(should_recover_existing_steam_endpoint(false, true, true, true));
}

TEST(AudioVisibilityRecovery, RefreshesStalePolicyClientAfterFalseSuccess) {
  using platf::audio::visibility_recovery::should_refresh_policy_client;

  EXPECT_TRUE(should_refresh_policy_client(true, true, false));
  EXPECT_FALSE(should_refresh_policy_client(true, false, false));
  EXPECT_FALSE(should_refresh_policy_client(true, true, true));
  EXPECT_FALSE(should_refresh_policy_client(false, true, false));
}

TEST(AudioVisibilityRecovery, RegistryCandidateRejectsSteamAuxJack) {
  using platf::audio::visibility_recovery::is_registered_steam_speaker_candidate;

  EXPECT_TRUE(is_registered_steam_speaker_candidate(true, true, true));
  EXPECT_FALSE(is_registered_steam_speaker_candidate(false, false, true));
  EXPECT_FALSE(is_registered_steam_speaker_candidate(true, false, true));
  EXPECT_FALSE(is_registered_steam_speaker_candidate(true, true, false));
}
