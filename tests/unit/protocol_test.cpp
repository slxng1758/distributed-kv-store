#include <iostream>
#include <string>

#include "common/protocol.hpp"

namespace {

using namespace kv::protocol;

int failures = 0;

void check(bool cond, const std::string& msg) {
  if (!cond) {
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
  }
}

void test_get_roundtrip() {
  Request req{Command::Get, "foo", ""};
  std::string encoded = encode_request(req);
  check(encoded == "GET foo\n", "GET should encode as 'GET foo\\n'");

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Complete,
        "GET request should parse as Complete");
  check(parsed.command == Command::Get && parsed.key == "foo",
        "parsed GET should match the original request");
}

void test_delete_roundtrip() {
  Request req{Command::Delete, "foo", ""};
  std::string encoded = encode_request(req);
  check(encoded == "DELETE foo\n", "DELETE should encode as 'DELETE foo\\n'");

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Complete,
        "DELETE request should parse as Complete");
  check(parsed.command == Command::Delete && parsed.key == "foo",
        "parsed DELETE should match the original request");
}

void test_set_roundtrip_with_embedded_space() {
  Request req{Command::Set, "foo", "bar baz"};
  std::string encoded = encode_request(req);
  check(encoded == "SET foo 7\nbar baz\n",
        "SET should encode with an explicit byte-length prefix");

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Complete,
        "SET request should parse as Complete");
  check(parsed.command == Command::Set && parsed.key == "foo" &&
            parsed.value == "bar baz",
        "parsed SET value should preserve an embedded space exactly");
}

void test_set_value_with_embedded_newline() {
  Request req{Command::Set, "k", std::string("line1\nline2")};
  std::string encoded = encode_request(req);

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Complete,
        "SET with an embedded newline in the value should still parse as "
        "Complete -- the length prefix, not a newline scan, delimits the "
        "value");
  check(parsed.value == "line1\nline2",
        "embedded newline in the value should be preserved exactly");
}

void test_incremental_feed_byte_by_byte() {
  Request req{Command::Set, "k", "hello"};
  std::string encoded = encode_request(req);

  IncrementalParser parser;
  Request parsed;
  ParseStatus status = ParseStatus::Incomplete;
  for (char c : encoded) {
    parser.feed(&c, 1);
    status = parser.try_parse_request(parsed);
    if (status != ParseStatus::Incomplete) break;
  }
  check(status == ParseStatus::Complete,
        "a request fed one byte at a time should still parse correctly "
        "(guards against assuming a full request always arrives in one read)");
  check(parsed.value == "hello", "value should be intact after a byte-by-byte feed");
}

void test_malformed_oversized_key() {
  std::string key(kMaxKeyLen + 1, 'a');
  std::string encoded = "GET " + key + "\n";

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Error,
        "an oversized key should produce a parse Error, not a crash or hang");
}

void test_malformed_negative_length() {
  std::string encoded = "SET foo -5\nxxxxx\n";

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Error,
        "a negative length should produce a parse Error");
}

void test_malformed_garbage_length() {
  std::string encoded = "SET foo abc\nxxxxx\n";

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Error,
        "a non-numeric length should produce a parse Error");
}

void test_truncated_value_is_incomplete_not_error() {
  // Declares a 10-byte value but only 3 bytes have arrived so far.
  std::string encoded = "SET foo 10\nabc";

  IncrementalParser parser;
  parser.feed(encoded.data(), encoded.size());
  Request parsed;
  check(parser.try_parse_request(parsed) == ParseStatus::Incomplete,
        "a value truncated mid-transmission should be Incomplete, not "
        "Error -- the rest may still arrive in a later read()");
}

void test_response_roundtrips() {
  {
    ResponseParser parser;
    std::string encoded = encode_value_response("bar");
    parser.feed(encoded.data(), encoded.size());
    Response resp;
    check(parser.try_parse_response(resp) == ParseStatus::Complete &&
              resp.type == ResponseType::Value && resp.value == "bar",
          "VALUE response should round-trip");
  }
  {
    ResponseParser parser;
    std::string encoded = encode_not_found_response();
    parser.feed(encoded.data(), encoded.size());
    Response resp;
    check(parser.try_parse_response(resp) == ParseStatus::Complete &&
              resp.type == ResponseType::NotFound,
          "NOT_FOUND response should round-trip");
  }
  {
    ResponseParser parser;
    std::string encoded = encode_error_response("bad request");
    parser.feed(encoded.data(), encoded.size());
    Response resp;
    check(parser.try_parse_response(resp) == ParseStatus::Complete &&
              resp.type == ResponseType::Error &&
              resp.message == "bad request",
          "ERROR response should round-trip with its message intact");
  }
}

}  // namespace

int main() {
  test_get_roundtrip();
  test_delete_roundtrip();
  test_set_roundtrip_with_embedded_space();
  test_set_value_with_embedded_newline();
  test_incremental_feed_byte_by_byte();
  test_malformed_oversized_key();
  test_malformed_negative_length();
  test_malformed_garbage_length();
  test_truncated_value_is_incomplete_not_error();
  test_response_roundtrips();

  if (failures > 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "protocol_test: all tests passed\n";
  return 0;
}
