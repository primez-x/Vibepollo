#include "tools/atmos_mat_direct_protocol.h"

#include <algorithm>
#include <limits>

namespace atmos_mat_direct {
  namespace {
    constexpr std::uint32_t magic = 0x3154414dU;
    void put16(std::vector<std::uint8_t> &out, const std::size_t at, const std::uint16_t value) { out[at] = value; out[at + 1] = value >> 8; }
    void put32(std::vector<std::uint8_t> &out, const std::size_t at, const std::uint32_t value) { for (int i = 0; i != 4; ++i) out[at + i] = value >> (i * 8); }
    void put64(std::vector<std::uint8_t> &out, const std::size_t at, const std::uint64_t value) { for (int i = 0; i != 8; ++i) out[at + i] = value >> (i * 8); }
    std::uint16_t get16(std::span<const std::uint8_t> in, std::size_t at) { return in[at] | (std::uint16_t(in[at + 1]) << 8); }
    std::uint32_t get32(std::span<const std::uint8_t> in, std::size_t at) { std::uint32_t r {}; for (int i = 3; i >= 0; --i) r = (r << 8) | in[at + i]; return r; }
    std::uint64_t get64(std::span<const std::uint8_t> in, std::size_t at) { std::uint64_t r {}; for (int i = 7; i >= 0; --i) r = (r << 8) | in[at + i]; return r; }
  }
  std::optional<std::vector<std::uint8_t>> encode(const record &r) {
    if (!r.generation || !r.stream_id || r.payload.empty() || r.payload.size() > max_payload_bytes || r.payload.size() % carrier_frame_bytes || r.host_qpc_frequency == 0 || (r.flags & ~0x0fU)) return std::nullopt;
    if (r.payload.size() > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    std::vector<std::uint8_t> out(header_bytes + r.payload.size());
    put32(out, 0, magic); put16(out, 4, protocol_major); put16(out, 6, 0); put16(out, 8, header_bytes); put16(out, 10, 0); put32(out, 12, static_cast<std::uint32_t>(out.size())); put32(out, 16,r.flags);
    put64(out, 20, r.generation); put64(out, 28, r.stream_id); std::copy_n(r.descriptor_value.bytes.begin(), 16, out.begin() + 36); put64(out,52,r.first_carrier_frame); put64(out,60,r.host_qpc); put64(out,68,r.host_qpc_frequency); put32(out,76,static_cast<std::uint32_t>(r.payload.size())); std::copy_n(r.descriptor_value.bytes.begin()+16, 36, out.begin()+80);
    std::copy(r.payload.begin(), r.payload.end(), out.begin() + header_bytes); return out;
  }
  std::optional<record> decode(const std::span<const std::uint8_t> in) {
    if (in.size() < header_bytes || get32(in,0)!=magic || get16(in,4)!=protocol_major || get16(in,6)>0 || get16(in,8)!=header_bytes || get16(in,10)!=0 || get32(in,12)!=in.size()) return std::nullopt;
    const auto bytes=get32(in,76); const auto flags=get32(in,16); if(!bytes || bytes>max_payload_bytes || bytes%carrier_frame_bytes || header_bytes+bytes!=in.size() || flags&~0x0fU || !get64(in,20) || !get64(in,28) || !get64(in,68)) return std::nullopt;
    record r; r.generation=get64(in,20); r.stream_id=get64(in,28); std::copy_n(in.begin()+36, 16, r.descriptor_value.bytes.begin()); std::copy_n(in.begin()+80, 36, r.descriptor_value.bytes.begin()+16); r.first_carrier_frame=get64(in,52); r.host_qpc=get64(in,60); r.host_qpc_frequency=get64(in,68); r.flags=flags; r.payload.assign(in.begin()+header_bytes,in.end()); return r;
  }
}
