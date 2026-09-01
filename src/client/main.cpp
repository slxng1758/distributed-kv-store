#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/protocol.hpp"
#include "common/socket_utils.hpp"

// kvclient: a one-shot CLI ("kvclient GET foo") or an interactive REPL
// (no command args) over a single persistent connection. Exists mainly
// because raw nc/telnet can't drive SET -- the protocol requires an exact
// byte-length prefix that's error-prone to compute by hand; kvclient
// computes it automatically from the value the user types.

namespace {

using kv::protocol::Command;
using kv::protocol::Request;

bool build_request(const std::vector<std::string>& tokens, Request& out,
                    std::string& err) {
  if (tokens.empty()) {
    err = "empty command";
    return false;
  }
  const std::string& cmd = tokens[0];

  if (cmd == "GET" || cmd == "get") {
    if (tokens.size() != 2) {
      err = "usage: GET key";
      return false;
    }
    out.command = Command::Get;
    out.key = tokens[1];
    return true;
  }
  if (cmd == "DELETE" || cmd == "delete") {
    if (tokens.size() != 2) {
      err = "usage: DELETE key";
      return false;
    }
    out.command = Command::Delete;
    out.key = tokens[1];
    return true;
  }
  if (cmd == "SET" || cmd == "set") {
    if (tokens.size() < 3) {
      err = "usage: SET key value...";
      return false;
    }
    out.command = Command::Set;
    out.key = tokens[1];
    std::string value = tokens[2];
    for (size_t i = 3; i < tokens.size(); ++i) {
      value += " ";
      value += tokens[i];
    }
    out.value = value;
    return true;
  }
  if (cmd == "PING" || cmd == "ping") {
    if (tokens.size() != 1) {
      err = "usage: PING";
      return false;
    }
    out.command = Command::Ping;
    return true;
  }

  err = "unknown command: " + cmd;
  return false;
}

// 0 for any well-formed protocol exchange, including NOT_FOUND; 1 only for
// a server-side ERROR response.
int print_response(const kv::protocol::Response& resp) {
  using kv::protocol::ResponseType;
  switch (resp.type) {
    case ResponseType::Ok:
      std::cout << "OK\n";
      return 0;
    case ResponseType::Deleted:
      std::cout << "DELETED\n";
      return 0;
    case ResponseType::NotFound:
      std::cout << "NOT_FOUND\n";
      return 0;
    case ResponseType::Value:
      std::cout << resp.value << "\n";
      return 0;
    case ResponseType::Pong:
      std::cout << "PONG\n";
      return 0;
    case ResponseType::Error:
      std::cerr << "ERROR " << resp.message << "\n";
      return 1;
  }
  return 1;
}

std::vector<std::string> split_whitespace(const std::string& line) {
  std::istringstream iss(line);
  std::vector<std::string> tokens;
  std::string tok;
  while (iss >> tok) tokens.push_back(tok);
  return tokens;
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 6380;
  std::vector<std::string> command_args;

  int i = 1;
  for (; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::atoi(argv[++i]));
    } else {
      break;
    }
  }
  for (; i < argc; ++i) command_args.push_back(argv[i]);

  try {
    kv::net::Socket sock = kv::net::connect_to(host, port);
    kv::net::set_tcp_nodelay(sock.fd());

    if (!command_args.empty()) {
      Request req;
      std::string err;
      if (!build_request(command_args, req, err)) {
        std::cerr << "kvclient: " << err << "\n";
        return 1;
      }
      return print_response(kv::protocol::send_request(sock.fd(), req));
    }

    std::string line;
    while (std::getline(std::cin, line)) {
      auto tokens = split_whitespace(line);
      if (tokens.empty()) continue;
      Request req;
      std::string err;
      if (!build_request(tokens, req, err)) {
        std::cerr << "kvclient: " << err << "\n";
        continue;
      }
      print_response(kv::protocol::send_request(sock.fd(), req));
    }
  } catch (const std::exception& e) {
    std::cerr << "kvclient: fatal: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
