#ifdef _WIN32
#define INITGUID
#include "tools/atmos_mat_client_writer.h"

#include "tools/atmos_capability_probe_policy.h"
#include "tools/atmos_capability_probe_windows.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstring>
#include <memory>
#include <string>

namespace atmos_mat_client {
  namespace {
    template<class T> struct com_release { void operator()(T *value) const noexcept { if (value) value->Release(); } };
    template<class T> using com_ptr = std::unique_ptr<T, com_release<T>>;
    struct close_handle { void operator()(HANDLE value) const noexcept { if (value) CloseHandle(value); } };
    using event_handle = std::unique_ptr<void, close_handle>;
    class com_apartment {
    public:
      com_apartment() noexcept: result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
      ~com_apartment() noexcept { if (result_ == S_OK || result_ == S_FALSE) CoUninitialize(); }
      bool usable() const noexcept { return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE; }
      HRESULT result() const noexcept { return result_; }
    private:
      HRESULT result_ {};
    };

    bool canonical_mat10(const descriptor_identity &identity) noexcept {
      static_assert(sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == descriptor_bytes);
      WAVEFORMATEXTENSIBLE_IEC61937 format {};
      std::memcpy(&format, identity.bytes.data(), descriptor_bytes);
      return format.FormatExt.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.FormatExt.Format.nChannels == 8 && format.FormatExt.Format.nSamplesPerSec == carrier_rate &&
        format.FormatExt.Format.nAvgBytesPerSec == carrier_rate * carrier_frame_bytes &&
        format.FormatExt.Format.nBlockAlign == carrier_frame_bytes && format.FormatExt.Format.wBitsPerSample == 16 &&
        format.FormatExt.Format.cbSize == 34 && format.FormatExt.Samples.wValidBitsPerSample == 16 &&
        format.FormatExt.dwChannelMask == KSAUDIO_SPEAKER_7POINT1 &&
        IsEqualGUID(format.FormatExt.SubFormat, atmos_probe::k_iec61937_dolby_mlp) &&
        format.dwEncodedSamplesPerSec == 96000 && format.dwEncodedChannelCount == 8 &&
        format.dwAverageBytesPerSec == 0;
    }

    std::string utf8(const std::wstring_view input) {
      if (input.empty()) return {};
      const auto size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
      std::string output(static_cast<std::size_t>(size), '\0');
      WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
      return output;
    }

    windows_render_result fail(writer &queue, const poison_reason reason, const HRESULT value) noexcept {
      if (reason == poison_reason::format_failure) queue.poison_format_failure();
      else if (reason == poison_reason::route_failure) queue.poison_route_failure();
      else queue.poison_sink_underrun();
      return {value, queue.poison(), queue.submitted_frames(), queue.played_frames()};
    }
  }  // namespace

