#include "tools/atmos_mat_direct_relay_cli.h"

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace atmos_mat_relay_cli {
  namespace {
    constexpr std::size_t max_frame = atmos_mat_direct::header_bytes + atmos_mat_direct::max_payload_bytes;
    bool decode_hex(const std::string_view text, std::span<std::uint8_t> out) {
      if (text.size() != out.size() * 2) return false;
      for (std::size_t i = 0; i != out.size(); ++i) {
        unsigned value {};
        const auto [at, error] = std::from_chars(text.data() + i * 2, text.data() + i * 2 + 2, value, 16);
        if (error != std::errc {} || at != text.data() + i * 2 + 2 || value > 0xff) return false;
        out[i] = static_cast<std::uint8_t>(value);
      }
      return true;
    }
    bool parse_u32(const std::string_view text, std::uint32_t &out) {
      const auto [at, error] = std::from_chars(text.data(), text.data() + text.size(), out);
      return error == std::errc {} && at == text.data() + text.size() && out != 0;
    }
    atmos_mat_client::descriptor_identity client_descriptor(const atmos_mat_direct::descriptor &source) {
      atmos_mat_client::descriptor_identity result {};
      std::copy(source.bytes.begin(), source.bytes.end(), result.bytes.begin());
      return result;
    }
    void hash_append(std::array<std::uint8_t, 32> &out, const std::span<const std::uint8_t> bytes) {
      EVP_MD_CTX *ctx = EVP_MD_CTX_new();
      if (!ctx) { out.fill(0); return; }
      unsigned int size {};
      const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(ctx, out.data(), out.size()) == 1 && EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1 &&
        EVP_DigestFinal_ex(ctx, out.data(), &size) == 1 && size == out.size();
      EVP_MD_CTX_free(ctx);
      if (!ok) out.fill(0);
    }
    bool write_client(atmos_mat_client::writer &client, const atmos_mat_direct::record &record,
      const atmos_mat_client::descriptor_identity &descriptor) {
      atmos_mat_client::carrier_block block {record.generation, record.first_carrier_frame, record.host_qpc,
        record.host_qpc_frequency, descriptor, record.payload};
      return client.enqueue(std::move(block)) == atmos_mat_client::enqueue_result::accepted;
    }
#ifdef _WIN32
    struct winsock_runtime { winsock_runtime() { WSADATA data {}; ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; } ~winsock_runtime() { if (ok) WSACleanup(); } bool ok {}; };
    struct socket_handle { SOCKET value {INVALID_SOCKET}; ~socket_handle() { if (value != INVALID_SOCKET) closesocket(value); } socket_handle() = default; socket_handle(const socket_handle &) = delete; socket_handle &operator=(const socket_handle &) = delete; socket_handle(socket_handle &&other) noexcept : value(other.value) { other.value=INVALID_SOCKET; } socket_handle &operator=(socket_handle &&other) noexcept { if(this!=&other){if(value!=INVALID_SOCKET)closesocket(value);value=other.value;other.value=INVALID_SOCKET;}return *this;} };
    std::optional<std::string> read_file(const std::string &path) { std::ifstream in(path, std::ios::binary); if (!in) return std::nullopt; in.seekg(0, std::ios::end); const auto length=in.tellg(); if (length <= 0 || length > 1024 * 1024) return std::nullopt; std::string data(static_cast<std::size_t>(length), '\0'); in.seekg(0); return in.read(data.data(), length) ? std::optional<std::string>(std::move(data)) : std::nullopt; }
    bool split_address(const std::string &input, std::string &host, std::string &port) { const auto p=input.rfind(':'); if(p==std::string::npos || p==0 || p+1==input.size()) return false; host=input.substr(0,p); port=input.substr(p+1); return true; }
    socket_handle tcp_open(const std::string &address, const bool listener) { std::string host, port; socket_handle result; if(!split_address(address,host,port)) return result; addrinfo hints {}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_STREAM; hints.ai_flags=listener ? AI_PASSIVE : 0; addrinfo *answers {}; if(getaddrinfo(host.c_str(),port.c_str(),&hints,&answers)!=0) return result; for(auto *a=answers;a;a=a->ai_next) { SOCKET s=socket(a->ai_family,a->ai_socktype,a->ai_protocol); if(s==INVALID_SOCKET) continue; if(listener) { BOOL one=TRUE; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&one),sizeof(one)); if(bind(s,a->ai_addr,static_cast<int>(a->ai_addrlen))==0 && listen(s,1)==0) { result.value=s; break; } } else if(connect(s,a->ai_addr,static_cast<int>(a->ai_addrlen))==0) { result.value=s; break; } closesocket(s); } freeaddrinfo(answers); return result; }
    bool ssl_write_all(SSL *ssl, const std::span<const std::uint8_t> data) { std::size_t at {}; while(at<data.size()) { const auto part=static_cast<int>(std::min<std::size_t>(data.size()-at, std::numeric_limits<int>::max())); const auto n=SSL_write(ssl,data.data()+at,part); if(n<=0) return false; at+=static_cast<std::size_t>(n); } return true; }
    bool ssl_read_all(SSL *ssl, std::span<std::uint8_t> data) { std::size_t at {}; while(at<data.size()) { const auto n=SSL_read(ssl,data.data()+at,static_cast<int>(std::min<std::size_t>(data.size()-at, std::numeric_limits<int>::max()))); if(n<=0) return false; at+=static_cast<std::size_t>(n); } return true; }
    bool ssl_send_frame(SSL *ssl, const std::span<const std::uint8_t> frame) { if(frame.empty() || frame.size()>max_frame) return false; std::array<std::uint8_t,4> length {static_cast<std::uint8_t>(frame.size()>>24),static_cast<std::uint8_t>(frame.size()>>16),static_cast<std::uint8_t>(frame.size()>>8),static_cast<std::uint8_t>(frame.size())}; return ssl_write_all(ssl,length) && ssl_write_all(ssl,frame); }
    std::optional<std::vector<std::uint8_t>> ssl_receive_frame(SSL *ssl) { std::array<std::uint8_t,4> length {}; if(!ssl_read_all(ssl,length)) return std::nullopt; const std::size_t size=(std::size_t(length[0])<<24)|(std::size_t(length[1])<<16)|(std::size_t(length[2])<<8)|length[3]; if(size==0 || size>max_frame) return std::nullopt; std::vector<std::uint8_t> frame(size); return ssl_read_all(ssl,frame) ? std::optional<std::vector<std::uint8_t>>(std::move(frame)) : std::nullopt; }
    struct ssl_context { SSL_CTX *value {}; ~ssl_context(){ if(value) SSL_CTX_free(value); } ssl_context()=default; ssl_context(const ssl_context&)=delete; ssl_context &operator=(const ssl_context&)=delete; ssl_context(ssl_context &&other) noexcept:value(other.value){other.value=nullptr;} ssl_context &operator=(ssl_context &&other) noexcept{if(this!=&other){if(value)SSL_CTX_free(value);value=other.value;other.value=nullptr;}return *this;} };
    struct ssl_connection { SSL *value {}; ~ssl_connection(){ if(value){ SSL_shutdown(value); SSL_free(value); } } };
    std::optional<ssl_context> tls_context(const options &value, const bool server) { auto cert=read_file(value.certificate), key=read_file(value.private_key), ca=read_file(value.peer_ca); if(!cert||!key||!ca) return std::nullopt; ssl_context context; context.value=SSL_CTX_new(TLS_method()); if(!context.value || SSL_CTX_set_min_proto_version(context.value,TLS1_3_VERSION)!=1 || !atmos_mat_host::configure_mutual_tls(context.value,cert->c_str(),key->c_str(),ca->c_str())) return std::nullopt; SSL_CTX_set_verify_depth(context.value, 2); if(server) SSL_CTX_set_options(context.value, SSL_OP_NO_TICKET); return context; }
    bool set_peer_identity(SSL *ssl, const std::string &identity) { return ssl && SSL_set1_host(ssl, identity.c_str()) == 1; }
