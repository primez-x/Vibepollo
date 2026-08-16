#include "../tests_common.h"
#include "src/audio.h"

#include <array>
#include <vector>

namespace {
  TEST(AtmosMatRuntime, FragmentsRecordsIntoFixedSizedValidatedShards) {
    std::vector<std::uint8_t> record(1'305, 0xa5);
    const auto fragments = audio::mat10_fragment_record(9, record);
    ASSERT_EQ(fragments.size(), 2);
    EXPECT_EQ(fragments[0].size(), audio::mat10_fragment_bytes);
    EXPECT_EQ(fragments[1].size(), audio::mat10_fragment_bytes);

    const auto first = audio::parse_mat10_fragment(fragments[0]);
    const auto last = audio::parse_mat10_fragment(fragments[1]);
    ASSERT_TRUE(first);
    ASSERT_TRUE(last);
    EXPECT_EQ(first->record_sequence, 9U);
    EXPECT_EQ(first->record_bytes, record.size());
    EXPECT_EQ(first->fragment_offset, 0U);
    EXPECT_EQ(first->fragment_bytes, audio::mat10_fragment_data_bytes);
    EXPECT_EQ(first->flags, audio::mat10_fragment_first);
    EXPECT_EQ(last->fragment_offset, audio::mat10_fragment_data_bytes);
    EXPECT_EQ(last->fragment_bytes, 1U);
    EXPECT_EQ(last->flags, audio::mat10_fragment_last);
  }

  TEST(AtmosMatRuntime, RejectsInvalidHeadersAndNonzeroPadding) {
    std::vector<std::uint8_t> record {1, 2, 3};
    auto fragment = audio::mat10_fragment_record(1, record).front();
    EXPECT_TRUE(audio::parse_mat10_fragment(fragment));
    fragment.back() = 1;
    EXPECT_FALSE(audio::parse_mat10_fragment(fragment));
    fragment = audio::mat10_fragment_record(1, record).front();
    fragment[0] = 0;
    EXPECT_FALSE(audio::parse_mat10_fragment(fragment));
  }

  TEST(AtmosMatRuntime, RejectsFragmentGapsAndSequenceChanges) {
    std::vector<std::uint8_t> record(1'305, 0xa5);
    const auto fragments = audio::mat10_fragment_record(4, record);
    audio::mat10_fragment_sequence_policy_t policy;
    ASSERT_TRUE(policy.admit(*audio::parse_mat10_fragment(fragments[0])));
    EXPECT_FALSE(policy.admit(*audio::parse_mat10_fragment(fragments[0])));
    EXPECT_TRUE(policy.poisoned());

    policy.reset();
    ASSERT_TRUE(policy.admit(*audio::parse_mat10_fragment(fragments[0])));
    auto sequence_change = audio::mat10_fragment_record(5, record).at(1);
    EXPECT_FALSE(policy.admit(*audio::parse_mat10_fragment(sequence_change)));
    EXPECT_TRUE(policy.poisoned());
  }
}
