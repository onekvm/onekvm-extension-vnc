#include "protocol_ipc.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static void Require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}

static void CheckSubscription(bool supports_events) {
  const std::string path =
      "/tmp/onekvm-capability-test-" + std::to_string(getpid());
  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  Require(bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0,
          "bind");
  Require(listen(listener, 1) == 0, "listen");
  std::atomic<int> events{0};
  std::atomic<bool> failed{false};
  std::atomic<bool> disconnected{false};
  std::thread server([&] {
    // First probe negotiates support; an old Core only sees "info" requests.
    for (int connection = 0; connection < (supports_events ? 2 : 3);
         ++connection) {
      pollfd wait{listener, POLLIN, 0};
      if (poll(&wait, 1, 5000) != 1) {
        failed.store(true);
        return;
      }
      const int peer = accept(listener, nullptr, nullptr);
      const timeval timeout{5, 0};
      setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      std::string request;
      char byte;
      while (recv(peer, &byte, 1, 0) == 1 && byte != '\n')
        request += byte;
      const bool subscribed = supports_events && connection > 0;
      if ((request.find("info-events") != std::string::npos) != subscribed)
        failed.store(true);
      std::array<unsigned char, 40> frame{};
      std::memcpy(frame.data(), "OKVF", 4);
      frame[4] = 1;
      frame[5] = 3;
      frame[7] = 0x81 | (supports_events ? 4 : 0);
      frame[26] = 0x41;
      frame[27] = 0x1a;
      frame[29] = 64;
      frame[31] = 64;
      if (connection == 2)
        frame[7] &= ~1;
      if (send(peer, frame.data(), frame.size(), MSG_NOSIGNAL) != 40)
        failed.store(true);
      if (subscribed) {
        frame[7] &= ~1;
        if (send(peer, frame.data(), frame.size(), MSG_NOSIGNAL) != 40)
          failed.store(true);
        disconnected.store(recv(peer, &byte, 1, 0) == 0);
      }
      close(peer);
    }
  });
  onekvm::vnc::VideoInfo info;
  std::string error;
  const bool probed =
      onekvm::vnc::QueryVideoInfo(path, {"test", "test"}, 60, 80, info, error);
  onekvm::vnc::CapabilitySubscription subscription(
      path, {"test", "test"}, info.events_supported,
      [&](onekvm::vnc::VideoInfo current) {
        const int index = events.fetch_add(1);
        if (current.audio_available != (index == 0))
          failed.store(true);
      },
      [&](std::string) { failed.store(true); });
  subscription.Start();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (events.load() < 2 && !failed.load() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  const auto stopping = std::chrono::steady_clock::now();
  subscription.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - stopping;
  server.join();
  close(listener);
  unlink(path.c_str());
  Require(probed && info.events_supported == supports_events,
          "metadata negotiation");
  Require(events.load() == 2 && !failed.load(),
          "initial/change capability events");
  Require(!supports_events || disconnected.load(),
          "subscription retained socket after Stop");
  Require(elapsed < std::chrono::milliseconds(200),
          "idle subscription Stop did not wake promptly");
}

int main() {
  try {
    CheckSubscription(true);
    CheckSubscription(false);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