  windows_render_result render_windows_mat10_exclusive(
      writer &queue, const std::wstring_view endpoint_id, const descriptor_identity &descriptor,
      bool (*keep_running)(void *), void *keep_running_context,
      const device_clock_feedback_callback feedback_callback, void *feedback_context) noexcept {
    if (!canonical_mat10(descriptor) || queue.poison() != poison_reason::none) {
      return fail(queue, poison_reason::format_failure, E_INVALIDARG);
    }
    try {
      const com_apartment apartment;
      if (!apartment.usable()) return fail(queue, poison_reason::route_failure, apartment.result());
      const atmos_probe::probe_options options {.endpoint_id = utf8(endpoint_id)};
      const auto gate = atmos_probe::evaluate(atmos_probe::collect_windows_observation(options));
      if (!gate.ready || gate.selected_profile != atmos_probe::mat_profile::mat10) {
        return fail(queue, poison_reason::route_failure, AUDCLNT_E_UNSUPPORTED_FORMAT);
      }
      com_ptr<IMMDeviceEnumerator> enumerator;
      // CoCreateInstance writes into a raw temporary; ownership is immediately
      // transferred to the local COM guard.
      IMMDeviceEnumerator *raw_enumerator {};
      HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&raw_enumerator));
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      enumerator.reset(raw_enumerator);
      IMMDevice *raw_device {};
      hr = enumerator->GetDevice(std::wstring(endpoint_id).c_str(), &raw_device);
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      com_ptr<IMMDevice> device(raw_device);
      IAudioClient *raw_client {};
      hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&raw_client));
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      com_ptr<IAudioClient> client(raw_client);
      WAVEFORMATEXTENSIBLE_IEC61937 format {};
      std::memcpy(&format, descriptor.bytes.data(), descriptor_bytes);
      hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.FormatExt.Format, nullptr);
      if (FAILED(hr)) return fail(queue, poison_reason::format_failure, hr);
      const REFERENCE_TIME period = 1'000'000;  // 100 ms, exclusive event period.
      hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        period, period, &format.FormatExt.Format, nullptr);
      if (FAILED(hr)) return fail(queue, poison_reason::format_failure, hr);
      event_handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
      if (!event) return fail(queue, poison_reason::route_failure, HRESULT_FROM_WIN32(GetLastError()));
      hr = client->SetEventHandle(event.get());
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      UINT32 buffer_frames {};
      hr = client->GetBufferSize(&buffer_frames);
      if (FAILED(hr) || buffer_frames == 0) return fail(queue, poison_reason::format_failure, FAILED(hr) ? hr : E_FAIL);
      IAudioRenderClient *raw_render {};
      hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&raw_render));
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      com_ptr<IAudioRenderClient> render(raw_render);
      IAudioClock *raw_clock {};
      hr = client->GetService(__uuidof(IAudioClock), reinterpret_cast<void **>(&raw_clock));
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      com_ptr<IAudioClock> clock(raw_clock);
      UINT64 clock_frequency {};
      hr = clock->GetFrequency(&clock_frequency);
      if (FAILED(hr) || clock_frequency != carrier_rate) return fail(queue, poison_reason::format_failure, FAILED(hr) ? hr : E_FAIL);
      // Prime the entire exclusive buffer before Start.  Starting an empty MAT
      // stream would ask the endpoint to render invented silence before its
      // first event, which is prohibited for opaque carrier transport.
      if (queue.queued_frames() < buffer_frames) {
        return fail(queue, poison_reason::sink_underrun, AUDCLNT_E_BUFFER_ERROR);
      }
      BYTE *initial_buffer {};
      hr = render->GetBuffer(buffer_frames, &initial_buffer);
      if (FAILED(hr) || !initial_buffer) return fail(queue, poison_reason::sink_underrun, FAILED(hr) ? hr : E_FAIL);
      const auto initial_copy = queue.copy_next(std::span<std::uint8_t>(
        initial_buffer, static_cast<std::size_t>(buffer_frames) * carrier_frame_bytes));
      if (initial_copy.status != copy_status::copied || initial_copy.frames != buffer_frames) {
        static_cast<void>(render->ReleaseBuffer(0, 0));
        return fail(queue, poison_reason::sink_underrun, E_FAIL);
      }
      hr = render->ReleaseBuffer(buffer_frames, 0);
      if (FAILED(hr)) return fail(queue, poison_reason::sink_underrun, hr);
      hr = client->Start();
      if (FAILED(hr)) return fail(queue, poison_reason::route_failure, hr);
      const auto cleanup = [&]() noexcept { static_cast<void>(client->Stop()); static_cast<void>(client->Reset()); };
      while (keep_running == nullptr || keep_running(keep_running_context)) {
        const DWORD wait = WaitForSingleObject(event.get(), 250);
        if (wait != WAIT_OBJECT_0) { cleanup(); return fail(queue, poison_reason::sink_underrun, wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError())); }
        UINT32 padding {};
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr) || padding > buffer_frames) { cleanup(); return fail(queue, poison_reason::sink_underrun, FAILED(hr) ? hr : E_FAIL); }
        const auto writable = buffer_frames - padding;
        if (writable != 0) {
          if (queue.queued_frames() == 0) { cleanup(); return fail(queue, poison_reason::sink_underrun, AUDCLNT_E_BUFFER_ERROR); }
          const auto frames = static_cast<UINT32>(std::min<std::uint64_t>(writable, queue.queued_frames()));
          BYTE *buffer {};
          hr = render->GetBuffer(frames, &buffer);
          if (FAILED(hr) || !buffer) { cleanup(); return fail(queue, poison_reason::sink_underrun, FAILED(hr) ? hr : E_FAIL); }
          const auto copied = queue.copy_next(std::span<std::uint8_t>(buffer, static_cast<std::size_t>(frames) * carrier_frame_bytes));
          if (copied.status != copy_status::copied || copied.frames != frames) { static_cast<void>(render->ReleaseBuffer(0, 0)); cleanup(); return fail(queue, poison_reason::sink_underrun, E_FAIL); }
          hr = render->ReleaseBuffer(frames, 0);
          if (FAILED(hr)) { cleanup(); return fail(queue, poison_reason::sink_underrun, hr); }
        }
        UINT64 position {}, qpc {};
        hr = clock->GetPosition(&position, &qpc);
        if (FAILED(hr)) { cleanup(); return fail(queue, poison_reason::sink_underrun, hr); }
        queue.acknowledge_played(position);
        if (queue.poison() != poison_reason::none) { cleanup(); return {E_FAIL, queue.poison(), queue.submitted_frames(), queue.played_frames()}; }
        if (feedback_callback) if (const auto feedback = queue.feedback(qpc, clock_frequency)) feedback_callback(*feedback, feedback_context);
      }
      cleanup();
      return {S_OK, queue.poison(), queue.submitted_frames(), queue.played_frames()};
    } catch (...) {
      return fail(queue, poison_reason::route_failure, E_FAIL);
    }
  }
}  // namespace atmos_mat_client
#endif
