#include "kvstore/net/listener.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace kvstore::net {

namespace {

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

Listener::Listener(const ListenerConfig& config) {
  FileDescriptor sock(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!sock.valid()) {
    throw_errno("Listener: socket() failed");
  }

  constexpr int kEnable = 1;
  if (::setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &kEnable, sizeof(kEnable)) != 0) {
    throw_errno("Listener: setsockopt(SO_REUSEADDR) failed");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(config.port);
  if (::inet_pton(AF_INET, config.bind_address.c_str(), &addr.sin_addr) != 1) {
    throw std::invalid_argument("Listener: invalid IPv4 bind address '" + config.bind_address +
                                "'");
  }

  if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw_errno("Listener: bind() failed");
  }

  if (config.backlog <= 0) {
    throw std::invalid_argument("Listener: backlog must be positive");
  }

  if (::listen(sock.get(), config.backlog) != 0) {
    throw_errno("Listener: listen() failed");
  }

  fd_ = std::move(sock);
}

std::uint16_t Listener::port() const {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd_.get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    throw_errno("Listener: getsockname() failed");
  }
  return ntohs(addr.sin_port);
}

std::optional<AcceptedClient> Listener::accept_one() {
  for (;;) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    const int client_fd = ::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&addr), &len,
                                    SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (client_fd >= 0) {
      char address_buf[INET_ADDRSTRLEN] = {};
      ::inet_ntop(AF_INET, &addr.sin_addr, address_buf, sizeof(address_buf));
      std::string peer_address =
          std::string(address_buf) + ":" + std::to_string(ntohs(addr.sin_port));
      return AcceptedClient{.fd = FileDescriptor(client_fd),
                            .peer_address = std::move(peer_address)};
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return std::nullopt;
    }
    if (errno == EINTR || errno == ECONNABORTED) {
      continue;  // transient; retry immediately
    }
    throw_errno("Listener: accept4() failed");
  }
}

}  // namespace kvstore::net
