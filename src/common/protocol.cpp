#include "common/protocol.hpp"

#include <cctype>
#include <vector>

namespace kv::protocol {

namespace {

bool is_valid_key(const std::string& key) {
  if (key.empty() || key.size() > kMaxKeyLen) return false;
  for (unsigned char c : key) {
    if (c <= 0x20) return false;  // no spaces or control chars
  }
  return true;
}

std::vector<std::string> split_spaces(const std::string& line) {
  std::vector<std::string> tokens;
  size_t start = 0;
  while (start <= line.size()) {
    size_t pos = line.find(' ', start);
    if (pos == std::string::npos) {
      tokens.push_back(line.substr(start));
      break;
    }
    tokens.push_back(line.substr(start, pos - start));
    start = pos + 1;
  }
  return tokens;
}

bool parse_length(const std::string& s, size_t& out) {
  if (s.empty() || s.size() > 10) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  unsigned long long v;
  try {
    v = std::stoull(s);
  } catch (...) {
    return false;
  }
  if (v > kMaxValueLen) return false;
  out = static_cast<size_t>(v);
  return true;
}

// Command lines are strictly LF-terminated per the spec, but a trailing CR
// is tolerated and stripped so the protocol stays typeable from tools that
// send CRLF (telnet in some configurations) without complicating the grammar.
std::string strip_trailing_cr(std::string line) {
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

}  // namespace

std::string encode_request(const Request& req) {
  switch (req.command) {
    case Command::Get:
      return "GET " + req.key + "\n";
    case Command::Delete:
      return "DELETE " + req.key + "\n";
    case Command::Set: {
      std::string out =
          "SET " + req.key + " " + std::to_string(req.value.size()) + "\n";
      out += req.value;
      out += "\n";
      return out;
    }
  }
  return "";
}

std::string encode_value_response(const std::string& value) {
  std::string out = "VALUE " + std::to_string(value.size()) + "\n";
  out += value;
  out += "\n";
  return out;
}

std::string encode_not_found_response() { return "NOT_FOUND\n"; }
std::string encode_ok_response() { return "OK\n"; }
std::string encode_deleted_response() { return "DELETED\n"; }

std::string encode_error_response(const std::string& message) {
  return "ERROR " + message + "\n";
}

void IncrementalParser::feed(const char* data, size_t len) {
  buffer_.append(data, len);
}

ParseStatus IncrementalParser::try_parse_request(Request& out_request) {
  // "SET " + key(<=250) + " " + length(<=10 digits)
  constexpr size_t kMaxCommandLineLen = 4 + kMaxKeyLen + 1 + 10;

  size_t line_end = buffer_.find('\n');
  if (line_end == std::string::npos) {
    if (buffer_.size() > kMaxCommandLineLen) {
      error_message_ = "command line too long";
      return ParseStatus::Error;
    }
    return ParseStatus::Incomplete;
  }

  std::string line = strip_trailing_cr(buffer_.substr(0, line_end));
  std::vector<std::string> tokens = split_spaces(line);

  if (tokens.empty() || tokens[0].empty()) {
    error_message_ = "empty command";
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Error;
  }

  const std::string& cmd = tokens[0];

  if (cmd == "GET" || cmd == "DELETE") {
    if (tokens.size() != 2 || !is_valid_key(tokens[1])) {
      error_message_ = "malformed " + cmd + " request";
      buffer_.erase(0, line_end + 1);
      return ParseStatus::Error;
    }
    out_request.command = (cmd == "GET") ? Command::Get : Command::Delete;
    out_request.key = tokens[1];
    out_request.value.clear();
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Complete;
  }

  if (cmd == "SET") {
    size_t value_len = 0;
    if (tokens.size() != 3 || !is_valid_key(tokens[1]) ||
        !parse_length(tokens[2], value_len)) {
      error_message_ = "malformed SET request";
      buffer_.erase(0, line_end + 1);
      return ParseStatus::Error;
    }

    size_t value_start = line_end + 1;
    size_t needed = value_start + value_len + 1;  // value bytes + trailing LF
    if (buffer_.size() < needed) {
      return ParseStatus::Incomplete;
    }
    if (buffer_[value_start + value_len] != '\n') {
      error_message_ = "expected newline after value";
      buffer_.erase(0, needed);
      return ParseStatus::Error;
    }

    out_request.command = Command::Set;
    out_request.key = tokens[1];
    out_request.value = buffer_.substr(value_start, value_len);
    buffer_.erase(0, needed);
    return ParseStatus::Complete;
  }

  error_message_ = "unknown command: " + cmd;
  buffer_.erase(0, line_end + 1);
  return ParseStatus::Error;
}

void ResponseParser::feed(const char* data, size_t len) {
  buffer_.append(data, len);
}

ParseStatus ResponseParser::try_parse_response(Response& out_response) {
  size_t line_end = buffer_.find('\n');
  if (line_end == std::string::npos) {
    constexpr size_t kMaxResponseLineLen = 4096;
    if (buffer_.size() > kMaxResponseLineLen) {
      error_message_ = "response line too long";
      return ParseStatus::Error;
    }
    return ParseStatus::Incomplete;
  }

  std::string line = strip_trailing_cr(buffer_.substr(0, line_end));

  if (line == "OK") {
    out_response.type = ResponseType::Ok;
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Complete;
  }
  if (line == "DELETED") {
    out_response.type = ResponseType::Deleted;
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Complete;
  }
  if (line == "NOT_FOUND") {
    out_response.type = ResponseType::NotFound;
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Complete;
  }
  if (line == "ERROR" || line.rfind("ERROR ", 0) == 0) {
    out_response.type = ResponseType::Error;
    out_response.message = line.size() > 6 ? line.substr(6) : "";
    buffer_.erase(0, line_end + 1);
    return ParseStatus::Complete;
  }
  if (line.rfind("VALUE ", 0) == 0) {
    size_t value_len = 0;
    if (!parse_length(line.substr(6), value_len)) {
      error_message_ = "malformed VALUE response";
      buffer_.erase(0, line_end + 1);
      return ParseStatus::Error;
    }

    size_t value_start = line_end + 1;
    size_t needed = value_start + value_len + 1;
    if (buffer_.size() < needed) {
      return ParseStatus::Incomplete;
    }
    if (buffer_[value_start + value_len] != '\n') {
      error_message_ = "expected newline after value";
      buffer_.erase(0, needed);
      return ParseStatus::Error;
    }

    out_response.type = ResponseType::Value;
    out_response.value = buffer_.substr(value_start, value_len);
    buffer_.erase(0, needed);
    return ParseStatus::Complete;
  }

  error_message_ = "unknown response: " + line;
  buffer_.erase(0, line_end + 1);
  return ParseStatus::Error;
}

}  // namespace kv::protocol
