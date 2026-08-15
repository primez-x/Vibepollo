#include "../tests_common.h"
#include "tools/atmos_capability_probe_cli.h"

#include <array>
#include <string_view>

namespace {
  using atmos_probe::cli_parse_result;

  void expect_options(
    const cli_parse_result &result,
    const bool json,
    const bool help,
    const std::optional<std::string_view> endpoint_id) {
    ASSERT_TRUE(result.options.has_value());
    EXPECT_TRUE(result.error.empty());
    EXPECT_EQ(result.options->json, json);
    EXPECT_EQ(result.options->help, help);
    if (endpoint_id) {
      ASSERT_TRUE(result.options->endpoint_id.has_value());
      EXPECT_EQ(*result.options->endpoint_id, *endpoint_id);
    } else {
      EXPECT_FALSE(result.options->endpoint_id.has_value());
    }
  }

  void expect_error(const cli_parse_result &result, const std::string_view error) {
    EXPECT_FALSE(result.options.has_value());
    EXPECT_EQ(result.error, error);
  }
}  // namespace

// Catches a parser regression that changes the no-argument default away from human eConsole mode.
TEST(AtmosCapabilityProbeCli, DefaultsToHumanConsoleSelectionWithoutArguments) {
  const std::array<std::string_view, 0> arguments {};

  expect_options(atmos_probe::parse_cli(arguments), false, false, std::nullopt);
}

// Catches a parser regression that fails to preserve JSON mode while retaining the default selection.
TEST(AtmosCapabilityProbeCli, EnablesJsonWithoutChangingTheDefaultSelection) {
  const std::array arguments {std::string_view {"--json"}};

  expect_options(atmos_probe::parse_cli(arguments), true, false, std::nullopt);
}

// Catches endpoint normalization or truncation before the provider receives the explicit ID.
TEST(AtmosCapabilityProbeCli, PreservesExplicitEndpointIdByteForByte) {
  constexpr std::string_view endpoint_id = "{0.0.0.00000000}.{exact-id=Bytes[1]}";
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    endpoint_id,
    std::string_view {"--json"},
  };

  expect_options(atmos_probe::parse_cli(arguments), true, false, endpoint_id);
}

// Catches a help branch that is not represented in the parser result for the app coordinator.
TEST(AtmosCapabilityProbeCli, RecognizesHelp) {
  const std::array arguments {std::string_view {"--help"}};

  expect_options(atmos_probe::parse_cli(arguments), false, true, std::nullopt);
}

// Catches treating a missing endpoint argument as an empty endpoint ID.
TEST(AtmosCapabilityProbeCli, RejectsEndpointOptionWithoutValue) {
  const std::array arguments {std::string_view {"--endpoint-id"}};

  expect_error(
    atmos_probe::parse_cli(arguments),
    "--endpoint-id requires a value");
}

// Catches ambiguity that would let a later endpoint option silently override the requested route.
TEST(AtmosCapabilityProbeCli, RejectsDuplicateEndpointOptions) {
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    std::string_view {"first"},
    std::string_view {"--endpoint-id"},
    std::string_view {"second"},
  };

  expect_error(
    atmos_probe::parse_cli(arguments),
    "--endpoint-id may be specified once");
}

// Catches accepting an unrecognized option that could otherwise change a preflight invocation silently.
TEST(AtmosCapabilityProbeCli, RejectsUnknownOptions) {
  const std::array arguments {std::string_view {"--bogus"}};

  expect_error(atmos_probe::parse_cli(arguments), "unknown option: --bogus");
}
