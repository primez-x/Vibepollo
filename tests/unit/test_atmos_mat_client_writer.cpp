#include "../tests_common.h"
#include "tools/atmos_mat_client_writer.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {
  atmos_mat_client::descriptor_identity mat10_descriptor() {
    atmos_mat_client::descriptor_identity value {};
    value.bytes[0] = 0xfe;  // Tests identity by exact bytes, not subtype alone.
    value.bytes[51] = 0x10;
    return value;
  }

  atmos_mat_client::carrier_block block(
    const std::uint64_t generation,
    const std::uint64_t first_frame,
    const std::size_t frames) {
    atmos_mat_client::carrier_block value;
    value.generation = generation;
    value.first_carrier_frame = first_frame;
    value.host_qpc = 1000;
    value.host_qpc_frequency = 10'000'000;
    value.descriptor = mat10_descriptor();
    value.bytes.resize(frames * atmos_mat_client::carrier_frame_bytes, 0xa5);
    return value;
  }

  TEST(AtmosMatClientWriter, AcceptsContiguousBlocksAndPreservesBytes) {
    atmos_mat_client::writer writer {mat10_descriptor(), 8};
    const auto input = block(7, 44, 2);
    EXPECT_EQ(writer.enqueue(input), atmos_mat_client::enqueue_result::accepted);

    std::vector<std::uint8_t> output(input.bytes.size());
    const auto copied = writer.copy_next(output);
    EXPECT_EQ(copied.status, atmos_mat_client::copy_status::copied);
    EXPECT_EQ(copied.frames, 2);
    EXPECT_EQ(output, input.bytes);
    EXPECT_EQ(writer.submitted_frames(), 2);
  }

  TEST(AtmosMatClientWriter, GapPoisoningNeverInsertsSilenceOrReplays) {
    atmos_mat_client::writer writer {mat10_descriptor(), 8};
    EXPECT_EQ(writer.enqueue(block(9, 0, 1)), atmos_mat_client::enqueue_result::accepted);
    EXPECT_EQ(writer.enqueue(block(9, 2, 1)), atmos_mat_client::enqueue_result::gap_poisoned);
    EXPECT_EQ(writer.poison(), atmos_mat_client::poison_reason::carrier_gap);
    std::array<std::uint8_t, atmos_mat_client::carrier_frame_bytes> output {};
    EXPECT_EQ(writer.copy_next(output).status, atmos_mat_client::copy_status::poisoned);
  }

  TEST(AtmosMatClientWriter, CopiesAcrossContiguousBlockBoundariesWithoutMutation) {
    atmos_mat_client::writer writer {mat10_descriptor(), 8};
    auto first = block(8, 100, 1);
    auto second = block(8, 101, 1);
    second.bytes[0] = 0x3c;
    EXPECT_EQ(writer.enqueue(first), atmos_mat_client::enqueue_result::accepted);
    EXPECT_EQ(writer.enqueue(second), atmos_mat_client::enqueue_result::accepted);
    std::array<std::uint8_t, atmos_mat_client::carrier_frame_bytes * 2> output {};
    EXPECT_EQ(writer.copy_next(output).frames, 2);
    EXPECT_TRUE(std::equal(first.bytes.begin(), first.bytes.end(), output.begin()));
    EXPECT_TRUE(std::equal(second.bytes.begin(), second.bytes.end(), output.begin() + first.bytes.size()));
  }

  TEST(AtmosMatClientWriter, GenerationDescriptorAndCapacityFailClosed) {
    atmos_mat_client::writer writer {mat10_descriptor(), 1};
    EXPECT_EQ(writer.enqueue(block(3, 0, 1)), atmos_mat_client::enqueue_result::accepted);
    EXPECT_EQ(writer.enqueue(block(3, 1, 1)), atmos_mat_client::enqueue_result::queue_full_poisoned);
    EXPECT_EQ(writer.poison(), atmos_mat_client::poison_reason::queue_full);

    writer.reset(4);
    auto wrong = block(4, 0, 1);
    wrong.descriptor.bytes[12] = 1;
    EXPECT_EQ(writer.enqueue(wrong), atmos_mat_client::enqueue_result::descriptor_mismatch_poisoned);
    EXPECT_EQ(writer.poison(), atmos_mat_client::poison_reason::descriptor_mismatch);
  }

  TEST(AtmosMatClientWriter, ProducerAndConsumerPreserveEachContiguousBlock) {
    constexpr std::uint64_t block_count = 64;
    atmos_mat_client::writer writer {mat10_descriptor(), 1};
    std::mutex turn_mutex;
    std::condition_variable turn_changed;
    bool producer_turn = true;
    std::uint64_t produced {};
    std::uint64_t consumed {};
    bool all_enqueued = true;
    bool all_copied = true;
    std::vector<std::array<std::uint8_t, atmos_mat_client::carrier_frame_bytes>> received(block_count);

    std::thread producer {[&] {
      for (std::uint64_t index = 0; index < block_count; ++index) {
        std::unique_lock lock {turn_mutex};
        turn_changed.wait(lock, [&] { return producer_turn; });
        lock.unlock();
        auto input = block(77, index, 1);
        input.bytes[0] = static_cast<std::uint8_t>(index);
        all_enqueued = all_enqueued &&
          writer.enqueue(std::move(input)) == atmos_mat_client::enqueue_result::accepted;
        lock.lock();
        ++produced;
        producer_turn = false;
        turn_changed.notify_one();
      }
    }};
    std::thread consumer {[&] {
      for (std::uint64_t index = 0; index < block_count; ++index) {
        std::unique_lock lock {turn_mutex};
        turn_changed.wait(lock, [&] { return !producer_turn && consumed < produced; });
        lock.unlock();
        all_copied = all_copied &&
          writer.copy_next(received[index]).status == atmos_mat_client::copy_status::copied;
        lock.lock();
        ++consumed;
        producer_turn = true;
        turn_changed.notify_one();
      }
    }};
    producer.join();
    consumer.join();
    EXPECT_TRUE(all_enqueued);
    EXPECT_TRUE(all_copied);
    EXPECT_EQ(writer.poison(), atmos_mat_client::poison_reason::none);
    EXPECT_EQ(writer.submitted_frames(), block_count);
    for (std::uint64_t index = 0; index < block_count; ++index) {
      EXPECT_EQ(received[index][0], static_cast<std::uint8_t>(index));
      EXPECT_TRUE(std::all_of(received[index].begin() + 1, received[index].end(),
        [](const std::uint8_t byte) { return byte == 0xa5; }));
    }
  }
}
