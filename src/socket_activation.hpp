#pragma once

#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace onekvm::activation {

// Match systemd's LISTEN_PID/LISTEN_FDS contract without linking a bus client
// into protocol daemons. Only one listening TCP socket is supported.
inline bool TakeListener(int &fd, int &family, std::string &error) {
  fd = -1;
  family = AF_UNSPEC;
  const auto *pid_text = std::getenv("LISTEN_PID");
  const auto *fds_text = std::getenv("LISTEN_FDS");
  if (pid_text == nullptr || fds_text == nullptr)
    return true;
  const auto parse = [](const char *text, int &value) {
    const std::string_view input(text);
    const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    return result.ec == std::errc{} && result.ptr == input.data() + input.size();
  };
  int pid = 0, count = 0;
  if (!parse(pid_text, pid) || pid != getpid())
    return true;
  if (!parse(fds_text, count) || count != 1) {
    error = "socket activation requires exactly one listening TCP socket";
    return false;
  }
  constexpr int listener = 3;
  int type = 0, listening = 0;
  socklen_t size = sizeof(type);
  sockaddr_storage address{};
  socklen_t address_size = sizeof(address);
  if (getsockopt(listener, SOL_SOCKET, SO_TYPE, &type, &size) != 0 ||
      type != SOCK_STREAM ||
      getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) != 0 ||
      listening != 1 ||
      getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_size) != 0 ||
      (address.ss_family != AF_INET && address.ss_family != AF_INET6)) {
    error = "socket activation fd 3 is not a listening TCP socket";
    return false;
  }
  const int descriptor_flags = fcntl(listener, F_GETFD);
  const int status_flags = fcntl(listener, F_GETFL);
  if (descriptor_flags < 0 || status_flags < 0 ||
      fcntl(listener, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0 ||
      fcntl(listener, F_SETFL, status_flags | O_NONBLOCK) < 0) {
    error = std::string("prepare activated socket: ") + std::strerror(errno);
    return false;
  }
  unsetenv("LISTEN_PID");
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
  fd = listener;
  family = address.ss_family;
  return true;
}

// A dependency restart can start a socket-backed service without a client.
// Leave the listener with systemd before initializing any protocol resources.
inline bool CheckPendingConnection(int fd, bool &pending, std::string &error) {
  pending = false;
  pollfd listener{fd, POLLIN, 0};
  int result;
  do {
    result = poll(&listener, 1, 0);
  } while (result < 0 && errno == EINTR);
  if (result < 0 || (listener.revents & (POLLERR | POLLNVAL)) != 0) {
    error = "check activated listener readiness";
    return false;
  }
  pending = (listener.revents & POLLIN) != 0;
  return true;
}

class IdleTimeout {
public:
  using Clock = std::chrono::steady_clock;
  explicit IdleTimeout(Clock::time_point now = Clock::now()) : last_active_(now) {}
  bool Expired(bool active, Clock::time_point now = Clock::now()) {
    if (active)
      last_active_ = now;
    return !active && now - last_active_ >= std::chrono::seconds(30);
  }
private:
  Clock::time_point last_active_;
};

} // namespace onekvm::activation
