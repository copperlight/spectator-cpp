#include "publisher.h"
#include "logger.h"
#include <fmt/format.h>

namespace spectator {

static const char NEW_LINE = '\n';

SpectatordPublisher::SpectatordPublisher(absl::string_view endpoint,
                                         uint32_t bytes_to_buffer,
                                         std::shared_ptr<spdlog::logger> logger)
    : logger_(std::move(logger)),
      udp_socket_(io_context_),
      local_socket_(io_context_), bytes_to_buffer_(bytes_to_buffer) {
  buffer_.reserve(bytes_to_buffer_ + 1024);     
  if (absl::StartsWith(endpoint, "unix:")) {
    this->unixDomainPath_ = std::string(endpoint.substr(5));
    setup_unix_domain();
  } else if (absl::StartsWith(endpoint, "udp:")) {
    auto pos = 4;
    // if the user used udp://foo:1234 instead of udp:foo:1234
    // adjust accordingly
    if (endpoint.substr(pos, 2) == "//") {
      pos += 2;
    }
    setup_udp(endpoint.substr(pos));
  } else if (endpoint != "disabled") {
    logger_->warn(
        "Unknown endpoint: '{}'. Expecting: 'unix:/path/to/socket'"
        " or 'udp:hostname:port' - Will not send metrics",
        std::string(endpoint));
    setup_nop_sender();
  }
}

void SpectatordPublisher::setup_nop_sender() {
  sender_ = [this](std::string_view msg) { logger_->trace("{}", msg); };
}

void SpectatordPublisher::local_reconnect(absl::string_view path) {
  using endpoint_t = asio::local::datagram_protocol::endpoint;
  try {
    if (local_socket_.is_open()) {
      local_socket_.close();
    }
    local_socket_.open();
    local_socket_.connect(endpoint_t(std::string(path)));
  } catch (std::exception& e) {
    logger_->warn("Unable to connect to {}: {}", std::string(path), e.what());
  }
}


bool SpectatordPublisher::try_to_send(const std::string& buffer) {
  for (auto i = 0; i < 3; ++i) {
    try {
      auto sent_bytes = local_socket_.send(asio::buffer(buffer));
      logger_->trace("Sent (local): {} bytes, in total had {}", sent_bytes,
                     buffer.length());
      return true;
    } catch (std::exception& e) {
      local_reconnect(this->unixDomainPath_);
      logger_->warn("Unable to send {} - attempt {}/3 ({})", buffer, i, e.what());
    }
  }
  return false;
}

void SpectatordPublisher::taskThreadFunction() try {
  while (shutdown_.load() == false) {
    std::string message {};
    {
      std::unique_lock<std::mutex> lock(mtx_);
      cv_sender_.wait(lock, [this] { return buffer_.size() > bytes_to_buffer_ || shutdown_.load();});
      if (shutdown_.load() == true) {
        return;
      }
      message = std::move(buffer_);
      buffer_ = std::string();
      buffer_.reserve(bytes_to_buffer_);
    }
    cv_receiver_.notify_one();
    try_to_send(message);
  }
} catch (const std::exception& e) {
  logger_->error("Fatal error in message processing thread: {}", e.what());
}

void SpectatordPublisher::setup_unix_domain(){
  // Reset connection to the unix domain socket
  local_reconnect(this->unixDomainPath_);
  if (bytes_to_buffer_ == 0) {
    sender_ = [this](std::string_view msg) {
      try_to_send(std::string(msg));
    };
    return;
  }
  else {
    sender_ = [this](std::string_view msg) {
      unsigned int currentBufferSize = buffer_.size();
      {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_receiver_.wait(lock, [this] { return buffer_.size() <= bytes_to_buffer_ || shutdown_.load(); }); 
        if (shutdown_.load()) {
          return;
        }
        buffer_.append(msg.data(), msg.size());
        buffer_.append(1, NEW_LINE);
        currentBufferSize = buffer_.size();
      }
      currentBufferSize > bytes_to_buffer_ ? cv_sender_.notify_one() : cv_receiver_.notify_one();
    };
    this->sendingThread_ = std::thread(&SpectatordPublisher::taskThreadFunction, this);
  }
}

inline asio::ip::udp::endpoint resolve_host_port(
    asio::io_context& io_context,  // NOLINT
    absl::string_view host_port) {
  using asio::ip::udp;
  udp::resolver resolver{io_context};

  auto end_host = host_port.find(':');
  if (end_host == std::string_view::npos) {
    auto err = fmt::format(
        "Unable to parse udp endpoint: '{}'. Expecting hostname:port",
        std::string(host_port));
    throw std::runtime_error(err);
  }

  auto host = host_port.substr(0, end_host);
  auto port = host_port.substr(end_host + 1);
  return *resolver.resolve(udp::v6(), std::string(host), std::string(port));
}

void SpectatordPublisher::udp_reconnect(
    const asio::ip::udp::endpoint& endpoint) {
  try {
    if (udp_socket_.is_open()) {
      udp_socket_.close();
    }
    udp_socket_.open(asio::ip::udp::v6());
    udp_socket_.connect(endpoint);
  } catch (std::exception& e) {
    logger_->warn("Unable to connect to {}: {}", endpoint.address().to_string(),
                  endpoint.port());
  }
}

void SpectatordPublisher::setup_udp(absl::string_view host_port) {
  auto endpoint = resolve_host_port(io_context_, host_port);
  udp_reconnect(endpoint);
  sender_ = [endpoint, this](std::string_view msg) {
    for (auto i = 0; i < 3; ++i) {
      try {
        udp_socket_.send(asio::buffer(msg));
        logger_->trace("Sent (udp): {}", msg);
        break;
      } catch (std::exception& e) {
        logger_->warn("Unable to send {} - attempt {}/3", msg, i);
        udp_reconnect(endpoint);
      }
    }
  };
}
}  // namespace spectator
