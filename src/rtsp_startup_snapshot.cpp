/**
 * @file src/rtsp_startup_snapshot.cpp
 * @brief Isolated launch-session snapshot construction for RTSP startup.
 */
#include "rtsp.h"

namespace rtsp_stream {

  std::shared_ptr<launch_session_t> launch_session_t::clone_for_startup() const {
    auto snapshot = std::make_shared<launch_session_t>();

    snapshot->id = id;
    snapshot->gcm_key = gcm_key;
    snapshot->iv = iv;
    snapshot->av_ping_payload = av_ping_payload;
    snapshot->control_connect_data = control_connect_data;
    snapshot->unique_id = unique_id;
    snapshot->client_uuid = client_uuid;
    snapshot->device_name = device_name;
    snapshot->hdr_profile = hdr_profile;
    snapshot->client_hdr_capabilities = client_hdr_capabilities;
    snapshot->hdr_peak_resolution = hdr_peak_resolution;
    snapshot->hdr_runtime_generation = hdr_runtime_generation;
    snapshot->hdr_runtime_owner_token = hdr_runtime_owner_token;
    snapshot->hdr_runtime_cohort_participant = hdr_runtime_cohort_participant;
    snapshot->client_display_mode_override = client_display_mode_override;
    snapshot->client_display_refresh_millihz = client_display_refresh_millihz;
    snapshot->enable_hdr = enable_hdr;
    snapshot->prefer_sdr_10bit = prefer_sdr_10bit;
    snapshot->force_sdr = force_sdr;
    snapshot->client_vrr_requested = client_vrr_requested;
    snapshot->perm = perm;
    snapshot->fps = fps;
    // Copied, not moved: the io_context thread still owns the original session.
    // stream::session::alloc() moves these out of the clone on the startup worker.
    snapshot->client_do_cmds = client_do_cmds;
    snapshot->client_undo_cmds = client_undo_cmds;
    snapshot->virtual_display = virtual_display;
    snapshot->virtual_display_guid_bytes = virtual_display_guid_bytes;
    snapshot->gen1_framegen_fix = gen1_framegen_fix;
    snapshot->gen2_framegen_fix = gen2_framegen_fix;
    snapshot->frame_generation_enabled = frame_generation_enabled;
    snapshot->lossless_scaling_framegen = lossless_scaling_framegen;
    snapshot->framegen_refresh_rate = framegen_refresh_rate;
    snapshot->framegen_refresh_millihz = framegen_refresh_millihz;
    snapshot->framegen_refresh_multiplier = framegen_refresh_multiplier;
    snapshot->frame_generation_provider = frame_generation_provider;
    snapshot->lossless_scaling_target_fps = lossless_scaling_target_fps;
    snapshot->lossless_scaling_rtss_limit = lossless_scaling_rtss_limit;
#ifdef _WIN32
    snapshot->display_helper_gate = display_helper_gate;
#endif

    return snapshot;
  }

}  // namespace rtsp_stream
