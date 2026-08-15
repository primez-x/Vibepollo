#pragma once

#include "tools/atmos_capability_probe_policy.h"

#include <windows.h>
#include <ksmedia.h>
#include <mmreg.h>
#include <propidl.h>
#include <optional>
#include <string>
#include <string_view>

namespace atmos_probe {
  inline constexpr GUID k_hdmi_interface {
    0xd1b9cc2a, 0xf519, 0x417f,
    {0x91, 0xc9, 0x55, 0xfa, 0x65, 0x48, 0x10, 0x01}
  };
  inline constexpr GUID k_displayport_interface {
    0xe47e4031, 0x3ea6, 0x418d,
    {0x8f, 0x9b, 0xb7, 0x38, 0x43, 0xcc, 0xba, 0x97}
  };
  inline constexpr GUID k_iec61937_dolby_mat20 {
    0x0000010c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };
  inline constexpr GUID k_iec61937_dolby_mlp {
    0x0000000c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };
  inline constexpr GUID k_iec61937_dolby_mat21 {
    0x0000030c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };

  WAVEFORMATEXTENSIBLE_IEC61937 make_mat_format(mat_profile profile);
  std::optional<std::string> canonical_guid(std::wstring_view raw);
  form_factor_observation decode_form_factor(const PROPVARIANT &value);
  jack_subtype_observation decode_jack_subtype(const PROPVARIANT &value);
  probe_observation collect_windows_observation(const probe_options &options);
}  // namespace atmos_probe
