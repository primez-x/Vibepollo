/**
 * @file src/audio_policy.cpp
 * @brief Platform-neutral audio stream and capture decisions.
 */
#include "audio_policy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace audio::policy {
  role_lane_background_transition_t select_role_lane_background(
    bool observer_ready,
    bool restore_ready,
    bool prefer_observer
  ) {
    if (!observer_ready && !restore_ready) {
      return {role_lane_background_selection_e::none, prefer_observer};
    }
    const bool choose_observer = observer_ready &&
      (!restore_ready || prefer_observer);
    return {
      choose_observer ?
        role_lane_background_selection_e::observer :
        role_lane_background_selection_e::restore,
      observer_ready && restore_ready ? !prefer_observer : prefer_observer,
    };
  }

  granted_restore_phase_action_e granted_restore_phase_action(
    bool stop_requested,
    bool worker_active
  ) {
    return !stop_requested && worker_active ?
      granted_restore_phase_action_e::run :
      granted_restore_phase_action_e::cancel;
  }

  std::optional<std::uint64_t> advance_policy_assignment_epoch(
    std::uint64_t current_epoch,
    const std::function<bool()> &rotate_runtime
  ) {
    if (current_epoch != std::numeric_limits<std::uint64_t>::max()) {
      return current_epoch + 1;
    }
    if (!rotate_runtime || !rotate_runtime()) {
      return std::nullopt;
    }
    return 1;
  }

  struct fixed_role_lane_coordinator_t::impl_t {
    struct lane_t {
      std::mutex mutex;
      std::condition_variable wake;
      std::optional<foreground_work_t> foreground;
      background_step_t observer_step;
      background_step_t restore_step;
      std::uint64_t observer_generation = 0;
      std::optional<restore_slot_key_t> restore_key;
      std::optional<restore_slot_key_t> restore_floor;
      bool accepting = true;
      bool dispatch_suspended = false;
      bool prefer_observer = true;
      std::size_t consecutive_foreground = 0;
      std::jthread worker;
    };

    std::array<std::unique_ptr<lane_t>, role_count> lanes;
    std::atomic_bool stopped {false};

    explicit impl_t(worker_factory_t worker_factory) {
      for (auto &lane : lanes) {
        lane = std::make_unique<lane_t>();
      }
      try {
        for (std::size_t index = 0; index < lanes.size(); ++index) {
          auto *const lane_ptr = lanes[index].get();
          std::function<void(std::stop_token)> entry =
            [lane_ptr](std::stop_token stop_token) {
              std::stop_callback wake_on_stop(stop_token, [lane_ptr]() {
                lane_ptr->wake.notify_all();
              });
              run_lane(*lane_ptr, stop_token);
            };
          lanes[index]->worker = worker_factory ?
            worker_factory(index, std::move(entry)) :
            std::jthread(std::move(entry));
        }
      } catch (...) {
        for (auto &lane : lanes) {
          if (lane->worker.joinable()) {
            lane->worker.request_stop();
            lane->wake.notify_all();
          }
        }
        for (auto &lane : lanes) {
          if (lane->worker.joinable()) {
            lane->worker.join();
          }
        }
        throw;
      }
    }

    static void run_lane(lane_t &lane, std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        std::optional<foreground_work_t> foreground;
        background_step_t background;
        role_lane_background_selection_e selected =
          role_lane_background_selection_e::none;
        std::uint64_t selected_observer_generation = 0;
        std::optional<restore_slot_key_t> selected_restore_key;
        {
          std::unique_lock lock(lane.mutex);
          lane.wake.wait(lock, [&]() {
            return stop_token.stop_requested() || !lane.accepting ||
                   (!lane.dispatch_suspended &&
                    (lane.foreground.has_value() || lane.observer_step ||
                     lane.restore_step));
          });
          if (stop_token.stop_requested() || !lane.accepting) {
            return;
          }
          if (lane.dispatch_suspended) {
            continue;
          }
          const bool have_observer = static_cast<bool>(lane.observer_step);
          const bool have_restore = static_cast<bool>(lane.restore_step);
          const bool have_background = have_observer || have_restore;
          const bool choose_foreground = lane.foreground &&
            (!have_background ||
             lane.consecutive_foreground <
               max_consecutive_foreground_before_background);
          if (choose_foreground) {
            foreground = std::move(lane.foreground);
            lane.foreground.reset();
            lane.consecutive_foreground = std::min(
              lane.consecutive_foreground + 1,
              max_consecutive_foreground_before_background
            );
          } else {
            lane.consecutive_foreground = 0;
            const auto transition = select_role_lane_background(
              have_observer,
              have_restore,
              lane.prefer_observer
            );
            lane.prefer_observer = transition.next_prefer_observer;
            selected = transition.selection;
            if (selected == role_lane_background_selection_e::observer) {
              selected_observer_generation = lane.observer_generation;
              background = lane.observer_step;
            } else {
              selected_restore_key = lane.restore_key;
              background = lane.restore_step;
            }
          }
        }

        if (foreground) {
          try {
            foreground->run(stop_token);
          } catch (...) {
          }
          continue;
        }

        bool keep = false;
        bool background_failed = false;
        try {
          keep = background(stop_token);
        } catch (...) {
          keep = true;
          background_failed = true;
        }
        if (background_failed) {
          std::unique_lock lock(lane.mutex);
          lane.wake.wait_for(lock, std::chrono::milliseconds(100), [&]() {
            return stop_token.stop_requested() || !lane.accepting ||
                   lane.foreground.has_value();
          });
          if (stop_token.stop_requested() || !lane.accepting) {
            keep = false;
          }
        }
        {
          std::scoped_lock lock(lane.mutex);
          if (selected == role_lane_background_selection_e::observer &&
              lane.observer_generation == selected_observer_generation && !keep) {
            lane.observer_step = {};
          } else if (selected == role_lane_background_selection_e::restore &&
                     lane.restore_key == selected_restore_key && !keep) {
            lane.restore_step = {};
          }
        }
      }
    }

    bool submit(std::size_t role_index, foreground_work_t work) {
      if (role_index >= lanes.size() || !work.run ||
          stopped.load(std::memory_order_acquire)) {
        return false;
      }
      std::function<void()> supersede;
      auto &lane = *lanes[role_index];
      {
        std::scoped_lock lock(lane.mutex);
        if (!lane.accepting) {
          return false;
        }
        if (lane.foreground) {
          supersede = std::move(lane.foreground->supersede);
        }
        lane.foreground = std::move(work);
      }
      if (supersede) {
        supersede();
      }
      lane.wake.notify_one();
      return true;
    }

    bool set_observer(
      std::size_t role_index,
      background_step_t step,
      bool replace
    ) {
      if (role_index >= lanes.size() || !step ||
          stopped.load(std::memory_order_acquire)) {
        return false;
      }
      auto &lane = *lanes[role_index];
      {
        std::scoped_lock lock(lane.mutex);
        if (!lane.accepting) {
          return false;
        }
        if (!replace && lane.observer_step) {
          if (++lane.observer_generation == 0) {
            ++lane.observer_generation;
          }
          lane.wake.notify_one();
          return true;
        }
        lane.observer_step = std::move(step);
        ++lane.observer_generation;
      }
      lane.wake.notify_one();
      return true;
    }

    void clear_observer(std::size_t role_index) {
      if (role_index >= lanes.size()) {
        return;
      }
      auto &lane = *lanes[role_index];
      std::scoped_lock lock(lane.mutex);
      lane.observer_step = {};
      if (++lane.observer_generation == 0) {
        ++lane.observer_generation;
      }
    }

    static bool key_is_older(
      const restore_slot_key_t &candidate,
      const restore_slot_key_t &reference
    ) noexcept {
      return candidate.runtime_generation < reference.runtime_generation ||
             (candidate.runtime_generation == reference.runtime_generation &&
              candidate.assignment_epoch < reference.assignment_epoch);
    }

    restore_slot_install_result_e set_restore(
      std::size_t role_index,
      restore_slot_key_t key,
      background_step_t step
    ) {
      if (role_index >= lanes.size() || !step || key.runtime_generation == 0 ||
          key.assignment_epoch == 0 ||
          stopped.load(std::memory_order_acquire)) {
        return restore_slot_install_result_e::unavailable;
      }
      auto &lane = *lanes[role_index];
      {
        std::scoped_lock lock(lane.mutex);
        if (!lane.accepting) {
          return restore_slot_install_result_e::unavailable;
        }
        if ((lane.restore_floor && key_is_older(key, *lane.restore_floor)) ||
            (lane.restore_key && key_is_older(key, *lane.restore_key))) {
          return restore_slot_install_result_e::rejected_older;
        }
        if (lane.restore_key && key == *lane.restore_key) {
          return restore_slot_install_result_e::unchanged_equal;
        }
        lane.restore_key = key;
        lane.restore_step = std::move(step);
      }
      lane.wake.notify_one();
      return restore_slot_install_result_e::installed;
    }

    bool clear_restore(std::size_t role_index, restore_slot_key_t key) {
      if (role_index >= lanes.size()) {
        return false;
      }
      auto &lane = *lanes[role_index];
      std::scoped_lock lock(lane.mutex);
      if (!lane.restore_step || lane.restore_key != key) {
        return false;
      }
      lane.restore_step = {};
      return true;
    }

    std::size_t clear_restores_before(restore_slot_key_t threshold) {
      if (threshold.runtime_generation == 0 || threshold.assignment_epoch == 0) {
        return 0;
      }
      std::size_t cleared = 0;
      for (auto &lane_ptr : lanes) {
        auto &lane = *lane_ptr;
        std::scoped_lock lock(lane.mutex);
        if (!lane.restore_floor || key_is_older(*lane.restore_floor, threshold)) {
          lane.restore_floor = threshold;
        }
        if (lane.restore_step && lane.restore_key &&
            lane.restore_key->runtime_generation == threshold.runtime_generation &&
            lane.restore_key->assignment_epoch < threshold.assignment_epoch) {
          lane.restore_step = {};
          ++cleared;
        }
      }
      return cleared;
    }

    void set_dispatch_suspended(bool suspended) noexcept {
      for (auto &lane_ptr : lanes) {
        auto &lane = *lane_ptr;
        {
          std::scoped_lock lock(lane.mutex);
          lane.dispatch_suspended = suspended;
        }
        if (!suspended) {
          lane.wake.notify_one();
        }
      }
    }

    void shutdown() noexcept {
      if (stopped.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      std::array<std::function<void()>, role_count> supersede;
      for (std::size_t index = 0; index < lanes.size(); ++index) {
        auto &lane = *lanes[index];
        {
          std::scoped_lock lock(lane.mutex);
          lane.accepting = false;
          if (lane.foreground) {
            supersede[index] = std::move(lane.foreground->supersede);
            lane.foreground.reset();
          }
          lane.observer_step = {};
          lane.restore_step = {};
        }
        lane.worker.request_stop();
        lane.wake.notify_all();
      }
      for (auto &callback : supersede) {
        if (callback) {
          callback();
        }
      }
      for (auto &lane : lanes) {
        if (lane->worker.joinable()) {
          lane->worker.join();
        }
      }
    }
  };

  fixed_role_lane_coordinator_t::fixed_role_lane_coordinator_t():
      fixed_role_lane_coordinator_t(worker_factory_t {}) {
  }

  fixed_role_lane_coordinator_t::fixed_role_lane_coordinator_t(
    worker_factory_t worker_factory
  ):
      impl_ {std::make_unique<impl_t>(std::move(worker_factory))} {
  }

  fixed_role_lane_coordinator_t::~fixed_role_lane_coordinator_t() {
    shutdown();
  }

  bool fixed_role_lane_coordinator_t::submit_foreground(
    std::size_t role_index,
    foreground_work_t work
  ) {
    return impl_->submit(role_index, std::move(work));
  }

  bool fixed_role_lane_coordinator_t::set_observer_step(
    std::size_t role_index,
    background_step_t step
  ) {
    return impl_->set_observer(role_index, std::move(step), true);
  }

  bool fixed_role_lane_coordinator_t::ensure_observer_step(
    std::size_t role_index,
    background_step_t step
  ) {
    return impl_->set_observer(role_index, std::move(step), false);
  }

  restore_slot_install_result_e fixed_role_lane_coordinator_t::set_restore_step(
    std::size_t role_index,
    restore_slot_key_t key,
    background_step_t step
  ) {
    return impl_->set_restore(role_index, key, std::move(step));
  }

  void fixed_role_lane_coordinator_t::clear_observer_step(std::size_t role_index) {
    impl_->clear_observer(role_index);
  }

  bool fixed_role_lane_coordinator_t::clear_restore_step(
    std::size_t role_index,
    restore_slot_key_t key
  ) {
    return impl_->clear_restore(role_index, key);
  }

  std::size_t fixed_role_lane_coordinator_t::clear_restore_steps_before(
    restore_slot_key_t threshold
  ) {
    return impl_->clear_restores_before(threshold);
  }

  void fixed_role_lane_coordinator_t::suspend_dispatch() noexcept {
    if (impl_) {
      impl_->set_dispatch_suspended(true);
    }
  }

  void fixed_role_lane_coordinator_t::resume_dispatch() noexcept {
    if (impl_) {
      impl_->set_dispatch_suspended(false);
    }
  }

  void fixed_role_lane_coordinator_t::shutdown() noexcept {
    if (impl_) {
      impl_->shutdown();
    }
  }

  std::size_t fixed_role_lane_coordinator_t::worker_capacity() const noexcept {
    return role_count;
  }

  bool fixed_role_lane_coordinator_t::is_current_lane(
    std::size_t role_index
  ) const noexcept {
    return impl_ && role_index < impl_->lanes.size() &&
           impl_->lanes[role_index]->worker.get_id() == std::this_thread::get_id();
  }

  std::optional<std::size_t>
  fixed_role_lane_coordinator_t::current_lane_index() const noexcept {
    if (!impl_) {
      return std::nullopt;
    }
    const auto current_id = std::this_thread::get_id();
    for (std::size_t index = 0; index < impl_->lanes.size(); ++index) {
      if (impl_->lanes[index]->worker.get_id() == current_id) {
        return index;
      }
    }
    return std::nullopt;
  }

  bool fixed_role_lane_coordinator_t::has_pending_foreground(
    std::size_t role_index
  ) const noexcept {
    if (!impl_ || role_index >= impl_->lanes.size()) {
      return false;
    }
    auto &lane = *impl_->lanes[role_index];
    std::scoped_lock lock(lane.mutex);
    return lane.foreground.has_value();
  }

  struct fixed_role_lane_runtime_t::impl_t {
    mutable std::mutex mutex;
    std::condition_variable quiesced;
    coordinator_factory_t coordinator_factory;
    runtime_created_callback_t runtime_created_callback;
    std::shared_ptr<fixed_role_lane_coordinator_t> runtime;
    std::size_t owner_count = 0;
    std::uint64_t generation = 0;
    bool enabled = false;
    bool creating = false;
    bool shutting_down = false;

    explicit impl_t(
      coordinator_factory_t factory,
      runtime_created_callback_t created_callback
    ):
        coordinator_factory {std::move(factory)},
        runtime_created_callback {std::move(created_callback)} {
      if (!coordinator_factory) {
        coordinator_factory = []() {
          return std::make_shared<fixed_role_lane_coordinator_t>();
        };
      }
    }

    bool activate() {
      std::unique_lock lock(mutex);
      quiesced.wait(lock, [&]() { return !shutting_down; });
      const bool first_owner = owner_count++ == 0;
      enabled = true;
      return first_owner;
    }

    bool release() noexcept {
      std::shared_ptr<fixed_role_lane_coordinator_t> retiring;
      {
        std::unique_lock lock(mutex);
        quiesced.wait(lock, [&]() { return !shutting_down; });
        if (owner_count == 0) {
          return false;
        }
        if (--owner_count != 0) {
          return false;
        }
        enabled = false;
        shutting_down = true;
        quiesced.wait(lock, [&]() { return !creating; });
        retiring = std::move(runtime);
      }
      if (retiring) {
        retiring->shutdown();
      }
      {
        std::scoped_lock lock(mutex);
        shutting_down = false;
      }
      quiesced.notify_all();
      return true;
    }

    std::shared_ptr<fixed_role_lane_coordinator_t> get(
      bool *created,
      std::uint64_t *runtime_generation
    ) {
      if (created) {
        *created = false;
      }
      if (runtime_generation) {
        *runtime_generation = 0;
      }
      std::uint64_t proposed_generation = 0;
      {
        std::unique_lock lock(mutex);
        quiesced.wait(lock, [&]() {
          return !creating || !enabled || shutting_down;
        });
        if (!enabled || shutting_down) {
          return {};
        }
        if (runtime) {
          if (runtime_generation) {
            *runtime_generation = generation;
          }
          return runtime;
        }
        if (generation == std::numeric_limits<std::uint64_t>::max()) {
          return {};
        }
        proposed_generation = generation + 1;
        creating = true;
      }

      std::shared_ptr<fixed_role_lane_coordinator_t> candidate;
      bool replay_succeeded = false;
      try {
        candidate = coordinator_factory();
        if (candidate) {
          candidate->suspend_dispatch();
          if (runtime_created_callback) {
            runtime_created_callback(candidate, proposed_generation);
          }
          replay_succeeded = true;
        }
      } catch (...) {
      }

      bool published = false;
      if (candidate && replay_succeeded) {
        std::scoped_lock lock(mutex);
        if (enabled && !shutting_down && !runtime) {
          runtime = candidate;
          generation = proposed_generation;
          published = true;
        }
      }
      if (published) {
        candidate->resume_dispatch();
      } else if (candidate) {
        candidate->shutdown();
      }
      {
        std::scoped_lock lock(mutex);
        creating = false;
      }
      quiesced.notify_all();

      if (!published) {
        return {};
      }
      if (created) {
        *created = true;
      }
      if (runtime_generation) {
        *runtime_generation = proposed_generation;
      }
      return candidate;
    }

    bool rotate(std::uint64_t *runtime_generation) {
      if (runtime_generation) {
        *runtime_generation = 0;
      }
      std::shared_ptr<fixed_role_lane_coordinator_t> retiring;
      {
        std::unique_lock lock(mutex);
        quiesced.wait(lock, [&]() {
          return !shutting_down && !creating;
        });
        if (!enabled) {
          return false;
        }
        shutting_down = true;
        retiring = std::move(runtime);
      }
      if (retiring) {
        retiring->shutdown();
      }
      {
        std::scoped_lock lock(mutex);
        shutting_down = false;
      }
      quiesced.notify_all();
      return static_cast<bool>(get(nullptr, runtime_generation));
    }

    void shutdown_all() noexcept {
      std::shared_ptr<fixed_role_lane_coordinator_t> retiring;
      {
        std::unique_lock lock(mutex);
        owner_count = 0;
        enabled = false;
        shutting_down = true;
        quiesced.wait(lock, [&]() { return !creating; });
        retiring = std::move(runtime);
      }
      if (retiring) {
        retiring->shutdown();
      }
      {
        std::scoped_lock lock(mutex);
        shutting_down = false;
      }
      quiesced.notify_all();
    }
  };

  fixed_role_lane_runtime_t::fixed_role_lane_runtime_t():
      fixed_role_lane_runtime_t(coordinator_factory_t {}, runtime_created_callback_t {}) {
  }

  fixed_role_lane_runtime_t::fixed_role_lane_runtime_t(
    coordinator_factory_t coordinator_factory,
    runtime_created_callback_t runtime_created_callback
  ):
      impl_ {std::make_unique<impl_t>(
        std::move(coordinator_factory),
        std::move(runtime_created_callback)
      )} {
  }

  fixed_role_lane_runtime_t::~fixed_role_lane_runtime_t() {
    if (impl_) {
      impl_->shutdown_all();
    }
  }

  bool fixed_role_lane_runtime_t::activate() {
    return impl_->activate();
  }

  bool fixed_role_lane_runtime_t::release() noexcept {
    return impl_->release();
  }

  std::shared_ptr<fixed_role_lane_coordinator_t>
  fixed_role_lane_runtime_t::get(
    bool *created,
    std::uint64_t *runtime_generation
  ) {
    return impl_->get(created, runtime_generation);
  }

  bool fixed_role_lane_runtime_t::rotate(
    std::uint64_t *runtime_generation
  ) {
    return impl_->rotate(runtime_generation);
  }

  std::uint64_t fixed_role_lane_runtime_t::generation() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->generation;
  }

  bool fixed_role_lane_runtime_t::quiescing() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->shutting_down;
  }

  int stream_index(int channels, bool high_quality) {
    const int quality_offset = high_quality ? 1 : 0;
    switch (channels) {
      case 2:
        return quality_offset;
      case 6:
        return 2 + quality_offset;
      case 8:
        return 4 + quality_offset;
      default:
        return 0;
    }
  }

  stream_layout_t apply_custom_layout(stream_layout_t base, const std::optional<stream_layout_t> &custom) {
    return custom.value_or(base);
  }

  std::string select_sink(const sink_catalog_t &catalog,
                          const std::string &configured_sink,
                          int channels,
                          bool host_audio_enabled) {
    std::string selected = configured_sink.empty() ? catalog.host : configured_sink;
    if (!host_audio_enabled || selected.empty()) {
      const std::optional<std::string> *virtual_sink = nullptr;
      switch (channels) {
        case 2:
          virtual_sink = &catalog.stereo;
          break;
        case 6:
          virtual_sink = &catalog.surround51;
          break;
        case 8:
          virtual_sink = &catalog.surround71;
          break;
      }
      if (virtual_sink && *virtual_sink) {
        selected = **virtual_sink;
      }
    }
    return selected;
  }

  bool is_steam_streaming_render_adapter(std::string_view adapter_name) {
    return adapter_name == "Steam Streaming Speakers" ||
           adapter_name == "Steam Streaming Microphone";
  }

  render_endpoint_catalog_t build_render_endpoint_catalog(
    bool discovery_complete,
    const std::vector<render_endpoint_t> &endpoints
  ) {
    render_endpoint_catalog_t catalog {
      discovery_complete,
      endpoints,
      {},
      {},
    };
    for (const auto &endpoint : endpoints) {
      if (!endpoint.active) {
        continue;
      }
      if (endpoint.id.empty() || endpoint.adapter_name.empty()) {
        catalog.complete = false;
        continue;
      }
      if (is_steam_streaming_render_adapter(endpoint.adapter_name)) {
        catalog.steam_endpoint_ids.push_back(endpoint.id);
      } else {
        catalog.eligible_non_steam_endpoint_ids.push_back(endpoint.id);
      }
    }
    return catalog;
  }

  bool is_eligible_non_steam_fallback(
    const render_endpoint_catalog_t &catalog,
    const std::string &device_id
  ) {
    return catalog.complete &&
           !device_id.empty() &&
           std::find(
             catalog.eligible_non_steam_endpoint_ids.begin(),
             catalog.eligible_non_steam_endpoint_ids.end(),
             device_id
           ) != catalog.eligible_non_steam_endpoint_ids.end();
  }

  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids
  ) {
    return select_eligible_non_steam_render_endpoint(catalog, preferred_ids, {});
  }

  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &preferred_ids,
    const std::vector<std::string> &excluded_ids
  ) {
    if (!catalog.complete) {
      return std::nullopt;
    }

    const auto excluded = [&](const std::string &device_id) {
      return std::find(excluded_ids.begin(), excluded_ids.end(), device_id) !=
             excluded_ids.end();
    };
    for (const auto &preferred_id : preferred_ids) {
      if (!excluded(preferred_id) &&
          is_eligible_non_steam_fallback(catalog, preferred_id)) {
        return preferred_id;
      }
    }

    for (const auto &device_id : catalog.eligible_non_steam_endpoint_ids) {
      if (!excluded(device_id)) {
        return device_id;
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> select_eligible_non_steam_render_endpoint(
    const std::vector<render_endpoint_t> &endpoints,
    const std::vector<std::string> &preferred_ids
  ) {
    return select_eligible_non_steam_render_endpoint(
      build_render_endpoint_catalog(true, endpoints),
      preferred_ids
    );
  }

  std::vector<bool> steam_owned_role_mask(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids
  ) {
    std::vector<bool> owned_roles;
    owned_roles.reserve(current_role_ids.size());
    for (const auto &current_id : current_role_ids) {
      owned_roles.push_back(
        !current_id.empty() &&
        std::find(steam_endpoint_ids.begin(), steam_endpoint_ids.end(), current_id) !=
          steam_endpoint_ids.end()
      );
    }
    return owned_roles;
  }

  std::vector<owned_role_snapshot_t> steam_owned_roles_from_snapshot(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids
  ) {
    std::vector<owned_role_snapshot_t> owned_roles;
    for (std::size_t role_index = 0; role_index < current_role_ids.size(); ++role_index) {
      const auto &current_id = current_role_ids[role_index];
      if (!current_id.empty() &&
          std::find(steam_endpoint_ids.begin(), steam_endpoint_ids.end(), current_id) !=
            steam_endpoint_ids.end()) {
        owned_roles.push_back({role_index, current_id});
      }
    }
    return owned_roles;
  }

  bool role_restore_still_owned(
    const owned_role_snapshot_t &owned_role,
    const std::string &current_id
  ) {
    return !owned_role.expected_current_id.empty() &&
           owned_role.expected_current_id == current_id;
  }

  std::optional<std::vector<std::string>> sanitize_captured_role_ids(
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &steam_endpoint_ids,
    const std::string &eligible_non_steam_replacement_id
  ) {
    const bool replacement_is_eligible_non_steam =
      !eligible_non_steam_replacement_id.empty() &&
      std::find(
        steam_endpoint_ids.begin(),
        steam_endpoint_ids.end(),
        eligible_non_steam_replacement_id
      ) == steam_endpoint_ids.end();

    std::vector<std::string> captured_ids = current_role_ids;
    for (std::size_t role_index = 0; role_index < captured_ids.size(); ++role_index) {
      auto &current_id = captured_ids[role_index];
      const bool steam_owned =
        !current_id.empty() &&
        std::find(steam_endpoint_ids.begin(), steam_endpoint_ids.end(), current_id) !=
          steam_endpoint_ids.end();
      if (!steam_owned) {
        continue;
      }
      if (replacement_is_eligible_non_steam) {
        current_id = eligible_non_steam_replacement_id;
      } else if (role_index == 0) {
        return std::nullopt;
      } else {
        current_id.clear();
      }
    }
    return captured_ids;
  }

  std::vector<role_restore_plan_t> plan_steam_role_restores(
    const render_endpoint_catalog_t &catalog,
    const std::vector<std::string> &current_role_ids,
    const std::vector<std::string> &target_role_ids
  ) {
    std::vector<role_restore_plan_t> restores;
    if (!catalog.complete) {
      return restores;
    }
    const auto role_count = std::min(current_role_ids.size(), target_role_ids.size());
    for (std::size_t role_index = 0; role_index < role_count; ++role_index) {
      const auto &current_id = current_role_ids[role_index];
      const auto &target_id = target_role_ids[role_index];
      const bool current_is_steam =
        std::find(catalog.steam_endpoint_ids.begin(), catalog.steam_endpoint_ids.end(), current_id) !=
        catalog.steam_endpoint_ids.end();
      if (current_is_steam && is_eligible_non_steam_fallback(catalog, target_id)) {
        restores.push_back({role_index, current_id, target_id});
      }
    }
    return restores;
  }

  fallback_role_transition_t plan_fallback_role_transition(
    const render_endpoint_catalog_t &catalog,
    const std::string &expected_steam_id,
    const std::string &current_id,
    const std::string &selected_fallback_id
  ) {
    if (current_id.empty() || !catalog.complete) {
      return {
        fallback_role_action_e::keep_steam_for_retry,
        expected_steam_id,
        true,
      };
    }

    const bool expected_is_steam =
      !expected_steam_id.empty() &&
      std::find(
        catalog.steam_endpoint_ids.begin(),
        catalog.steam_endpoint_ids.end(),
        expected_steam_id
      ) != catalog.steam_endpoint_ids.end();
    if (expected_is_steam &&
        current_id == expected_steam_id &&
        is_eligible_non_steam_fallback(catalog, selected_fallback_id)) {
      return {
        fallback_role_action_e::assign_selected_fallback,
        expected_steam_id,
        true,
      };
    }

    return {
      fallback_role_action_e::adopt_and_retire,
      {},
      false,
    };
  }

  fallback_commit_transition_t complete_fallback_role_transition(
    const std::string &committed_expected_id,
    const std::string &inflight_target_id,
    bool set_succeeded,
    const std::optional<std::string> &readback_id
  ) {
    if (!set_succeeded || !readback_id || readback_id->empty()) {
      return {
        fallback_commit_action_e::retry_committed,
        committed_expected_id,
        true,
      };
    }
    if (*readback_id == inflight_target_id) {
      return {
        fallback_commit_action_e::commit_fallback,
        inflight_target_id,
        true,
      };
    }
    if (*readback_id == committed_expected_id) {
      return {
        fallback_commit_action_e::retry_committed,
        committed_expected_id,
        true,
      };
    }
    return {
      fallback_commit_action_e::release_external,
      {},
      false,
    };
  }

  std::optional<std::string> superseded_assignment_repair_target(
    const std::string &superseded_side_effect_target_id,
    const std::string &live_id,
    const std::string &active_epoch_owned_target_id
  ) {
    if (superseded_side_effect_target_id.empty() ||
        live_id != superseded_side_effect_target_id ||
        active_epoch_owned_target_id.empty() ||
        active_epoch_owned_target_id == live_id) {
      return std::nullopt;
    }
    return active_epoch_owned_target_id;
  }

  superseded_write_observation_transition_t observe_superseded_write(
    const std::vector<std::string> &stale_target_ids,
    const std::vector<std::string> &tolerated_live_ids,
    const std::string &live_id,
    const std::string &active_epoch_owned_target_id,
    bool active_epoch_entitled,
    bool cancelled
  ) {
    if (cancelled ||
        !active_epoch_entitled ||
        active_epoch_owned_target_id.empty()) {
      return {superseded_write_observation_action_e::cancel, {}};
    }
    if (live_id.empty() || live_id == active_epoch_owned_target_id) {
      return {superseded_write_observation_action_e::keep_observing, {}};
    }
    if (std::find(stale_target_ids.begin(), stale_target_ids.end(), live_id) !=
        stale_target_ids.end()) {
      return {
        superseded_write_observation_action_e::repair_active,
        active_epoch_owned_target_id,
      };
    }
    if (std::find(tolerated_live_ids.begin(), tolerated_live_ids.end(), live_id) !=
        tolerated_live_ids.end()) {
      return {superseded_write_observation_action_e::keep_observing, {}};
    }
    return {
      superseded_write_observation_action_e::adopt_external,
      live_id,
    };
  }

  bool policy_write_requires_settling(
    bool set_succeeded,
    bool assignment_still_current
  ) {
    return !set_succeeded || !assignment_still_current;
  }

  bool activation_policy_write_requires_settling(
    bool set_succeeded,
    bool assignment_still_current,
    bool exact_target_readback_confirmed
  ) {
    return !set_succeeded ||
           !assignment_still_current ||
           !exact_target_readback_confirmed;
  }

  bool policy_write_receipt_is_pending(
    std::uint64_t receipt_assignment_epoch,
    std::uint64_t active_assignment_epoch,
    std::uint64_t receipt_role_revision,
    std::uint64_t active_role_revision,
    bool repair_hazard,
    const std::string &issuer_active_id,
    const std::string &active_owned_id
  ) {
    return !repair_hazard &&
           receipt_assignment_epoch == active_assignment_epoch &&
           receipt_role_revision == active_role_revision &&
           !issuer_active_id.empty() &&
           issuer_active_id == active_owned_id;
  }

  bool policy_observer_owner_changed(
    const policy_observer_owner_key_t &observed,
    const policy_observer_owner_key_t &current
  ) {
    return observed.generation != current.generation ||
           observed.assignment_epoch != current.assignment_epoch ||
           observed.role_revision != current.role_revision ||
           observed.desired_id != current.desired_id;
  }

  causal_repair_transition_t plan_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick,
    bool stale_target_is_live,
    unsigned max_attempts
  ) {
    if (now_tick >= state.deadline_tick || state.attempts >= max_attempts) {
      state.outstanding = false;
      return {causal_repair_action_e::retire, state};
    }
    if (!stale_target_is_live || state.outstanding ||
        now_tick < state.next_attempt_tick) {
      return {causal_repair_action_e::wait, state};
    }
    state.outstanding = true;
    ++state.attempts;
    return {causal_repair_action_e::issue, state};
  }

  causal_repair_chain_selection_t select_causal_repair_chain(
    const std::vector<causal_repair_chain_candidate_t> &chains,
    const std::string &stale_target_id,
    std::uint64_t now_tick,
    unsigned max_attempts
  ) {
    const auto matches_target = [&](const auto &candidate) {
      return candidate.stale_target_id == stale_target_id;
    };
    std::optional<causal_repair_chain_selection_t> retired_outstanding;
    for (auto candidate = chains.rbegin(); candidate != chains.rend(); ++candidate) {
      if (matches_target(*candidate) && candidate->state.outstanding) {
        causal_repair_chain_selection_t selection {
          candidate->causal_receipt_id,
          plan_causal_repair(
            candidate->state,
            now_tick,
            true,
            max_attempts
          ),
        };
        if (selection.transition.action != causal_repair_action_e::retire) {
          return selection;
        }
        if (!retired_outstanding) {
          retired_outstanding = selection;
        }
      }
    }

    std::optional<causal_repair_chain_selection_t> waiting;
    std::optional<causal_repair_chain_selection_t> retired;
    for (auto candidate = chains.rbegin(); candidate != chains.rend(); ++candidate) {
      if (!matches_target(*candidate)) {
        continue;
      }
      causal_repair_chain_selection_t selection {
        candidate->causal_receipt_id,
        plan_causal_repair(
          candidate->state,
          now_tick,
          true,
          max_attempts
        ),
      };
      if (selection.transition.action == causal_repair_action_e::issue) {
        return selection;
      }
      if (selection.transition.action == causal_repair_action_e::wait && !waiting) {
        waiting = selection;
      }
      if (selection.transition.action == causal_repair_action_e::retire && !retired) {
        retired = selection;
      }
    }
    if (waiting) {
      return *waiting;
    }
    if (retired) {
      return *retired;
    }
    if (retired_outstanding) {
      return *retired_outstanding;
    }
    return {
      std::nullopt,
      {causal_repair_action_e::wait, {}},
    };
  }

  causal_repair_state_t complete_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick,
    bool set_succeeded,
    bool exact_target_readback,
    std::uint64_t retry_backoff_ticks
  ) {
    if (set_succeeded && !exact_target_readback) {
      return state;
    }
    state.outstanding = false;
    state.next_attempt_tick = now_tick + retry_backoff_ticks;
    return state;
  }

  causal_repair_state_t retract_unissued_causal_repair(
    causal_repair_state_t state,
    std::uint64_t now_tick
  ) {
    state.outstanding = false;
    if (state.attempts != 0) {
      --state.attempts;
    }
    state.next_attempt_tick = now_tick;
    return state;
  }

  std::uint64_t causal_repair_deadline_after_generation(
    std::uint64_t original_deadline_tick,
    std::uint64_t proposed_deadline_tick,
    bool repair_generation
  ) {
    return repair_generation ? original_deadline_tick : proposed_deadline_tick;
  }

  policy_receipt_capacity_decision_t decide_policy_receipt_capacity(
    std::size_t receipt_count,
    std::size_t receipt_limit,
    const std::vector<bool> &settled_receipts
  ) {
    if (receipt_count < receipt_limit) {
      return {policy_receipt_capacity_action_e::append, std::nullopt};
    }
    const auto settled = std::find(
      settled_receipts.begin(),
      settled_receipts.end(),
      true
    );
    if (settled != settled_receipts.end()) {
      return {
        policy_receipt_capacity_action_e::reclaim_settled,
        static_cast<std::size_t>(std::distance(settled_receipts.begin(), settled)),
      };
    }
    return {policy_receipt_capacity_action_e::reject_unresolved, std::nullopt};
  }

  policy_write_execution_action_e classify_policy_write_execution(
    bool may_have_executed,
    bool precondition_proved_no_write,
    bool pre_read_proved_no_write
  ) {
    return may_have_executed && !precondition_proved_no_write &&
             !pre_read_proved_no_write ?
      policy_write_execution_action_e::retain_hazard :
      policy_write_execution_action_e::erase_unissued;
  }

  bool authenticated_policy_pre_read_proves_no_write(
    const policy_pre_read_proof_t &proof
  ) {
    return proof.process_reaped && proof.execution_completed &&
           proof.pre_read_stage && proof.set_was_not_issued &&
           proof.read_failed && proof.readback_empty;
  }

  superseded_write_observer_progress_t advance_superseded_write_observer(
    bool generation_changed,
    bool deadline_expired,
    bool read_succeeded,
    bool observation_state_unchanged,
    bool kept_observing,
    int stable_observations,
    int required_stable_observations
  ) {
    if (generation_changed) {
      return {superseded_write_observer_progress_action_e::handoff_generation, 0};
    }
    if (deadline_expired) {
      return {superseded_write_observer_progress_action_e::retire_generation, 0};
    }
    if (!read_succeeded || !observation_state_unchanged || !kept_observing) {
      return {superseded_write_observer_progress_action_e::continue_observing, 0};
    }

    const auto next_stable_observations = stable_observations + 1;
    if (required_stable_observations <= 0 ||
        next_stable_observations >= required_stable_observations) {
      return {
        superseded_write_observer_progress_action_e::retire_generation,
        next_stable_observations,
      };
    }
    return {
      superseded_write_observer_progress_action_e::continue_observing,
      next_stable_observations,
    };
  }

  superseded_write_ledger_phase_t advance_superseded_write_ledger_after_external_adoption(
    const std::vector<std::string> &stale_target_ids,
    const std::string &previous_active_target_id,
    const std::string &external_id
  ) {
    superseded_write_ledger_phase_t next_phase {stale_target_ids, {}};
    if (!previous_active_target_id.empty() &&
        std::find(
          next_phase.stale_target_ids.begin(),
          next_phase.stale_target_ids.end(),
          previous_active_target_id
        ) == next_phase.stale_target_ids.end()) {
      next_phase.stale_target_ids.push_back(previous_active_target_id);
    }
    if (!external_id.empty()) {
      next_phase.tolerated_live_ids.push_back(external_id);
    }
    return next_phase;
  }

  pending_role_ownership_action_e classify_pending_role_ownership(
    const render_endpoint_catalog_t &catalog,
    const std::string &expected_current_id,
    const std::optional<std::string> &live_id
  ) {
    const auto observation = classify_default_endpoint_observation(
      expected_current_id,
      live_id
    );
    if (!catalog.complete ||
        observation == default_endpoint_observation_e::unavailable) {
      return pending_role_ownership_action_e::poll_catalog;
    }
    const auto &observed_id = *live_id;
    const bool expected_is_steam =
      !expected_current_id.empty() &&
      std::find(
        catalog.steam_endpoint_ids.begin(),
        catalog.steam_endpoint_ids.end(),
        expected_current_id
      ) != catalog.steam_endpoint_ids.end();
    const bool observed_is_steam =
      std::find(
        catalog.steam_endpoint_ids.begin(),
        catalog.steam_endpoint_ids.end(),
        observed_id
      ) != catalog.steam_endpoint_ids.end();
    if (observed_is_steam &&
        (expected_current_id.empty() ||
         (expected_is_steam &&
          observation == default_endpoint_observation_e::matches_expected))) {
      return pending_role_ownership_action_e::confirm_steam_owned;
    }
    return pending_role_ownership_action_e::release_external;
  }

  default_endpoint_observation_e classify_default_endpoint_observation(
    const std::string &expected_current_id,
    const std::optional<std::string> &live_id
  ) {
    if (!live_id || live_id->empty()) {
      return default_endpoint_observation_e::unavailable;
    }
    return *live_id == expected_current_id ?
      default_endpoint_observation_e::matches_expected :
      default_endpoint_observation_e::different_nonempty;
  }

  restore_external_observation_action_e classify_restore_external_observation(
    const std::string &live_id,
    const std::vector<std::string> &pending_target_ids,
    const std::vector<std::string> &stale_target_ids
  ) {
    if (std::find(
          pending_target_ids.begin(),
          pending_target_ids.end(),
          live_id
        ) != pending_target_ids.end()) {
      return restore_external_observation_action_e::commit_pending_receipt;
    }
    if (std::find(
          stale_target_ids.begin(),
          stale_target_ids.end(),
          live_id
        ) != stale_target_ids.end()) {
      return restore_external_observation_action_e::retry_stale_receipt;
    }
    return restore_external_observation_action_e::adopt_external;
  }

  bool should_attempt_role_fallback(
    bool fallback_requested,
    bool has_ambiguous_inflight_write
  ) {
    return fallback_requested && !has_ambiguous_inflight_write;
  }

  worker_bootstrap_action_e worker_bootstrap_action(
    bool assignment_active,
    bool initialization_succeeded
  ) {
    if (!assignment_active) {
      return worker_bootstrap_action_e::stop;
    }
    return initialization_succeeded ?
      worker_bootstrap_action_e::run :
      worker_bootstrap_action_e::retry_after_backoff;
  }

  worker_role_write_action_e worker_role_write_action(
    bool worker_active,
    bool role_assignment_still_current
  ) {
    if (!worker_active) {
      return worker_role_write_action_e::stop_worker;
    }
    return role_assignment_still_current ?
      worker_role_write_action_e::use_policy_status :
      worker_role_write_action_e::release_external;
  }

  fallback_catalog_action_e fallback_catalog_action(const render_endpoint_catalog_t &catalog) {
    if (!catalog.complete) {
      return fallback_catalog_action_e::poll_after_backoff;
    }
    if (catalog.steam_endpoint_ids.empty() ||
        catalog.eligible_non_steam_endpoint_ids.empty()) {
      return fallback_catalog_action_e::wait_for_arrival;
    }
    return fallback_catalog_action_e::attempt;
  }

  bool should_queue_post_install_unknown_role(
    bool current_read_known,
    const std::string &
  ) {
    return !current_read_known;
  }

  sample_action_e sample_action(sample_status_e status) {
    switch (status) {
      case sample_status_e::ok:
        return sample_action_e::emit;
      case sample_status_e::timeout:
        return sample_action_e::retry;
      case sample_status_e::reinitialize:
        return sample_action_e::reacquire;
      case sample_status_e::interrupted:
      case sample_status_e::error:
        return sample_action_e::stop;
    }
    return sample_action_e::stop;
  }

  capture_summary_t drive_capture(sample_source_t &source, std::size_t event_limit) {
    capture_summary_t summary;
    for (std::size_t event = 0; event < event_limit; ++event) {
      switch (sample_action(source.sample())) {
        case sample_action_e::emit:
          ++summary.emitted;
          break;
        case sample_action_e::retry:
          ++summary.timeouts;
          break;
        case sample_action_e::reacquire:
          ++summary.reacquisitions;
          if (!source.reacquire()) {
            summary.stopped = true;
            return summary;
          }
          break;
        case sample_action_e::stop:
          summary.stopped = true;
          return summary;
      }
    }
    return summary;
  }
}  // namespace audio::policy
