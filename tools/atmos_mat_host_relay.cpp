#include "tools/atmos_mat_host_relay.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <memory>

namespace atmos_mat_host {
  namespace { void digest(std::array<std::uint8_t,32> &out, std::span<const std::uint8_t> old, std::span<const std::uint8_t> next) { EVP_MD_CTX *ctx=EVP_MD_CTX_new(); if (!ctx) return; unsigned int n {}; EVP_DigestInit_ex(ctx,EVP_sha256(),nullptr); EVP_DigestUpdate(ctx,old.data(),old.size()); EVP_DigestUpdate(ctx,next.data(),next.size()); EVP_DigestFinal_ex(ctx,out.data(),&n); EVP_MD_CTX_free(ctx); } }
  std::optional<tap_session_identity> validate_tap_session(std::uint64_t driver_generation, std::uint64_t stream_id) {
    if (!driver_generation || !stream_id) return std::nullopt;
    return tap_session_identity {driver_generation, stream_id};
  }
  relay::relay(descriptor_identity expected, std::size_t capacity) : expected_(expected), capacity_(capacity) {}
  admit_result relay::admit(const carrier_block &b) {
    const auto fail=[this](terminal_reason r, admit_result a){ poisoned_=true; terminal_=r; queue_.clear(); return a; };
    if (poisoned_ || b.bytes.empty() || b.bytes.size() % carrier_frame_bytes || !b.host_qpc_frequency) return fail(terminal_reason::malformed,admit_result::malformed_poisoned);
    if (b.descriptor.bytes != expected_.bytes) return fail(terminal_reason::descriptor_mismatch,admit_result::descriptor_mismatch_poisoned);
    if (!initialized_) { initialized_=true; generation_=b.generation; stream_=b.stream_id; next_frame_=b.first_carrier_frame; }
    if (b.generation != generation_) return fail(terminal_reason::generation_change,admit_result::generation_changed_poisoned);
    if (b.stream_id != stream_) return fail(terminal_reason::stream_change,admit_result::stream_changed_poisoned);
    if (b.first_carrier_frame != next_frame_) return fail(terminal_reason::gap,admit_result::gap_poisoned);
    if (queue_.size() == capacity_) return fail(terminal_reason::queue_full,admit_result::queue_full_poisoned);
    digest(digest_, digest_, b.bytes); bytes_ += b.bytes.size(); next_frame_ += b.bytes.size()/carrier_frame_bytes; queue_.push_back(b); return admit_result::accepted;
  }
  std::optional<carrier_block> relay::pop() { if (queue_.empty()) return std::nullopt; auto result=std::move(queue_.front()); queue_.erase(queue_.begin()); return result; }
  void relay::reset() { queue_.clear(); initialized_=poisoned_=false; terminal_=terminal_reason::none; generation_=stream_=next_frame_=bytes_=0; digest_={}; }
  bool relay::poisoned() const { return poisoned_; } terminal_reason relay::terminal() const { return terminal_; } std::array<std::uint8_t,32> relay::transcript_sha256() const { return digest_; } std::uint64_t relay::bytes() const { return bytes_; }
  terminal_reason reader::pump_once() { auto wire=transport_.read(); if (!wire) return target_.terminal()==terminal_reason::none ? terminal_reason::eof : target_.terminal(); if (wire->empty()) return terminal_reason::none; auto record=atmos_mat_direct::decode(*wire); if (!record) { target_.admit({}); return target_.terminal(); } carrier_block b {record->generation,record->stream_id,record->first_carrier_frame,record->host_qpc,record->host_qpc_frequency,record->descriptor_value,record->flags,std::move(record->payload)}; target_.admit(b); return target_.terminal(); }
  void reader::cancel() noexcept { transport_.cancel(); }
  bool configure_mutual_tls(SSL_CTX *ctx, const char *cert, const char *key, const char *ca) {
    if (!ctx || !cert || !key || !ca) return false; auto cb=BIO_new_mem_buf(cert,-1); auto kb=BIO_new_mem_buf(key,-1); auto ab=BIO_new_mem_buf(ca,-1); if(!cb||!kb||!ab) return false; auto x=PEM_read_bio_X509(cb,nullptr,nullptr,nullptr); auto p=PEM_read_bio_PrivateKey(kb,nullptr,nullptr,nullptr); auto trust=PEM_read_bio_X509(ab,nullptr,nullptr,nullptr); BIO_free(cb); BIO_free(kb); BIO_free(ab); if(!x||!p||!trust){X509_free(x);EVP_PKEY_free(p);X509_free(trust);return false;} X509_STORE *s=SSL_CTX_get_cert_store(ctx); const bool ok=SSL_CTX_use_certificate(ctx,x)==1 && SSL_CTX_use_PrivateKey(ctx,p)==1 && SSL_CTX_check_private_key(ctx)==1 && X509_STORE_add_cert(s,trust)==1; X509_free(x);EVP_PKEY_free(p);X509_free(trust); if(ok) SSL_CTX_set_verify(ctx,SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,nullptr); return ok;
  }
}
