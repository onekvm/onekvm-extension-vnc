#pragma once

#include "admin_hid.hpp"
#include "config.hpp"
#include "protocol_ipc.hpp"

#include <rfb/rfb.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace onekvm::vnc {

class InputState {
public:
  explicit InputState(AdminHID &hid);
  ~InputState();
  InputState(const InputState &) = delete;
  InputState &operator=(const InputState &) = delete;

  void Key(bool down, rfbKeySym key);
  void Pointer(int button_mask, int x, int y);
  void Resize(std::uint16_t width, std::uint16_t height);
  void Release();
  std::pair<std::uint64_t, std::uint64_t> TakePointerStats();

private:
  struct PointerReport {
    std::uint16_t buttons;
    std::uint16_t x;
    std::uint16_t y;
    int wheel;
    std::uint64_t generation;
  };

  void SendKeyboard();
  void FlushPointer(bool wait);
  void PointerLoop();
  void SendPointer(std::uint16_t buttons, std::uint16_t x, std::uint16_t y,
                   int wheel);

  AdminHID &hid_;
  std::uint16_t width_ = 1;
  std::uint16_t height_ = 1;
  std::uint8_t modifiers_ = 0;
  std::unordered_map<rfbKeySym, std::uint8_t> keys_;
  std::mutex pointer_mutex_;
  std::condition_variable pointer_changed_;
  std::condition_variable pointer_flushed_;
  std::thread pointer_thread_;
  bool pointer_stopping_ = false;
  bool pointer_pending_ = false;
  std::uint16_t pointer_buttons_ = 0;
  std::uint16_t pointer_x_ = 0;
  std::uint16_t pointer_y_ = 0;
  std::deque<PointerReport> pointer_urgent_;
  std::uint64_t pointer_generation_ = 0;
  std::uint64_t pointer_completed_generation_ = 0;
  std::atomic<std::uint64_t> pointer_events_{0};
  std::atomic<std::uint64_t> pointer_reports_{0};
};

class VNCServer {
public:
  VNCServer(const Config &config, Identity identity, const VideoInfo &info,
            std::string &error);
  ~VNCServer();
  VNCServer(const VNCServer &) = delete;
  VNCServer &operator=(const VNCServer &) = delete;

  bool Ready() const { return server_ != nullptr; }
  bool Run(volatile char &exit_flag, std::string &error);

private:
  struct ClientData {
    bool need_update = false;
    bool force_update = false;
    bool continuous_supported = false;
    bool continuous_enabled = false;
    bool audio_supported = false;
    bool audio_capability_pending = false;
    bool audio_enabled = false;
    bool audio_started = false;
    std::uint8_t audio_sample_format = 3;
    std::uint8_t audio_channels = 2;
    std::uint32_t audio_frequency = 44100;
    std::uint64_t audio_resample_position = 0;
    bool logged_encoding = false;
    std::uint64_t last_sequence = 0;
    std::unique_ptr<SessionReporter> session_reporter;
  };

  static enum rfbNewClientAction NewClient(rfbClientPtr client);
  static void ClientGone(rfbClientPtr client);
  static void
  FramebufferUpdateRequested(rfbClientPtr client,
                             rfbFramebufferUpdateRequestMsg *request);
  static void KeyEvent(rfbBool down, rfbKeySym key, rfbClientPtr client);
  static void PointerEvent(int button_mask, int x, int y, rfbClientPtr client);
  static void DisplayHook(rfbClientPtr client);
  static rfbBool ContinuousNewClient(rfbClientPtr client, void **data);
  static rfbBool ContinuousEnablePseudoEncoding(rfbClientPtr client,
                                                void **data,
                                                int encoding_number);
  static rfbBool NativeJPEGNewClient(rfbClientPtr client, void **data);
  static rfbBool NativeJPEGEnableEncoding(rfbClientPtr client, void **data,
                                          int encoding_number);
  static rfbBool AudioNewClient(rfbClientPtr client, void **data);
  static rfbBool AudioEnableEncoding(rfbClientPtr client, void **data,
                                     int encoding_number);
  static rfbBool AudioHandleMessage(rfbClientPtr client, void *data,
                                    const rfbClientToServerMsg *message);
  static rfbBool ContinuousHandleMessage(
      rfbClientPtr client, void *data,
      const rfbClientToServerMsg *message);

  void SetFrame(JPEGFrame frame);
  void QueueAudio(std::vector<std::int16_t> pcm);
  void SetFatal(std::string error);
  void SyncSubscription();
  void SyncAudioSubscription();
  bool ProcessFrame(std::string &error);
  bool ProcessAudio(std::string &error);
  bool SendAudioCapability(rfbClientPtr client, std::string &error);
  bool SendAudioControl(rfbClientPtr client, std::uint16_t operation,
                        std::string &error);
  bool SendAudioData(rfbClientPtr client, const std::vector<std::uint8_t> &pcm,
                     std::string &error);
  static std::vector<std::uint8_t>
  ConvertAudio(const std::vector<std::int16_t> &source, ClientData &client);
  bool Resize(std::uint16_t width, std::uint16_t height, std::string &error);
  bool SendJPEG(rfbClientPtr client, const JPEGFrame &frame,
                std::string &error);
  bool SendNativeJPEG(rfbClientPtr client, const JPEGFrame &frame,
                      std::string &error);
  bool SendFramebuffer(rfbClientPtr client, bool force_update,
                       std::string &error);
  bool ClientWantsNativeJPEG(rfbClientPtr client) const;
  bool ClientWantsTightJPEG(rfbClientPtr client) const;
  bool DecodeLatestFrame(const JPEGFrame &frame, std::string &error);
  void SuppressDirectJPEGUpdates();
  void ClearLatestFrame();

  Config config_;
  Identity identity_;
  AdminHID hid_;
  InputState input_;
  rfbScreenInfoPtr server_ = nullptr;
  std::string desktop_name_;
  std::vector<char> framebuffer_;
  std::vector<std::uint16_t> previous_rgb_;
  std::uint64_t decoded_sequence_ = 0;
  std::array<char *, 2> password_list_{};
  std::string listen6_interface_;
  std::unique_ptr<MediaSubscription> subscription_;
  std::unique_ptr<AudioSubscription> audio_subscription_;
  unsigned int clients_ = 0;
  std::uint16_t width_ = 0;
  std::uint16_t height_ = 0;
  std::mutex frame_mutex_;
  JPEGFrame latest_frame_;
  std::uint64_t latest_sequence_ = 0;
  std::chrono::steady_clock::time_point last_jpeg_sent_{};
  std::chrono::steady_clock::time_point last_framebuffer_sent_{};
  std::mutex audio_mutex_;
  std::deque<std::vector<std::int16_t>> audio_queue_;
  std::mutex fatal_mutex_;
  std::atomic<bool> fatal_{false};
  std::string fatal_error_;
  bool protocol_extensions_registered_ = false;
  static int native_jpeg_encodings_[2];
  static rfbProtocolExtension native_jpeg_extension_;
  static int audio_encodings_[2];
  static rfbProtocolExtension audio_extension_;
  static int continuous_encodings_[2];
  static rfbProtocolExtension continuous_extension_;
};

} // namespace onekvm::vnc
