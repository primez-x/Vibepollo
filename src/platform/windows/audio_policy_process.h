/**
 * @file src/platform/windows/audio_policy_process.h
 * @brief Synchronous bounded supervisor for one-shot Windows audio policy work.
 */
#pragma once

#include "audio_policy_helper_protocol.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace platf::audio_policy {

  using operation_e = protocol::operation_e;
  using render_role_e = protocol::render_role_e;
  using execution_disposition_e = protocol::execution_disposition_e;
  using wave_format_extensible_t = protocol::wave_format_extensible_t;
  inline constexpr HRESULT kPreconditionMismatchHresult =
    protocol::kPreconditionMismatchHresult;

  enum class failure_stage_e : std::uint32_t {
    success = 0,
    launch = 1,
    protocol = 2,
    com = 3,
    set = 4,
    read = 5,
    timeout = 6,
    kill = 7,
    reap = 8,
    cancelled = 9,
    format = 10,
    precondition = 11,
    pre_read = 12,
    post_set_readback = 13,
  };

  inline constexpr std::chrono::milliseconds kDefaultTimeout {1000};
  inline constexpr std::chrono::milliseconds kMaximumTimeout {30'000};

  struct request_t {
    operation_e operation;
    render_role_e role;
    std::wstring endpoint_id;
    std::chrono::milliseconds timeout {kDefaultTimeout};
    std::stop_token stop_token {};
    std::optional<wave_format_extensible_t> device_format {};
    std::wstring expected_current_id {};
  };

  struct result_t {
    failure_stage_e stage {failure_stage_e::launch};
    HRESULT com_hresult {E_UNEXPECTED};
    HRESULT set_hresult {E_UNEXPECTED};
    HRESULT format_hresult {E_UNEXPECTED};
    HRESULT read_hresult {E_UNEXPECTED};
    std::wstring readback_id;
    DWORD process_exit_code {STILL_ACTIVE};
    DWORD win32_error {ERROR_SUCCESS};
    bool kill_attempted {false};
    bool process_reaped {false};
    execution_disposition_e execution_disposition {
      execution_disposition_e::not_started};
  };

  /**
   * @brief Run exactly one READ, SET_AND_READBACK, or SET_DEVICE_FORMAT operation synchronously.
   *
   * The helper is resolved to the protected absolute path
   * `<Sunshine directory>\\tools\\sunshine_audio_policy_helper.exe`. The
   * supervisor waits for the child to become signaled before returning. For a
   * launched child, process_reaped is true unless an exceptional bounded reap
   * fails; pre-launch failures leave it false.
   */
  result_t invoke(const request_t &request);

#if defined(SUNSHINE_AUDIO_POLICY_PROCESS_TESTING)
  /**
   * @brief Test-only equivalent of invoke with an explicitly supplied helper.
   *
   * The same absolute-path, fixed-basename, and reparse-point checks apply.
   */
  result_t invoke_with_helper_path(
    const request_t &request,
    const std::filesystem::path &helper_path);

  using process_test_before_resume_hook_t = std::function<void()>;

  result_t invoke_with_helper_path(
    const request_t &request,
    const std::filesystem::path &helper_path,
    const process_test_before_resume_hook_t &before_resume);
#endif

}  // namespace platf::audio_policy
