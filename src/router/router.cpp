#include "router/router.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include "common/protocol.hpp"

namespace kv {

namespace {

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

// Reads and returns exactly one full response from a backend connection.
protocol::Response read_one_response(int fd) {
  protocol::ResponseParser parser;
  protocol::Response resp;
  char buf[4096];
  while (true) {
    auto status = parser.try_parse_response(resp);
    if (status == protocol::ParseStatus::Complete) return resp;
    if (status == protocol::ParseStatus::Error) {
      throw std::runtime_error("backend protocol error: " + parser.error_message());
    }
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) throw std::runtime_error("backend connection closed unexpectedly");
    parser.feed(buf, static_cast<size_t>(n));
  }
}

// The backend's response is already in our own wire protocol, so this is
// a straight re-encode, not a translation -- kept as an explicit step
// (rather than forwarding raw bytes) because read_one_response() already
// had to fully parse the response to know where it ends.
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
  }
  return protocol::encode_error_response("internal router error");
}

}  // namespace

Router::Router(uint16_t port, std::vector<NodeAddress> nodes,
               size_t virtual_nodes_per_node)
    : listen_socket_(net::create_listening_socket(port)),
      nodes_(std::move(nodes)),
      ring_(virtual_nodes_per_node) {
  for (const auto& n : nodes_) ring_.add_node(n.id());
}

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

void Router::handle_client(int client_fd) {
  // Backend connections are cached per client-handling thread, keyed by
  // node id, and reused across requests on that connection -- avoids
  // reconnecting to a backend on every single request while needing no
  // cross-thread synchronization at all (each thread owns its own map).
  std::unordered_map<std::string, net::Socket> backend_conns;
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

      std::string node_id;
      try {
        node_id = ring_.get_node(req.key);

        auto it = backend_conns.find(node_id);
        if (it == backend_conns.end()) {
          const NodeAddress* addr = nullptr;
          for (const auto& n : nodes_) {
            if (n.id() == node_id) {
              addr = &n;
              break;
            }
          }
          if (addr == nullptr) {
            throw std::runtime_error("ring returned unknown node id: " + node_id);
          }
          net::Socket sock = net::connect_to(addr->host, addr->port);
          net::set_tcp_nodelay(sock.fd());
          it = backend_conns.emplace(node_id, std::move(sock)).first;
        }

        write_all(it->second.fd(), protocol::encode_request(req));
        protocol::Response resp = read_one_response(it->second.fd());
        write_all(client_fd, encode_response(resp));
      } catch (const std::exception& e) {
        // Drop the (likely broken) cached connection so the next request
        // to this node gets a fresh connect attempt, and surface a clear
        // error to the client. No retry/failover here -- that's Phase 4's
        // job; Phase 3 just needs to fail cleanly rather than hang.
        backend_conns.erase(node_id);
        write_all(client_fd, protocol::encode_error_response(
                                  "backend unavailable (" + node_id + "): " + e.what()));
      }
    }
  }

  ::close(client_fd);
}

}  // namespace kv
