#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <Simple-Web-Server/server_http.hpp>

namespace http::client_hdr {

inline constexpr std::size_t kMaxDecodedBase64ValueBytes = 4096;

enum class capability_status {
  absent,
  valid,
  invalid,
};

struct request_query_view {
  // The capability field is removed from this value. Other raw query
  // segments remain byte-for-byte unchanged.
  std::string sanitized_query;
  std::vector<std::pair<std::string, std::string>> parameters;
  std::optional<std::string> encoded_value;
  capability_status status = capability_status::absent;
};

request_query_view sanitize_request_query(std::string_view raw_query);

// A bounded, value-only snapshot of a Simple-Web request. Route handlers use
// this type instead of retaining the transport-owned Request object. The raw
// query is consumed only while constructing query, and the capability field
// is removed before the snapshot is exposed to a handler or logger.
struct request_view {
  std::string method;
  std::string path;
  SimpleWeb::CaseInsensitiveMultimap header;
  std::vector<std::string> path_match;
  std::string body;
  boost::asio::ip::tcp::endpoint local_endpoint;
  boost::asio::ip::tcp::endpoint remote_endpoint;
  request_query_view query;
  // Resolved during the TLS transport callback and copied into this value
  // before a route is queued.  It is intentionally an opaque paired-client
  // UUID rather than a certificate, endpoint cache key, or raw transport
  // object.
  std::optional<std::string> tls_client_uuid;
  bool tls = false;
};

template <typename RequestPtr>
request_view make_request_view(
  const RequestPtr &request,
  const bool tls,
  std::optional<std::string> tls_client_uuid = std::nullopt
) {
  request_view result;
  result.tls = tls;
  if (!request) {
    return result;
  }

  result.method = request->method;
  result.path = request->path;
  for (const auto &[name, value] : request->header) {
    result.header.emplace(name, value);
  }
  result.path_match.reserve(request->path_match.size());
  for (const auto &match : request->path_match) {
    result.path_match.emplace_back(match.str());
  }
  result.body = request->content.string();
  result.local_endpoint = request->local_endpoint();
  result.remote_endpoint = request->remote_endpoint();
  result.query = sanitize_request_query(request->query_string);
  result.tls_client_uuid = std::move(tls_client_uuid);
  return result;
}

template <typename Map>
Map parse_sanitized_query(std::string_view raw_query) {
  const auto view = sanitize_request_query(raw_query);
  Map result;
  for (const auto &[name, value] : view.parameters) {
    result.emplace(name, value);
  }
  return result;
}

template <typename Map>
Map parse_sanitized_query(const request_query_view &query) {
  Map result;
  for (const auto &[name, value] : query.parameters) {
    result.emplace(name, value);
  }
  return result;
}

}  // namespace http::client_hdr
