#include "router/router.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace kv {

namespace {

constexpr std::chrono::milliseconds kHeartbeatInterval{200};
constexpr std::chrono::milliseconds kHeartbeatTimeout{400};

void write_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
  }
}

// The backend's response is already in our own wire protocol, so this is
// a straight re-encode, not a translation.
std::string encode_response(const protocol::Response& resp) {
  using protocol::ResponseType;
  switch (resp.type) {
    case ResponseType::Value:
      return protocol::encode_value_response(resp.value);
    case ResponseType::NotFound:
      return protocol::encode_not_found_response();
    case ResponseType::Ok:
      return protocol::encode_ok_response();
    case ResponseType::Deleted:
      return protocol::encode_deleted_response();
    case ResponseType::Error:
      return protocol::encode_error_response(resp.message);
    case ResponseType::Pong:
      return protocol::encode_pong_response();
  }
  return protocol::encode_error_response("internal router error");
}

protocol::Response no_live_replica_error(size_t tried) {
  protocol::Response err;
  err.type = protocol::ResponseType::Error;
  err.message = "no live replica for key (tried " + std::to_string(tried) + ")";
  return err;
}

}  // namespace

Router::Router(uint16_t port, std::vector<NodeAddress> nodes,
               size_t virtual_nodes_per_node, size_t replication_factor)
    : listen_socket_(net::create_listening_socket(port)),
      nodes_(nodes),
      ring_(virtual_nodes_per_node),
      replication_factor_(std::max<size_t>(1, replication_factor)),
      failure_detector_(std::move(nodes), kHeartbeatInterval, kHeartbeatTimeout) {
  for (const auto& n : nodes_) ring_.add_node(n.id());
  failure_detector_.start();
}

Router::~Router() = default;

void Router::run() {
  while (true) {
    int client_fd = ::accept(listen_socket_.fd(), nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      std::cerr << "kvrouter: accept() failed: " << std::strerror(errno)
                << ", continuing\n";
      continue;
    }
    net::set_tcp_nodelay(client_fd);
    std::thread([this, client_fd] { handle_client(client_fd); }).detach();
  }
}

const NodeAddress* Router::find_address(const std::string& node_id) const {
  for (const auto& n : nodes_) {
    if (n.id() == node_id) return &n;
  }
  return nullptr;
}

net::Socket& Router::backend_connection(const std::string& node_id, BackendConns& conns) {
  auto it = conns.find(node_id);
  if (it != conns.end()) return it->second;

  const NodeAddress* addr = find_address(node_id);
  if (addr == nullptr) {
    throw std::runtime_error("ring returned unknown node id: " + node_id);
  }
  net::Socket sock = net::connect_to(addr->host, addr->port);
  net::set_tcp_nodelay(sock.fd());
  auto inserted = conns.emplace(node_id, std::move(sock));
  return inserted.first->second;
}

protocol::Response Router::route_write(const protocol::Request& req, BackendConns& conns) {
  std::vector<std::string> preference_list = ring_.get_nodes(req.key, replication_factor_);

  bool any_success = false;
  protocol::Response first_success{};

  for (const auto& node_id : preference_list) {
    if (!failure_detector_.is_alive(node_id)) continue;
    try {
      net::Socket& sock = backend_connection(node_id, conns);
      protocol::Response resp = protocol::send_request(sock.fd(), req);
      if (!any_success) first_success = resp;
      any_success = true;
    } catch (const std::exception&) {
      failure_detector_.mark_down(node_id);
      conns.erase(node_id);
    }
  }

  if (!any_success) return no_live_replica_error(preference_list.size());
  return first_success;
}

protocol::Response Router::route_read(const protocol::Request& req, BackendConns& conns) {
  std::vector<std::string> preference_list = ring_.get_nodes(req.key, replication_factor_);

  for (const auto& node_id : preference_list) {
    if (!failure_detector_.is_alive(node_id)) continue;
    try {
      net::Socket& sock = backend_connection(node_id, conns);
      return protocol::send_request(sock.fd(), req);
    } catch (const std::exception&) {
      failure_detector_.mark_down(node_id);
      conns.erase(node_id);
    }
  }

  return no_live_replica_error(preference_list.size());
}

void Router::handle_client(int client_fd) {
  // Backend connections are cached per client-handling thread, keyed by
  // node id, and reused across requests -- avoids reconnecting to a
  // backend on every single request while needing no cross-thread
  // synchronization (each thread owns its own map).
  BackendConns backend_conns;
  protocol::IncrementalParser parser;
  char buf[4096];

  while (true) {
    ssize_t n = ::read(client_fd, buf, sizeof(buf));
    if (n <= 0) break;  // client disconnected or read error
    parser.feed(buf, static_cast<size_t>(n));

    protocol::Request req;
    protocol::ParseStatus status;
    while ((status = parser.try_parse_request(req)) !=
           protocol::ParseStatus::Incomplete) {
      if (status == protocol::ParseStatus::Error) {
        write_all(client_fd, protocol::encode_error_response(parser.error_message()));
        continue;
      }

      std::string response_bytes;
      if (req.command == protocol::Command::Ping) {
        // The router itself is reachable, so it can answer directly --
        // there's no key to route by, and this is also handy for probing
        // "is the router up" independent of any backend node.
        response_bytes = protocol::encode_pong_response();
      } else if (req.command == protocol::Command::Get) {
        response_bytes = encode_response(route_read(req, backend_conns));
      } else {
        response_bytes = encode_response(route_write(req, backend_conns));
      }
      write_all(client_fd, response_bytes);
    }
  }

  ::close(client_fd);
}

}  // namespace kv
