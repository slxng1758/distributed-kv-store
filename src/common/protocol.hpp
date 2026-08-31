#pragma once

#include <cstddef>
#include <string>

// Wire protocol for the KV store: newline-delimited command lines with a
// length-prefixed, binary-safe value payload for SET/VALUE. See
// docs/PROTOCOL.md for the full grammar and rationale.
namespace kv::protocol {

constexpr size_t kMaxKeyLen = 250;
constexpr size_t kMaxValueLen = 1024 * 1024;  // 1 MiB

enum class Command { Get, Set, Delete };

struct Request {
  Command command;
  std::string key;
  std::string value;  // only meaningful for Set
};

std::string encode_request(const Request& req);

std::string encode_value_response(const std::string& value);
std::string encode_not_found_response();
std::string encode_ok_response();
std::string encode_deleted_response();
std::string encode_error_response(const std::string& message);

enum class ParseStatus { Incomplete, Complete, Error };

// Buffers partial reads from a client socket and yields complete requests.
// Handles TCP segmentation: feed() may be called with arbitrarily small
// chunks, and try_parse_request() only consumes bytes once a full request
// is available.
class IncrementalParser {
 public:
  void feed(const char* data, size_t len);

  // On Complete, out_request is populated and the consumed bytes are
  // dropped from the internal buffer. On Error, error_message() explains
  // why and the offending bytes are dropped. On Incomplete, nothing is
  // consumed; call again after feeding more data.
  ParseStatus try_parse_request(Request& out_request);

  const std::string& error_message() const { return error_message_; }

 private:
  std::string buffer_;
  std::string error_message_;
};

enum class ResponseType { Value, NotFound, Ok, Deleted, Error };

struct Response {
  ResponseType type;
  std::string value;    // for Value
  std::string message;  // for Error
};

// Same buffering strategy as IncrementalParser, for the client side.
class ResponseParser {
 public:
  void feed(const char* data, size_t len);
  ParseStatus try_parse_response(Response& out_response);
  const std::string& error_message() const { return error_message_; }

 private:
  std::string buffer_;
  std::string error_message_;
};

}  // namespace kv::protocol
