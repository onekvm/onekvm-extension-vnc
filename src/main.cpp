#include "config.hpp"
#include "socket_activation.hpp"
#include "protocol_ipc.hpp"
#include "vnc_server.hpp"

#include <signal.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

volatile char exit_flag = 0;

void HandleSignal(int) { exit_flag = 1; }

struct Options {
  std::string config_path = onekvm::vnc::DefaultConfigPath();
  bool health = false;
  std::string lifecycle;
};

bool ParseOptions(int argc, char **argv, Options &options, std::string &error) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--health") {
      options.health = true;
    } else if (argument == "--config" || argument == "--lifecycle") {
      if (++index >= argc) {
        error = std::string(argument) + " requires a value";
        return false;
      }
      if (argument == "--config")
        options.config_path = argv[index];
      else
        options.lifecycle = argv[index];
    } else {
      error = "unexpected argument: " + std::string(argument);
      return false;
    }
  }
  if (!options.lifecycle.empty() && options.lifecycle != "enable" &&
      options.lifecycle != "disable") {
    error = "unsupported lifecycle action " + options.lifecycle;
    return false;
  }
  return true;
}

int Run(int argc, char **argv) {
  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, options, error)) {
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    return 2;
  }

  onekvm::vnc::Config config;
  if (!onekvm::vnc::LoadConfig(options.config_path, options.health, config,
                               error)) {
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    return 1;
  }
  if (options.health || !options.lifecycle.empty() || !config.service_enabled)
    return 0;

  int listen_fd = -1, listen_family = AF_UNSPEC;
  if (!onekvm::activation::TakeListener(listen_fd, listen_family, error)) {
    std::cerr << "socket activation: " << error << '\n';
    return 1;
  }
  if (listen_fd >= 0) {
    bool pending = false;
    if (!onekvm::activation::CheckPendingConnection(listen_fd, pending, error)) {
      std::cerr << "socket activation: " << error << '\n';
      return 1;
    }
    if (!pending)
      return 0;
  }

  onekvm::vnc::Identity identity;
  if (!onekvm::vnc::LoadIdentity(identity, error)) {
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    return 1;
  }
  onekvm::vnc::VideoInfo info;
  if (!onekvm::vnc::QueryVideoInfo(config.media_socket, identity, config.fps,
                                   config.jpeg_quality, info, error)) {
    std::cerr << "onekvm-protocol-vnc: query MJPEG video stream: " << error
              << '\n';
    return 1;
  }

  struct sigaction action{};
  action.sa_handler = HandleSignal;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) < 0 ||
      sigaction(SIGTERM, &action, nullptr) < 0) {
    std::cerr << "onekvm-protocol-vnc: install signal handler: "
              << std::strerror(errno) << '\n';
    return 1;
  }

  onekvm::vnc::VNCServer server(config, identity, info, error, listen_fd);
  if (!server.Ready()) {
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    return 1;
  }
  std::atomic<bool> audio_capability_changed{false};
  std::atomic<bool> capability_subscription_failed{false};
  onekvm::vnc::CapabilitySubscription capability_monitor(
      config.media_socket, identity, info.events_supported,
      [&](onekvm::vnc::VideoInfo current) {
        if (current.audio_available != info.audio_available) {
          audio_capability_changed.store(true);
          kill(getpid(), SIGTERM);
        }
      },
      [&](std::string reason) {
        capability_subscription_failed.store(true);
        std::cerr << "onekvm-protocol-vnc: " << reason << '\n';
        kill(getpid(), SIGTERM);
      });
  capability_monitor.Start();
  const bool ran = server.Run(exit_flag, error);
  capability_monitor.Stop();
  if (audio_capability_changed.load()) {
    std::cerr << "onekvm-protocol-vnc: USB audio capability changed; restarting\n";
    return 1;
  }
  if (capability_subscription_failed.load())
    return 1;
  if (!ran) {
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) { return Run(argc, argv); }
