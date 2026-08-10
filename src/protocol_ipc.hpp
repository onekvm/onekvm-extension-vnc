#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace onekvm::vnc {

enum class Codec : std::uint8_t { kH264 = 1, kH265 = 2, kMJPEG = 3 };

struct Identity {
  std::string extension_id;
  std::string token;
};

struct VideoInfo {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t duration_us = 0;
};

struct JPEGFrame {
  std::shared_ptr<void> owner;
  const std::uint8_t *data = nullptr;
  std::size_t size = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::uint32_t duration_us = 0;
};

bool LoadIdentity(Identity &identity, std::string &error);
bool QueryVideoInfo(const std::string &socket_path, const Identity &identity,
                    int fps, int jpeg_quality, VideoInfo &info,
                    std::string &error);

class SessionReporter {
public:
  SessionReporter(const std::string &socket_path, const Identity &identity,
                  std::string client_ip, std::string client_user,
                  std::string client_transport, std::string codec,
                  std::string &error);
  ~SessionReporter();
  SessionReporter(const SessionReporter &) = delete;
  SessionReporter &operator=(const SessionReporter &) = delete;

  bool Ready() const { return socket_fd_ >= 0; }

private:
  int socket_fd_ = -1;
};

class MediaSubscription {
public:
  using FrameCallback = std::function<void(JPEGFrame)>;
  using NotifyCallback = std::function<void()>;
  using FatalCallback = std::function<void(std::string)>;

  MediaSubscription(std::string socket_path, Identity identity, int fps,
                    int jpeg_quality,
                    FrameCallback frame_callback,
                    NotifyCallback notify_callback,
                    FatalCallback fatal_callback);
  ~MediaSubscription();
  MediaSubscription(const MediaSubscription &) = delete;
  MediaSubscription &operator=(const MediaSubscription &) = delete;

  void Start();
  void Stop();

private:
  void Run();

  std::string socket_path_;
  Identity identity_;
  int fps_;
  int jpeg_quality_;
  FrameCallback frame_callback_;
  NotifyCallback notify_callback_;
  FatalCallback fatal_callback_;
  std::atomic<bool> stopping_{false};
  std::atomic<int> socket_fd_{-1};
  std::thread thread_;
};

} // namespace onekvm::vnc
