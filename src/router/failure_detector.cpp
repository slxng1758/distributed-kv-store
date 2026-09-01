#include "router/failure_detector.hpp"

#include "common/protocol.hpp"
#include "common/socket_utils.hpp"

namespace kv {

FailureDetector::FailureDetector(std::vector<NodeAddress> nodes,
                                  std::chrono::milliseconds interval,
                                  std::chrono::milliseconds timeout)
    : nodes_(std::move(nodes)), interval_(interval), timeout_(timeout) {
  // Optimistic initial state: assume every node is up until the first
  // heartbeat (or a reactive failure) says otherwise, so routing isn't
  // needlessly blocked before the first heartbeat cycle completes.
  for (const auto& n : nodes_) alive_[n.id()] = true;
}

FailureDetector::~FailureDetector() { stop(); }

void FailureDetector::start() {
  running_.store(true, std::memory_order_relaxed);
  thread_ = std::thread([this] { heartbeat_loop(); });
}

void FailureDetector::stop() {
  running_.store(false, std::memory_order_relaxed);
  if (thread_.joinable()) thread_.join();
}

bool FailureDetector::is_alive(const std::string& node_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = alive_.find(node_id);
  return it == alive_.end() ? false : it->second;
}

void FailureDetector::mark_down(const std::string& node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  alive_[node_id] = false;
}

void FailureDetector::mark_up(const std::string& node_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  alive_[node_id] = true;
}

bool FailureDetector::ping_once(const NodeAddress& addr) {
  try {
    net::Socket sock = net::connect_to(addr.host, addr.port);
    net::set_recv_timeout(sock.fd(), static_cast<int>(timeout_.count()));
    protocol::Request req;
    req.command = protocol::Command::Ping;
    protocol::Response resp = protocol::send_request(sock.fd(), req);
    return resp.type == protocol::ResponseType::Pong;
  } catch (const std::exception&) {
    return false;
  }
}

void FailureDetector::heartbeat_loop() {
  while (running_.load(std::memory_order_relaxed)) {
    for (const auto& addr : nodes_) {
      if (!running_.load(std::memory_order_relaxed)) break;
      bool up = ping_once(addr);
      if (up) {
        mark_up(addr.id());
      } else {
        mark_down(addr.id());
      }
    }
    std::this_thread::sleep_for(interval_);
  }
}

}  // namespace kv