#endif
  }

  std::optional<options> parse(const int argc, const char *const argv[]) {
    if (argc < 2 || !argv || !argv[1]) return std::nullopt;
    options result; bool descriptor_seen {}; const std::string_view mode(argv[1]); if(mode=="host") result.mode=role::host; else if(mode=="client") result.mode=role::client; else return std::nullopt;
    for (int i=2;i<argc;i+=2) { if(i+1>=argc || !argv[i] || !argv[i+1]) return std::nullopt; const std::string_view key(argv[i]), value(argv[i+1]); auto put=[&](std::string &field){if(!field.empty()) return false; field=std::string(value); return true;};
      if(key=="--listen" && !put(result.listen)) return std::nullopt; else if(key=="--connect" && !put(result.connect)) return std::nullopt; else if(key=="--cert" && !put(result.certificate)) return std::nullopt; else if(key=="--key" && !put(result.private_key)) return std::nullopt; else if(key=="--peer-ca" && !put(result.peer_ca)) return std::nullopt; else if(key=="--peer-identity" && !put(result.peer_identity)) return std::nullopt; else if(key=="--endpoint" && !put(result.endpoint_id)) return std::nullopt; else if(key=="--tap" && !put(result.tap_path)) return std::nullopt; else if(key=="--descriptor" && (descriptor_seen || !(descriptor_seen=true, decode_hex(value,result.descriptor.bytes)))) return std::nullopt; else if(key=="--queue-frames" && !parse_u32(value,result.queue_frames)) return std::nullopt; else if(key!="--listen" && key!="--connect" && key!="--cert" && key!="--key" && key!="--peer-ca" && key!="--peer-identity" && key!="--endpoint" && key!="--tap" && key!="--descriptor" && key!="--queue-frames") return std::nullopt;
    }
    if(result.certificate.empty()||result.private_key.empty()||result.peer_ca.empty()||result.peer_identity.empty()||!descriptor_seen||result.queue_frames<atmos_mat_direct::max_payload_bytes/atmos_mat_direct::carrier_frame_bytes) return std::nullopt;
    if(result.mode==role::host) { if(result.listen.empty()||!result.connect.empty()||result.tap_path.empty()) return std::nullopt; } else if(result.connect.empty()||!result.listen.empty()||result.endpoint_id.empty()) return std::nullopt;
    return result;
  }

  bool memory_channel::send(std::vector<std::uint8_t> frame) { if(cancelled_||frame.empty()||frame.size()>max_frame) return false; frames_.push_back(std::move(frame)); return true; }
  std::optional<std::vector<std::uint8_t>> memory_channel::receive() { if(cancelled_||read_==frames_.size()) return std::nullopt; return std::move(frames_[read_++]); }
  void memory_channel::cancel() noexcept { cancelled_=true; frames_.clear(); }
  relay_result pump_in_memory(atmos_mat_host::relay &host, atmos_mat_client::writer &client, const atmos_mat_direct::descriptor &expected, const std::vector<std::vector<std::uint8_t>> &tap_records, memory_channel &channel) {
    relay_result result {}; bool started {}, stopped {}; for(const auto &wire:tap_records) { const auto record=atmos_mat_direct::decode(wire); if(!record || stopped || record->descriptor_value.bytes!=expected.bytes || ((!started && record->flags != atmos_mat_direct::start) || (record->flags & (atmos_mat_direct::discontinuity|atmos_mat_direct::format_change))) || host.admit({record->generation,record->stream_id,record->first_carrier_frame,record->host_qpc,record->host_qpc_frequency,record->descriptor_value,record->flags,record->payload})!=atmos_mat_host::admit_result::accepted||!channel.send(wire)) { channel.cancel(); result.cancelled=true; return result; } started=true; stopped=(record->flags&atmos_mat_direct::stop)!=0; }
    while(const auto frame=channel.receive()) { const auto record=atmos_mat_direct::decode(*frame); if(!record||!write_client(client,*record,client_descriptor(expected))) { channel.cancel(); result.cancelled=true; return result; } hash_append(result.client_sha256,record->payload); result.client_bytes+=record->payload.size(); result.carrier_frames+=record->payload.size()/atmos_mat_direct::carrier_frame_bytes; }
    result.host_bytes=host.bytes(); result.host_sha256=host.transcript_sha256(); result.clean=!tap_records.empty()&&!channel.cancelled()&&result.host_bytes==result.client_bytes&&result.host_sha256==result.client_sha256; if(!result.clean){channel.cancel();result.cancelled=true;} return result;
  }
  std::string hex_digest(const std::array<std::uint8_t,32> &digest) { static constexpr char hex[]="0123456789abcdef"; std::string result; result.reserve(64); for(auto b:digest){result.push_back(hex[b>>4]);result.push_back(hex[b&15]);} return result; }
  int run(const options &value) {
#ifdef _WIN32
    const auto fail_stage=[](const char *stage) { std::fprintf(stderr,"direct-relay failure stage=%s winsock=%d\n",stage,WSAGetLastError()); ERR_print_errors_fp(stderr); return 2; };
    winsock_runtime winsock; if(!winsock.ok) return fail_stage("winsock"); auto context=tls_context(value,value.mode==role::host); if(!context) return fail_stage("tls-context");
    socket_handle socket=value.mode==role::host ? tcp_open(value.listen,true) : tcp_open(value.connect,false); if(socket.value==INVALID_SOCKET) return fail_stage("tcp-open"); socket_handle accepted; if(value.mode==role::host){ accepted.value=accept(socket.value,nullptr,nullptr); if(accepted.value==INVALID_SOCKET)return fail_stage("tcp-accept"); } SSL *raw=SSL_new(context->value); if(!raw)return fail_stage("ssl-new"); ssl_connection connection{raw}; if(SSL_set_fd(raw,static_cast<int>(value.mode==role::host?accepted.value:socket.value))!=1)return fail_stage("ssl-fd"); if(!set_peer_identity(raw,value.peer_identity))return fail_stage("peer-identity"); if((value.mode==role::host ? SSL_accept(raw) : SSL_connect(raw))!=1)return fail_stage("tls-handshake"); if(SSL_get_verify_result(raw)!=X509_V_OK)return fail_stage("peer-verify");
    relay_result result {}; if(value.mode==role::host) { auto reader=atmos_mat_host::make_windows_control_device_reader(std::wstring(value.tap_path.begin(),value.tap_path.end()).c_str(),value.descriptor); atmos_mat_host::relay relay(value.descriptor,value.queue_frames); if(!reader)return 2; bool started {}, stopped {}; while(auto wire=reader->read()){auto record=atmos_mat_direct::decode(*wire);if(!record||stopped||record->descriptor_value.bytes!=value.descriptor.bytes||(!started&&record->flags!=atmos_mat_direct::start)||(record->flags&(atmos_mat_direct::discontinuity|atmos_mat_direct::format_change))||relay.admit({record->generation,record->stream_id,record->first_carrier_frame,record->host_qpc,record->host_qpc_frequency,record->descriptor_value,record->flags,record->payload})!=atmos_mat_host::admit_result::accepted||!ssl_send_frame(raw,*wire)){reader->cancel();return 2;} started=true; stopped=(record->flags&atmos_mat_direct::stop)!=0; if(stopped)break;} reader->cancel(); result.host_bytes=relay.bytes();result.host_sha256=relay.transcript_sha256();result.carrier_frames=result.host_bytes/atmos_mat_direct::carrier_frame_bytes;result.clean=started&&stopped; }
    else { const auto descriptor=client_descriptor(value.descriptor); atmos_mat_client::writer queue(descriptor,value.queue_frames); std::atomic<bool> started {}, stopped {}, failed {}, render_started {}; struct state { std::atomic<bool> *started, *stopped, *failed; atmos_mat_client::writer *queue; }; state control {&started,&stopped,&failed,&queue}; std::thread receiver([&] { bool saw_start {}; while(auto frame=ssl_receive_frame(raw)){auto record=atmos_mat_direct::decode(*frame); if(!record||record->descriptor_value.bytes!=value.descriptor.bytes||(!saw_start&&record->flags!=atmos_mat_direct::start)||(record->flags&(atmos_mat_direct::discontinuity|atmos_mat_direct::format_change))){failed=true;break;} const auto needed=record->payload.size()/atmos_mat_direct::carrier_frame_bytes; while(!failed && queue.queued_frames()>value.queue_frames-needed) std::this_thread::sleep_for(std::chrono::milliseconds(1)); if(failed||!write_client(queue,*record,descriptor)){failed=true;break;} saw_start=true; started=true; hash_append(result.client_sha256,record->payload);result.client_bytes+=record->payload.size();result.carrier_frames+=needed; if(record->flags&atmos_mat_direct::stop){stopped=true;break;} } if(!stopped)failed=true; }); while(!failed&&!stopped&&queue.queued_frames()<value.queue_frames/2) std::this_thread::sleep_for(std::chrono::milliseconds(1)); if(!failed&&started){render_started=true; const auto rendered=atmos_mat_client::render_windows_mat10_exclusive(queue,std::wstring(value.endpoint_id.begin(),value.endpoint_id.end()),descriptor,[](void *raw_state){auto &s=*static_cast<state *>(raw_state);return !s.failed->load()&&(!s.stopped->load()||s.queue->queued_frames()!=0);},&control,nullptr,nullptr); if(rendered.hresult!=0||rendered.poison!=atmos_mat_client::poison_reason::none)failed=true;} if(receiver.joinable())receiver.join(); result.clean=render_started&&started&&stopped&&!failed&&queue.poison()==atmos_mat_client::poison_reason::none; }
    std::fprintf(stderr,"direct-relay %s bytes=%llu frames=%llu sha256=%s status=%s\n",value.mode==role::host?"host":"client",static_cast<unsigned long long>(value.mode==role::host?result.host_bytes:result.client_bytes),static_cast<unsigned long long>(result.carrier_frames),hex_digest(value.mode==role::host?result.host_sha256:result.client_sha256).c_str(),result.clean?"clean":"failed"); return result.clean?0:2;
#else
    static_cast<void>(value); return 2;
#endif
  }
}  // namespace atmos_mat_relay_cli
