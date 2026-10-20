#include "socket_activation.hpp"

#include <arpa/inet.h>
#include <sys/wait.h>
#include <iostream>
#include <stdexcept>

using onekvm::activation::IdleTimeout;
using onekvm::activation::TakeListener;
void Require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void ChildTest(int family) {
  int listener = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  Require(listener >= 0, "create listener");
  if (family == AF_INET) {
    sockaddr_in address{};address.sin_family = family;address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind IPv4");
  } else {
    sockaddr_in6 address{};address.sin6_family = family;address.sin6_addr = in6addr_loopback;
    Require(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind IPv6");
  }
  Require(listen(listener, 8) == 0, "listen");
  if (listener != 3) { Require(dup2(listener, 3) == 3, "fd 3");close(listener); }
  int fd = -1, actual_family = 0;std::string error;
  unsetenv("LISTEN_PID");unsetenv("LISTEN_FDS");
  Require(TakeListener(fd, actual_family, error) && fd == -1, "standalone mode");
  setenv("LISTEN_PID", std::to_string(getpid()+1).c_str(), 1);setenv("LISTEN_FDS", "1", 1);
  Require(TakeListener(fd, actual_family, error) && fd == -1, "foreign PID ignored");
  setenv("LISTEN_PID", std::to_string(getpid()).c_str(), 1);setenv("LISTEN_FDS", "2", 1);
  Require(!TakeListener(fd, actual_family, error), "extra listeners rejected");
  setenv("LISTEN_FDS", "1", 1);
  Require(TakeListener(fd, actual_family, error) && fd == 3 && actual_family == family, "adopt listener");
  Require((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "listener not inherited by children");
  Require((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "nonblocking listener");
  Require(getenv("LISTEN_PID") == nullptr && getenv("LISTEN_FDS") == nullptr, "environment consumed");
  bool pending = true;
  Require(onekvm::activation::CheckPendingConnection(fd, pending, error) && !pending,
          "dependency restart without clients stays idle");
  sockaddr_storage bound{};socklen_t bound_size = sizeof(bound);
  Require(getsockname(fd, reinterpret_cast<sockaddr *>(&bound), &bound_size) == 0, "listener address");
  int client = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  Require(client >= 0 && connect(client, reinterpret_cast<sockaddr *>(&bound), bound_size) == 0, "queue client");
  Require(onekvm::activation::CheckPendingConnection(fd, pending, error) && pending,
          "queued connection starts protocol resources");
  close(client);
  close(fd);
  int pair[2];Require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "unix pair");
  if (pair[0] != 3) { Require(dup2(pair[0], 3) == 3, "non TCP fd 3");close(pair[0]); }
  setenv("LISTEN_PID", std::to_string(getpid()).c_str(), 1);setenv("LISTEN_FDS", "1", 1);
  Require(!TakeListener(fd, actual_family, error), "non TCP socket rejected");
}
int main() {
  try {
    const auto now = IdleTimeout::Clock::now();IdleTimeout timer(now);
    Require(!timer.Expired(false, now + std::chrono::seconds(29)), "startup grace");
    Require(!timer.Expired(true, now + std::chrono::seconds(31)), "connection keeps alive");
    Require(!timer.Expired(false, now + std::chrono::seconds(60)), "disconnect grace");
    Require(timer.Expired(false, now + std::chrono::seconds(61)), "idle exits");
    Require(!timer.Expired(true, now + std::chrono::seconds(62)), "reconnection cancels exit");
    for (int family : {AF_INET, AF_INET6}) {
      auto pid = fork();Require(pid >= 0, "fork");
      if (pid == 0) { try { ChildTest(family);_exit(0); } catch (const std::exception &e) {std::cerr << e.what() << '\n';_exit(1);} }
      int status;Require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "activation child test");
    }
    std::cout << "socket activation and idle lifetime passed\n";
  } catch (const std::exception &e) {std::cerr << e.what() << '\n';return 1;}
}
