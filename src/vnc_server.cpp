#include "vnc_server.hpp"

#include "jpeg_rgb.hpp"
#include "keymap.hpp"

#include <arpa/inet.h>
#include <rfb/rfbproto.h>
extern "C" {
#include <rfb/rfbregion.h>
}

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace onekvm::vnc {
namespace {

constexpr std::size_t kBytesPerFramebufferPixel = 2;
constexpr std::size_t kMaxFramebufferBytes = 64U << 20;
// VNC's JPEG path is considerably more CPU-heavy than RustDesk's encoded
// passthrough.  Cap pure pointer motion at the configured maximum video
// cadence instead of spending half of a single-core NanoKVM on 60 HID API
// calls per second.  Button transitions and wheel events still flush
// immediately below.
constexpr auto kPointerFlushInterval = std::chrono::milliseconds(33);
// The current RFB protocol registry assigns encoding 21 to a complete JPEG
// image. The LibVNCServer revision used by the appliance predates that
// assignment, so an application extension claims it and forwards MMF's JPEG
// verbatim.
constexpr int kNativeJPEGEncoding = 21;
constexpr int kQEMUAudioEncoding = -259;
constexpr std::uint8_t kQEMUMessage = 255;
constexpr std::uint8_t kQEMUAudioSubmessage = 1;
constexpr std::uint16_t kQEMUAudioEnable = 0;
constexpr std::uint16_t kQEMUAudioDisable = 1;
constexpr std::uint16_t kQEMUAudioSetFormat = 2;
constexpr std::uint16_t kQEMUAudioEnd = 0;
constexpr std::uint16_t kQEMUAudioBegin = 1;
constexpr std::uint16_t kQEMUAudioData = 2;
constexpr std::uint32_t kOpusSampleRate = 48000;
constexpr std::size_t kMaxQueuedAudioFrames = 12;
// At most 500ms of stereo PCM may wait behind a JPEG. Disconnect on overload
// instead of silently dropping samples from an otherwise live audio stream.
constexpr std::size_t kMaxDeferredAudioSamples = kOpusSampleRate;
constexpr int kContinuousUpdatesEncoding = -313;
constexpr std::uint8_t kEnableContinuousUpdatesMessage = 150;

constexpr std::uint16_t RFBButtonsToHID(int button_mask) {
  // RFB orders the first three buttons as left, middle, right. USB HID uses
  // left, right, middle, so the middle and right bits must be exchanged.
  std::uint16_t buttons = static_cast<std::uint16_t>(button_mask & 0x01);
  if ((button_mask & 0x02) != 0)
    buttons |= 1U << 2U;
  if ((button_mask & 0x04) != 0)
    buttons |= 1U << 1U;
  if ((button_mask & 0x20) != 0)
    buttons |= 1U << 3U;
  if ((button_mask & 0x40) != 0)
    buttons |= 1U << 4U;
  return buttons;
}

static_assert(RFBButtonsToHID(0x01) == 0x01);
static_assert(RFBButtonsToHID(0x02) == 0x04);
static_assert(RFBButtonsToHID(0x04) == 0x02);
static_assert(RFBButtonsToHID(0x07) == 0x07);

std::string DesktopName() {
  std::array<char, 256> hostname{};
  if (gethostname(hostname.data(), hostname.size() - 1) != 0 ||
      hostname.front() == '\0')
    return "OneKVM";
  hostname.back() = '\0';
  return std::string(hostname.data()) + " - OneKVM";
}

void ClearClientRegions(rfbClientPtr client) {
  LOCK(client->updateMutex);
  sraRgnMakeEmpty(client->requestedRegion);
  sraRgnMakeEmpty(client->modifiedRegion);
  sraRgnMakeEmpty(client->copyRegion);
  client->startDeferring.tv_usec = 0;
  UNLOCK(client->updateMutex);
}

std::uint16_t ReadBE16(const std::uint8_t *data) {
  return static_cast<std::uint16_t>((data[0] << 8U) | data[1]);
}

std::uint32_t ReadBE32(const std::uint8_t *data) {
  return (static_cast<std::uint32_t>(data[0]) << 24U) |
         (static_cast<std::uint32_t>(data[1]) << 16U) |
         (static_cast<std::uint32_t>(data[2]) << 8U) | data[3];
}

void AppendLE16(std::vector<std::uint8_t> &output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void AppendLE32(std::vector<std::uint8_t> &output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void AppendAudioSample(std::vector<std::uint8_t> &output,
                       std::int16_t sample, std::uint8_t format) {
  switch (format) {
  case 0:
    output.push_back(static_cast<std::uint8_t>(
        (static_cast<std::int32_t>(sample) + 32768) >> 8U));
    break;
  case 1:
    output.push_back(static_cast<std::uint8_t>(
        static_cast<std::int8_t>(sample >> 8U)));
    break;
  case 2:
    AppendLE16(output, static_cast<std::uint16_t>(
                           static_cast<std::int32_t>(sample) + 32768));
    break;
  case 3:
    AppendLE16(output, static_cast<std::uint16_t>(sample));
    break;
  case 4: {
    const auto value = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(sample) + 32768);
    AppendLE32(output, (value << 16U) | value);
    break;
  }
  case 5:
    AppendLE32(output, static_cast<std::uint32_t>(
                           static_cast<std::int32_t>(sample) * 65536));
    break;
  default:
    break;
  }
}

// LibVNCServer owns the client list iterator. Closing a client while that
// iterator still holds its reference can leave the non-threaded event loop
// spinning on a closed client. Defer removal until the iterator is released,
// then complete the non-threaded close path immediately. Waiting for a later
// rfbProcessEvents call proved unreliable after a failed Tight write because
// the closed descriptor could keep the event loop busy indefinitely.
void CloseClients(const std::vector<rfbClientPtr> &clients) {
  for (auto *client : clients) {
    if (client != nullptr && client->sock != RFB_INVALID_SOCKET)
      rfbCloseClient(client);
    if (client != nullptr)
      rfbClientConnectionGone(client);
  }
}

void ApplyRGB565ServerFormat(rfbScreenInfoPtr server, bool refresh_clients) {
  if (server == nullptr)
    return;
  /* rfbGetScreen(5,3,2) builds RGB555 with R in the low bits. The JPEG
     fallback packs standard RGB565 (R[15:11] G[10:5] B[4:0]). */
  auto &format = server->serverFormat;
  format.bitsPerPixel = 16;
  format.depth = 16;
  format.trueColour = TRUE;
  format.redMax = 31;
  format.greenMax = 63;
  format.blueMax = 31;
  format.redShift = 11;
  format.greenShift = 5;
  format.blueShift = 0;
  if (!refresh_clients)
    return;
  auto iterator = rfbGetClientIterator(server);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr)
    server->setTranslateFunction(client);
  rfbReleaseClientIterator(iterator);
}

bool AllocateFramebuffer(std::uint16_t width, std::uint16_t height,
                         std::vector<char> &framebuffer, std::string &error) {
  const auto size =
      static_cast<std::size_t>(width) * height * kBytesPerFramebufferPixel;
  if (width == 0 || height == 0 || size > kMaxFramebufferBytes) {
    error = "VNC framebuffer dimensions are invalid or too large";
    return false;
  }
  try {
    framebuffer.assign(size, 0);
  } catch (const std::bad_alloc &) {
    error = "allocate VNC framebuffer";
    return false;
  }
  return true;
}

} // namespace

InputState::InputState(AdminHID &hid)
    : hid_(hid), pointer_thread_([this] { PointerLoop(); }) {}

InputState::~InputState() {
  Release();
  {
    std::lock_guard lock(pointer_mutex_);
    pointer_stopping_ = true;
  }
  pointer_changed_.notify_one();
  if (pointer_thread_.joinable())
    pointer_thread_.join();
}

void InputState::SendKeyboard() {
  std::vector<std::uint8_t> keys;
  keys.reserve(keys_.size());
  for (const auto &[key, usage] : keys_) {
    (void)key;
    keys.push_back(usage);
  }
  std::sort(keys.begin(), keys.end());
  if (keys.size() > 6)
    keys.resize(6);
  std::string error;
  if (!hid_.SendKeyboard(modifiers_, keys, error))
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
}

void InputState::Key(bool down, rfbKeySym key) {
  // Preserve input ordering: a key or modifier must not overtake the latest
  // coalesced pointer position or button transition.
  FlushPointer(true);
  const auto modifier = ikvm::keyToMod(key);
  if (modifier != 0) {
    if (down)
      modifiers_ |= modifier;
    else
      modifiers_ &= static_cast<std::uint8_t>(~modifier);
    SendKeyboard();
    return;
  }
  const auto usage = ikvm::keyToScancode(key);
  if (usage == 0)
    return;
  if (down)
    keys_[key] = usage;
  else
    keys_.erase(key);
  SendKeyboard();
}

void InputState::Pointer(int button_mask, int x, int y) {
  pointer_events_.fetch_add(1, std::memory_order_relaxed);
  const std::uint16_t buttons = RFBButtonsToHID(button_mask);
  const auto clamped_x = std::clamp(x, 0, static_cast<int>(width_ - 1));
  const auto clamped_y = std::clamp(y, 0, static_cast<int>(height_ - 1));
  const auto absolute_x =
      width_ > 1
          ? static_cast<std::uint16_t>(static_cast<std::uint32_t>(clamped_x) *
                                       32767U / (width_ - 1U))
          : 0;
  const auto absolute_y =
      height_ > 1
          ? static_cast<std::uint16_t>(static_cast<std::uint32_t>(clamped_y) *
                                       32767U / (height_ - 1U))
          : 0;
  std::int8_t wheel = 0;
  if ((button_mask & 0x08) != 0)
    wheel = 1;
  else if ((button_mask & 0x10) != 0)
    wheel = -1;
  {
    std::lock_guard lock(pointer_mutex_);
    const bool button_changed = buttons != pointer_buttons_;
    pointer_buttons_ = buttons;
    pointer_x_ = absolute_x;
    pointer_y_ = absolute_y;
    ++pointer_generation_;
    if (button_changed || wheel != 0) {
      // The urgent report contains the newest coordinates, so it supersedes
      // any pending pure movement before it.  Unlike movement, every button
      // transition and wheel tick must remain ordered and cannot share a
      // single coalescing slot.
      pointer_pending_ = false;
      pointer_urgent_.push_back(
          {buttons, absolute_x, absolute_y, wheel, pointer_generation_});
    } else {
      pointer_pending_ = true;
    }
  }
  pointer_changed_.notify_one();
}

void InputState::SendPointer(std::uint16_t buttons, std::uint16_t x,
                             std::uint16_t y, int wheel) {
  std::string error;
  pointer_reports_.fetch_add(1, std::memory_order_relaxed);
  if (!hid_.SendAbsoluteMouse(buttons, x, y, error))
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
  while (wheel != 0) {
    const auto step = static_cast<std::int8_t>(std::clamp(wheel, -127, 127));
    pointer_reports_.fetch_add(1, std::memory_order_relaxed);
    if (!hid_.SendMouse(static_cast<std::uint8_t>(buttons), step, error))
      std::cerr << "onekvm-protocol-vnc: " << error << '\n';
    wheel -= step;
  }
}

void InputState::PointerLoop() {
  auto next_flush =
      std::chrono::steady_clock::now() + kPointerFlushInterval;
  std::unique_lock lock(pointer_mutex_);
  while (!pointer_stopping_) {
    if (pointer_urgent_.empty() && !pointer_pending_) {
      pointer_changed_.wait(lock, [this] {
        return pointer_stopping_ || !pointer_urgent_.empty() ||
               pointer_pending_;
      });
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (pointer_urgent_.empty() && now < next_flush) {
      pointer_changed_.wait_until(lock, next_flush, [this] {
        return pointer_stopping_ || !pointer_urgent_.empty();
      });
      continue;
    }
    PointerReport report{};
    if (!pointer_urgent_.empty()) {
      report = pointer_urgent_.front();
      pointer_urgent_.pop_front();
    } else {
      report = {pointer_buttons_, pointer_x_, pointer_y_, 0,
                pointer_generation_};
      pointer_pending_ = false;
      // Keep a fixed cadence like RustDesk's interval timer.  Resetting the
      // deadline whenever the queue was briefly empty allowed bursty RFB
      // input to bypass the limiter.  Skip missed ticks instead of sending a
      // burst.
      do {
        next_flush += kPointerFlushInterval;
      } while (next_flush <= now);
    }
    lock.unlock();
    SendPointer(report.buttons, report.x, report.y, report.wheel);
    lock.lock();
    pointer_completed_generation_ =
        std::max(pointer_completed_generation_, report.generation);
    pointer_flushed_.notify_all();
  }
}

void InputState::FlushPointer(bool wait) {
  std::unique_lock lock(pointer_mutex_);
  const auto generation = pointer_generation_;
  if (pointer_pending_ || !pointer_urgent_.empty()) {
    pointer_changed_.notify_one();
  }
  if (wait && generation > pointer_completed_generation_) {
    pointer_flushed_.wait(lock, [this, generation] {
      return pointer_stopping_ ||
             pointer_completed_generation_ >= generation;
    });
  }
}

void InputState::Resize(std::uint16_t width, std::uint16_t height) {
  width_ = std::max<std::uint16_t>(width, 1);
  height_ = std::max<std::uint16_t>(height, 1);
}

void InputState::Release() {
  FlushPointer(true);
  bool release_pointer = false;
  {
    std::lock_guard lock(pointer_mutex_);
    if (pointer_buttons_ != 0) {
      pointer_buttons_ = 0;
      ++pointer_generation_;
      pointer_pending_ = false;
      pointer_urgent_.push_back(
          {0, pointer_x_, pointer_y_, 0, pointer_generation_});
      release_pointer = true;
    }
  }
  if (release_pointer) {
    pointer_changed_.notify_one();
    FlushPointer(true);
  }
  std::string error;
  if ((modifiers_ != 0 || !keys_.empty()) && !hid_.SendKeyboard(0, {}, error))
    std::cerr << "onekvm-protocol-vnc: " << error << '\n';
  modifiers_ = 0;
  keys_.clear();
}

std::pair<std::uint64_t, std::uint64_t> InputState::TakePointerStats() {
  return {pointer_events_.exchange(0, std::memory_order_relaxed),
          pointer_reports_.exchange(0, std::memory_order_relaxed)};
}

int VNCServer::continuous_encodings_[2] = {kContinuousUpdatesEncoding, 0};

int VNCServer::native_jpeg_encodings_[2] = {kNativeJPEGEncoding, 0};

rfbProtocolExtension VNCServer::native_jpeg_extension_ = {
    NativeJPEGNewClient,
    nullptr,
    native_jpeg_encodings_,
    NativeJPEGEnableEncoding,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

int VNCServer::audio_encodings_[2] = {kQEMUAudioEncoding, 0};

rfbProtocolExtension VNCServer::audio_extension_ = {
    AudioNewClient,
    nullptr,
    audio_encodings_,
    AudioEnableEncoding,
    AudioHandleMessage,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

rfbProtocolExtension VNCServer::continuous_extension_ = {
    ContinuousNewClient,
    nullptr,
    continuous_encodings_,
    ContinuousEnablePseudoEncoding,
    ContinuousHandleMessage,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

VNCServer::VNCServer(const Config &config, Identity identity,
                     const VideoInfo &info, std::string &error)
    : config_(config), identity_(std::move(identity)),
      hid_(config.admin_socket, identity_), input_(hid_), width_(info.width),
      height_(info.height) {
  if (!AllocateFramebuffer(width_, height_, framebuffer_, error))
    return;
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wake_fd_ < 0) {
    error = "create VNC wake event";
    return;
  }

  int argc = 1;
  char program[] = "onekvm-protocol-vnc";
  char *argv[] = {program, nullptr};
  server_ = rfbGetScreen(&argc, argv, width_, height_, 5, 3,
                         kBytesPerFramebufferPixel);
  if (server_ == nullptr) {
    error = "initialize LibVNCServer screen";
    return;
  }
  server_->screenData = this;
  desktop_name_ = DesktopName();
  server_->desktopName = desktop_name_.c_str();
  server_->frameBuffer = framebuffer_.data();
  ApplyRGB565ServerFormat(server_, false);
  server_->alwaysShared = TRUE;
  server_->permitFileTransfer = FALSE;
  server_->newClientHook = NewClient;
  server_->kbdAddEvent = KeyEvent;
  server_->ptrAddEvent = PointerEvent;
  // InputState already coalesces pointer motion; we do not use the library's
  // deferred pointer/update scheduler.
  server_->deferPtrUpdateTime = 0;
  server_->displayHook = DisplayHook;
  server_->port = config_.port;
  server_->ipv6port = -1;

  in_addr address{};
  if (inet_pton(AF_INET, config_.listen_address.c_str(), &address) == 1) {
    server_->listenInterface = address.s_addr;
  } else {
    in6_addr address6{};
    if (inet_pton(AF_INET6, config_.listen_address.c_str(), &address6) != 1) {
      error = "VNC listen address must be an IPv4 or IPv6 address";
      rfbScreenCleanup(server_);
      server_ = nullptr;
      return;
    }
    server_->port = -1;
    server_->ipv6port = config_.port;
    listen6_interface_ = config_.listen_address;
    server_->listen6Interface = listen6_interface_.data();
  }

  if (config_.access_control_enabled) {
    password_list_[0] = config_.password.data();
    password_list_[1] = nullptr;
    server_->authPasswdData = password_list_.data();
    server_->passwordCheck = rfbCheckPasswordByList;
  } else {
    server_->authPasswdData = nullptr;
    server_->passwordCheck = nullptr;
  }

  input_.Resize(width_, height_);
  rfbRegisterProtocolExtension(&native_jpeg_extension_);
  rfbRegisterProtocolExtension(&audio_extension_);
  rfbRegisterProtocolExtension(&continuous_extension_);
  protocol_extensions_registered_ = true;
  rfbInitServer(server_);
  const bool ipv4_ready =
      server_->port < 0 || server_->listenSock != RFB_INVALID_SOCKET;
  const bool ipv6_ready =
      server_->ipv6port < 0 || server_->listen6Sock != RFB_INVALID_SOCKET;
  if (!ipv4_ready || !ipv6_ready) {
    error = "listen on VNC address " + config_.listen_address + ":" +
            std::to_string(config_.port);
    rfbShutdownServer(server_, TRUE);
    rfbScreenCleanup(server_);
    server_ = nullptr;
    rfbUnregisterProtocolExtension(&continuous_extension_);
    rfbUnregisterProtocolExtension(&audio_extension_);
    rfbUnregisterProtocolExtension(&native_jpeg_extension_);
    protocol_extensions_registered_ = false;
  }
}

VNCServer::~VNCServer() {
  if (subscription_ != nullptr)
    subscription_->Stop();
  if (audio_subscription_ != nullptr)
    audio_subscription_->Stop();
  if (server_ != nullptr) {
    rfbShutdownServer(server_, TRUE);
    rfbScreenCleanup(server_);
  }
  if (protocol_extensions_registered_) {
    rfbUnregisterProtocolExtension(&continuous_extension_);
    rfbUnregisterProtocolExtension(&audio_extension_);
    rfbUnregisterProtocolExtension(&native_jpeg_extension_);
  }
  if (wake_fd_ >= 0)
    close(wake_fd_);
}

enum rfbNewClientAction VNCServer::NewClient(rfbClientPtr client) {
  auto *server = static_cast<VNCServer *>(client->screen->screenData);
  client->clientData = new ClientData{};
  client->clientGoneHook = ClientGone;
  client->clientFramebufferUpdateRequestHook = FramebufferUpdateRequested;
  ++server->clients_;
  std::cerr << "onekvm-protocol-vnc: client connected from " << client->host
            << '\n';
  return RFB_CLIENT_ACCEPT;
}

void VNCServer::ClientGone(rfbClientPtr client) {
  auto *server = static_cast<VNCServer *>(client->screen->screenData);
  // LibVNCServer's bundled revision does not free extension registration nodes
  // on client destruction. Our three registrations have no separate payload.
  auto **link = &client->extensions;
  while (*link != nullptr) {
    auto *node = *link;
    if (node->extension == &native_jpeg_extension_ ||
        node->extension == &audio_extension_ ||
        node->extension == &continuous_extension_) {
      *link = node->next;
      std::free(node);
    } else {
      link = &node->next;
    }
  }
  delete static_cast<ClientData *>(client->clientData);
  client->clientData = nullptr;
  if (server->clients_ > 0)
    --server->clients_;
  if (server->clients_ == 0) {
    server->input_.Release();
    const auto [events, reports] = server->input_.TakePointerStats();
    if (events != 0)
      std::cerr << "onekvm-protocol-vnc: coalesced " << events
                << " pointer events into " << reports << " HID reports\n";
  }
  std::cerr << "onekvm-protocol-vnc: client disconnected\n";
}

void VNCServer::FramebufferUpdateRequested(
    rfbClientPtr client, rfbFramebufferUpdateRequestMsg *request) {
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data == nullptr)
    return;
  if (data->session_reporter == nullptr) {
    auto *server = static_cast<VNCServer *>(client->screen->screenData);
    std::string error;
    auto reporter = std::make_unique<SessionReporter>(
        server->config_.media_socket, server->identity_,
        client->host == nullptr ? "" : client->host, "", "tcp", "mjpeg",
        error);
    if (reporter->Ready()) {
      data->session_reporter = std::move(reporter);
    } else {
      std::cerr << "onekvm-protocol-vnc: report client session: " << error
                << '\n';
    }
  }
  data->need_update = true;
  data->force_update |= request->incremental == 0;
}

void VNCServer::KeyEvent(rfbBool down, rfbKeySym key, rfbClientPtr client) {
  auto *server = static_cast<VNCServer *>(client->screen->screenData);
  server->input_.Key(down != FALSE, key);
}

void VNCServer::PointerEvent(int button_mask, int x, int y,
                             rfbClientPtr client) {
  auto *server = static_cast<VNCServer *>(client->screen->screenData);
  server->input_.Pointer(button_mask, x, y);
}

void VNCServer::DisplayHook(rfbClientPtr client) {
  if (client == nullptr)
    return;
  /* LibVNCServer otherwise replies with Raw/ZRLE/Ultra/Hextile/etc. This
     process only originates Tight JPEG or a decoded RGB fallback. */
  client->enableSupportedEncodings = FALSE;
  client->enableSupportedMessages = FALSE;
  client->enableServerIdentity = FALSE;
}

rfbBool VNCServer::ContinuousNewClient(rfbClientPtr, void **data) {
  *data = nullptr;
  return TRUE;
}

rfbBool VNCServer::NativeJPEGNewClient(rfbClientPtr, void **data) {
  *data = nullptr;
  return TRUE;
}

rfbBool VNCServer::NativeJPEGEnableEncoding(rfbClientPtr client, void **,
                                            int encoding_number) {
  if (encoding_number != kNativeJPEGEncoding)
    return FALSE;
  // The older LibVNCServer processes application encodings after its built-in
  // choices and can otherwise leave ZRLE selected even when the viewer puts
  // JPEG first. Seeing encoding 21 is an explicit opt-in, so make it win over
  // software framebuffer encodings regardless of that implementation detail.
  client->preferredEncoding = kNativeJPEGEncoding;
  return TRUE;
}

rfbBool VNCServer::AudioNewClient(rfbClientPtr, void **data) {
  *data = nullptr;
  return TRUE;
}

rfbBool VNCServer::AudioEnableEncoding(rfbClientPtr client, void **,
                                       int encoding_number) {
  if (encoding_number != kQEMUAudioEncoding)
    return FALSE;
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data == nullptr)
    return FALSE;
  data->audio_supported = true;
  data->audio_capability_pending = true;
  rfbLog("Enabling QEMU audio protocol extension for client %s\n",
         client->host);
  return TRUE;
}

rfbBool VNCServer::AudioHandleMessage(
    rfbClientPtr client, void *, const rfbClientToServerMsg *message) {
  if (message->type != kQEMUMessage)
    return FALSE;
  std::uint8_t subtype = 0;
  const auto peeked = recv(client->sock, &subtype, 1, MSG_PEEK);
  if (peeked <= 0) {
    rfbCloseClient(client);
    return TRUE;
  }
  // Message type 255 also carries the QEMU extended-key submessage. Leave it
  // untouched for another protocol extension when it is not audio.
  if (subtype != kQEMUAudioSubmessage)
    return FALSE;

  std::array<std::uint8_t, 3> header{};
  if (rfbReadExact(client, reinterpret_cast<char *>(header.data()),
                   static_cast<int>(header.size())) <= 0) {
    rfbCloseClient(client);
    return TRUE;
  }
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data == nullptr || !data->audio_supported) {
    rfbCloseClient(client);
    return TRUE;
  }
  const auto operation = ReadBE16(header.data() + 1);
  auto *server = static_cast<VNCServer *>(client->screen->screenData);
  if (operation == kQEMUAudioEnable) {
    data->audio_enabled = true;
    data->audio_resample_position = 0;
    rfbLog("QEMU audio enabled for client %s\n", client->host);
    return TRUE;
  }
  if (operation == kQEMUAudioDisable) {
    if (data->audio_started) {
      std::string error;
      if (!server->SendAudioControl(client, kQEMUAudioEnd, error)) {
        std::cerr << "onekvm-protocol-vnc: " << error << '\n';
        rfbCloseClient(client);
      }
    }
    data->audio_enabled = false;
    data->audio_started = false;
    data->deferred_audio.clear();
    data->deferred_audio_samples = 0;
    data->audio_resample_position = 0;
    rfbLog("QEMU audio disabled for client %s\n", client->host);
    return TRUE;
  }
  if (operation == kQEMUAudioSetFormat) {
    std::array<std::uint8_t, 6> format{};
    if (rfbReadExact(client, reinterpret_cast<char *>(format.data()),
                     static_cast<int>(format.size())) <= 0) {
      rfbCloseClient(client);
      return TRUE;
    }
    const auto sample_format = format[0];
    const auto channels = format[1];
    const auto frequency = ReadBE32(format.data() + 2);
    if (sample_format > 5 || (channels != 1 && channels != 2) ||
        frequency == 0 || frequency > kOpusSampleRate) {
      rfbLog("Invalid QEMU audio format from client %s\n", client->host);
      rfbCloseClient(client);
      return TRUE;
    }
    if (data->audio_started) {
      std::string error;
      if (!server->SendAudioControl(client, kQEMUAudioEnd, error)) {
        std::cerr << "onekvm-protocol-vnc: " << error << '\n';
        rfbCloseClient(client);
        return TRUE;
      }
      data->audio_started = false;
    }
    data->audio_sample_format = sample_format;
    data->audio_channels = channels;
    data->audio_frequency = frequency;
    data->audio_resample_position = 0;
    rfbLog("QEMU audio format %u channels=%u rate=%u for client %s\n",
           sample_format, channels, frequency, client->host);
    return TRUE;
  }
  rfbLog("Invalid QEMU audio operation %u from client %s\n", operation,
         client->host);
  rfbCloseClient(client);
  return TRUE;
}

rfbBool VNCServer::ContinuousEnablePseudoEncoding(rfbClientPtr client,
                                                  void **,
                                                  int encoding_number) {
  if (encoding_number != kContinuousUpdatesEncoding)
    return FALSE;
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data != nullptr)
    data->continuous_supported = true;
  rfbLog("Enabling ContinuousUpdates protocol extension for client %s\n",
         client->host);
  return TRUE;
}

rfbBool VNCServer::ContinuousHandleMessage(
    rfbClientPtr client, void *, const rfbClientToServerMsg *message) {
  if (message->type != kEnableContinuousUpdatesMessage)
    return FALSE;
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data == nullptr || !data->continuous_supported)
    return FALSE;
  std::array<std::uint8_t, 9> payload{};
  if (rfbReadExact(client, reinterpret_cast<char *>(payload.data()),
                   static_cast<int>(payload.size())) <= 0) {
    rfbCloseClient(client);
    return TRUE;
  }
  const bool enabled = payload[0] != 0;
  data->continuous_enabled = enabled;
  data->need_update = enabled;
  data->force_update = enabled;
  rfbLog("ContinuousUpdates %s for client %s\n",
         enabled ? "enabled" : "disabled", client->host);
  return TRUE;
}

void VNCServer::SetFrame(JPEGFrame frame) {
  std::lock_guard lock(frame_mutex_);
  latest_frame_ = std::move(frame);
  ++latest_sequence_;
  Wake();
}

void VNCServer::QueueAudio(std::vector<std::int16_t> pcm) {
  if (pcm.empty())
    return;
  std::lock_guard lock(audio_mutex_);
  while (audio_queue_.size() >= kMaxQueuedAudioFrames)
    audio_queue_.pop_front();
  audio_queue_.push_back(std::move(pcm));
  Wake();
}

void VNCServer::SetFatal(std::string error) {
  {
    std::lock_guard lock(fatal_mutex_);
    fatal_error_ = std::move(error);
  }
  fatal_.store(true);
  Wake();
}

void VNCServer::SyncSubscription() {
  if (clients_ > 0 && subscription_ == nullptr) {
    subscription_ = std::make_unique<MediaSubscription>(
        config_.media_socket, identity_, config_.fps, config_.jpeg_quality,
        [this](JPEGFrame frame) { SetFrame(std::move(frame)); }, [] {},
        [this](std::string error) { SetFatal(std::move(error)); });
    subscription_->Start();
  } else if (clients_ == 0 && subscription_ != nullptr) {
    subscription_->Stop();
    subscription_.reset();
    ClearLatestFrame();
  }
}

void VNCServer::SyncAudioSubscription() {
  bool wanted = false;
  auto iterator = rfbGetClientIterator(server_);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (data != nullptr && data->audio_enabled) {
      wanted = true;
      break;
    }
  }
  rfbReleaseClientIterator(iterator);

  if (wanted && audio_subscription_ == nullptr) {
    audio_subscription_ = std::make_unique<AudioSubscription>(
        config_.media_socket, identity_,
        [this](std::vector<std::int16_t> pcm) {
          QueueAudio(std::move(pcm));
        },
        [](std::string error) {
          std::cerr << "onekvm-protocol-vnc: " << error << '\n';
        });
    audio_subscription_->Start();
  } else if (!wanted && audio_subscription_ != nullptr) {
    audio_subscription_->Stop();
    audio_subscription_.reset();
    std::lock_guard lock(audio_mutex_);
    audio_queue_.clear();
  }
}

bool VNCServer::SendAudioCapability(rfbClientPtr client, std::string &error) {
  rfbFramebufferUpdateMsg update{};
  update.type = rfbFramebufferUpdate;
  update.nRects = Swap16IfLE(1);
  rfbFramebufferUpdateRectHeader rectangle{};
  rectangle.r.x = 0;
  rectangle.r.y = 0;
  rectangle.r.w = Swap16IfLE(width_);
  rectangle.r.h = Swap16IfLE(height_);
  rectangle.encoding = Swap32IfLE(kQEMUAudioEncoding);
  std::array<std::uint8_t, sizeof(update) + sizeof(rectangle)> message{};
  std::memcpy(message.data(), &update, sizeof(update));
  std::memcpy(message.data() + sizeof(update), &rectangle, sizeof(rectangle));
  if (rfbWriteExact(client, reinterpret_cast<const char *>(message.data()),
                    static_cast<int>(message.size())) < 0) {
    error = "send QEMU audio capability";
    return false;
  }
  return true;
}

bool VNCServer::SendAudioControl(rfbClientPtr client, std::uint16_t operation,
                                 std::string &error) {
  const std::array<std::uint8_t, 4> message{
      kQEMUMessage, kQEMUAudioSubmessage,
      static_cast<std::uint8_t>(operation >> 8U),
      static_cast<std::uint8_t>(operation)};
  if (rfbWriteExact(client, reinterpret_cast<const char *>(message.data()),
                    static_cast<int>(message.size())) < 0) {
    error = "send QEMU audio control";
    return false;
  }
  return true;
}

bool VNCServer::SendAudioData(rfbClientPtr client,
                              const std::vector<std::uint8_t> &pcm,
                              std::string &error) {
  if (pcm.empty())
    return true;
  if (pcm.size() > static_cast<std::size_t>(INT_MAX) - 8U) {
    error = "QEMU audio packet is too large";
    return false;
  }
  std::vector<std::uint8_t> message;
  message.reserve(8U + pcm.size());
  message.push_back(kQEMUMessage);
  message.push_back(kQEMUAudioSubmessage);
  message.push_back(static_cast<std::uint8_t>(kQEMUAudioData >> 8U));
  message.push_back(static_cast<std::uint8_t>(kQEMUAudioData));
  const auto size = static_cast<std::uint32_t>(pcm.size());
  message.push_back(static_cast<std::uint8_t>(size >> 24U));
  message.push_back(static_cast<std::uint8_t>(size >> 16U));
  message.push_back(static_cast<std::uint8_t>(size >> 8U));
  message.push_back(static_cast<std::uint8_t>(size));
  message.insert(message.end(), pcm.begin(), pcm.end());
  if (rfbWriteExact(client, reinterpret_cast<const char *>(message.data()),
                    static_cast<int>(message.size())) < 0) {
    error = "send QEMU audio data";
    return false;
  }
  return true;
}

std::vector<std::uint8_t>
VNCServer::ConvertAudio(const std::vector<std::int16_t> &source,
                        ClientData &client) {
  const auto source_frames = source.size() / 2U;
  if (source_frames == 0 || client.audio_frequency == 0 ||
      client.audio_frequency > kOpusSampleRate ||
      (client.audio_channels != 1 && client.audio_channels != 2) ||
      client.audio_sample_format > 5)
    return {};

  const auto bytes_per_sample =
      static_cast<std::size_t>(1U << (client.audio_sample_format / 2U));
  const auto estimated_frames =
      (source_frames * client.audio_frequency + kOpusSampleRate - 1U) /
      kOpusSampleRate;
  std::vector<std::uint8_t> output;
  output.reserve(estimated_frames * client.audio_channels * bytes_per_sample);
  const std::uint64_t limit =
      static_cast<std::uint64_t>(source_frames) * client.audio_frequency;
  auto position = client.audio_resample_position;
  while (position < limit) {
    const auto index = static_cast<std::size_t>(position / client.audio_frequency);
    const auto fraction = position % client.audio_frequency;
    const auto next = std::min(index + 1U, source_frames - 1U);
    auto interpolate = [&](std::size_t channel) {
      const auto first = static_cast<std::int64_t>(source[index * 2U + channel]);
      const auto second = static_cast<std::int64_t>(source[next * 2U + channel]);
      return static_cast<std::int16_t>(
          (first * static_cast<std::int64_t>(client.audio_frequency - fraction) +
           second * static_cast<std::int64_t>(fraction)) /
          static_cast<std::int64_t>(client.audio_frequency));
    };
    const auto left = interpolate(0);
    const auto right = interpolate(1);
    if (client.audio_channels == 1) {
      const auto mono = static_cast<std::int16_t>(
          (static_cast<std::int32_t>(left) + right) / 2);
      AppendAudioSample(output, mono, client.audio_sample_format);
    } else {
      AppendAudioSample(output, left, client.audio_sample_format);
      AppendAudioSample(output, right, client.audio_sample_format);
    }
    position += kOpusSampleRate;
  }
  client.audio_resample_position = position - limit;
  return output;
}

bool VNCServer::ProcessAudio(std::string &error) {
  std::deque<std::vector<std::int16_t>> queued;
  {
    std::lock_guard lock(audio_mutex_);
    queued.swap(audio_queue_);
  }
  std::vector<std::shared_ptr<const std::vector<std::int16_t>>> shared;
  shared.reserve(queued.size());
  for (auto &source : queued)
    shared.push_back(std::make_shared<const std::vector<std::int16_t>>(std::move(source)));
  std::vector<rfbClientPtr> close_clients;
  auto iterator = rfbGetClientIterator(server_);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (data == nullptr)
      continue;
    if (data->audio_enabled) {
      for (const auto &source : shared) {
        data->deferred_audio.push_back(source);
        data->deferred_audio_samples += source->size();
      }
      if (data->deferred_audio_samples > kMaxDeferredAudioSamples) {
        rfbLog("Closing client %s: audio output backlog exceeds 500ms\n", client->host);
        close_clients.push_back(client);
        continue;
      }
    }
    if (data->pending_jpeg.owner != nullptr)
      continue;
    if (data->audio_capability_pending) {
      if (!SendAudioCapability(client, error)) {
        close_clients.push_back(client);
        continue;
      }
      data->audio_capability_pending = false;
    }
    if (!data->audio_enabled || data->deferred_audio.empty())
      continue;
    if (!data->audio_started) {
      if (!SendAudioControl(client, kQEMUAudioBegin, error)) {
        close_clients.push_back(client);
        continue;
      }
      data->audio_started = true;
    }
    for (const auto &source : data->deferred_audio) {
      auto pcm = ConvertAudio(*source, *data);
      if (!SendAudioData(client, pcm, error)) {
        close_clients.push_back(client);
        break;
      }
    }
    data->deferred_audio.clear();
    data->deferred_audio_samples = 0;
  }
  rfbReleaseClientIterator(iterator);
  CloseClients(close_clients);
  // A failed client is isolated; it must not terminate the VNC service or
  // interrupt video for other clients.
  error.clear();
  return true;
}

void VNCServer::ClearLatestFrame() {
  std::lock_guard lock(frame_mutex_);
  latest_frame_ = {};
  latest_sequence_ = 0;
  last_jpeg_sent_ = {};
  last_framebuffer_processed_ = {};
  previous_rgb_.clear();
  decoded_sequence_ = 0;
}

bool VNCServer::Resize(std::uint16_t width, std::uint16_t height,
                       std::string &error) {
  std::vector<char> framebuffer;
  if (!AllocateFramebuffer(width, height, framebuffer, error))
    return false;
  framebuffer_.swap(framebuffer);
  width_ = width;
  height_ = height;
  input_.Resize(width_, height_);
  rfbNewFramebuffer(server_, framebuffer_.data(), width_, height_, 5, 3,
                    kBytesPerFramebufferPixel);
  ApplyRGB565ServerFormat(server_, true);
  previous_rgb_.clear();
  decoded_sequence_ = 0;

  std::vector<rfbClientPtr> close_clients;
  auto iterator = rfbGetClientIterator(server_);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (!client->useNewFBSize) {
      close_clients.push_back(client);
      continue;
    }
    if (!rfbSendFramebufferUpdate(client, client->modifiedRegion)) {
      close_clients.push_back(client);
      continue;
    }
    ClearClientRegions(client);
    if (data != nullptr) {
      data->need_update = data->continuous_enabled;
      data->force_update = false;
      data->last_sequence = 0;
    }
  }
  rfbReleaseClientIterator(iterator);
  CloseClients(close_clients);
  return true;
}

bool VNCServer::ClientWantsTightJPEG(rfbClientPtr client) const {
  if (client == nullptr)
    return false;
  /* Quality/fine-quality pseudo-encodings mean the client can decode Tight
     JPEG. Do not require preferredEncoding==Tight: many clients list ZRLE
     first and still accept JPEG. */
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
  return client->tightQualityLevel >= 0 || client->turboQualityLevel >= 0;
#else
  return client->tightQualityLevel >= 0;
#endif
}

bool VNCServer::ClientWantsNativeJPEG(rfbClientPtr client) const {
  return client != nullptr &&
         client->preferredEncoding == kNativeJPEGEncoding;
}

bool VNCServer::DecodeLatestFrame(const JPEGFrame &frame,
                                  std::uint64_t sequence,
                                  std::string &error) {
  if (decoded_sequence_ == sequence && !previous_rgb_.empty())
    return true;
  auto *pixels = reinterpret_cast<std::uint16_t *>(framebuffer_.data());
  const int count = static_cast<int>(width_) * static_cast<int>(height_);
  if (previous_rgb_.size() != static_cast<std::size_t>(count))
    previous_rgb_.clear();
  if (!DecodeJPEGToRGB565(frame.data, frame.size, pixels, width_, height_,
                          error))
    return false;
  // Keep damage for every viewer, including clients that have not requested
  // their next update yet. One viewer advancing the decoder must not lose
  // changes for another, slower viewer.
  const auto tiles = CollectDirtyTiles(
      previous_rgb_.empty() ? nullptr : previous_rgb_.data(), pixels,
      width_, height_);
  for (const auto &tile : tiles)
    rfbMarkRectAsModified(server_, tile.x, tile.y, tile.x + tile.width,
                          tile.y + tile.height);
  previous_rgb_.assign(pixels, pixels + count);
  decoded_sequence_ = sequence;
  return true;
}

bool VNCServer::SendFramebuffer(rfbClientPtr client, bool force_update,
                                bool &sent, std::string &error) {
  sent = false;
  auto *data = static_cast<ClientData *>(client->clientData);
  LOCK(client->updateMutex);
  if (force_update || (data != nullptr && data->continuous_enabled)) {
    auto region = sraRgnCreateRect(0, 0, width_, height_);
    if (force_update)
      sraRgnOr(client->modifiedRegion, region);
    // ContinuousUpdates viewers do not request every frame individually.
    if (data != nullptr && data->continuous_enabled)
      sraRgnOr(client->requestedRegion, region);
    sraRgnDestroy(region);
  }
  auto pending = sraRgnCreateRgn(client->modifiedRegion);
  sraRgnAnd(pending, client->requestedRegion);
  const bool has_pixels = !sraRgnEmpty(pending);
  sraRgnDestroy(pending);
  UNLOCK(client->updateMutex);
  if (!has_pixels)
    return true;
  if (!rfbSendFramebufferUpdate(client, client->modifiedRegion)) {
    error = "send VNC framebuffer update";
    return false;
  }
  // The library consumes the request and preserves damage outside it.
  // An unchanged frame keeps the outstanding request pending.
  sent = true;
  return true;
}

void VNCServer::Wake() {
  const std::uint64_t one = 1;
  if (wake_fd_ >= 0)
    (void)write(wake_fd_, &one, sizeof(one));
}

bool VNCServer::HasPendingJPEG() const {
  for (auto *client = server_->clientHead; client != nullptr; client = client->next) {
    const auto *data = static_cast<const ClientData *>(client->clientData);
    if (data != nullptr && data->pending_jpeg.owner != nullptr)
      return true;
  }
  return false;
}

void VNCServer::FlushJPEGWrites() {
  const auto now = std::chrono::steady_clock::now();
  for (auto *client = server_->clientHead; client != nullptr; client = client->next) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (client->sock == RFB_INVALID_SOCKET || data == nullptr || data->pending_jpeg.owner == nullptr)
      continue;
    if (now - data->jpeg_started > std::chrono::seconds(2)) {
      data->pending_jpeg = {};
      rfbCloseClient(client);
      continue;
    }
    const auto &frame = data->pending_jpeg;
    iovec chunks[2]{};
    int used = 0;
    if (data->jpeg_written < data->jpeg_header_size)
      chunks[used++] = {data->jpeg_header.data() + data->jpeg_written,
                        data->jpeg_header_size - data->jpeg_written};
    const auto offset = data->jpeg_written > data->jpeg_header_size
                            ? data->jpeg_written - data->jpeg_header_size : 0;
    chunks[used++] = {const_cast<std::uint8_t *>(frame.data + offset), frame.size - offset};
    msghdr message{};
    message.msg_iov = chunks;
    message.msg_iovlen = used;
    const auto sent = sendmsg(client->sock, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        data->pending_jpeg = {};
        rfbCloseClient(client);
      }
      continue;
    }
    data->jpeg_written += static_cast<std::size_t>(sent);
    if (data->jpeg_written == data->jpeg_header_size + frame.size) {
      const int raw_size = static_cast<int>(frame.width) * frame.height *
                              std::max(client->format.bitsPerPixel / 8, 1) +
                          sz_rfbFramebufferUpdateRectHeader;
      rfbStatRecordEncodingSent(client, data->jpeg_encoding,
          static_cast<int>(data->jpeg_header_size - sz_rfbFramebufferUpdateMsg + frame.size), raw_size);
      data->pending_jpeg = {};
    }
  }
}

void VNCServer::PollClientEvents() {
  // Media, audio and fatal callbacks wake the foreground without a 1ms timer.
  // LibVNCServer sockets and lifecycle remain confined to this thread.
  std::vector<pollfd> descriptors{{wake_fd_, POLLIN, 0}};
  for (int fd = 0; fd <= server_->maxFd; ++fd) {
    if (!FD_ISSET(fd, &server_->allFds))
      continue;
    short events = POLLIN;
    for (auto *client = server_->clientHead; client != nullptr; client = client->next) {
      const auto *data = static_cast<const ClientData *>(client->clientData);
      if (client->sock == fd && data != nullptr && data->pending_jpeg.owner != nullptr) {
        events = POLLOUT;
        break;
      }
    }
    descriptors.push_back({fd, events, 0});
  }
  int timeout_ms = 20;
  const auto now = std::chrono::steady_clock::now();
  const auto interval = std::chrono::microseconds(1000000 / config_.fps);
  if (last_jpeg_sent_ != std::chrono::steady_clock::time_point{}) {
    const auto due = last_jpeg_sent_ + interval - interval / 10;
    if (due > now) {
      const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(due - now).count();
      timeout_ms = std::min(timeout_ms, static_cast<int>((wait_us + 999) / 1000));
    }
  }
  (void)poll(descriptors.data(), descriptors.size(), timeout_ms);
  std::uint64_t count;
  while (read(wake_fd_, &count, sizeof(count)) == sizeof(count)) {}
  FlushJPEGWrites();
  // The appliance builds LibVNCServer without threads, so its iterator includes
  // closed sockets. Reap them before rfbCheckFds can call FD_ISSET(-1).
  auto reap_closed = [this] {
    auto *client = server_->clientHead;
    while (client != nullptr) {
      auto *next = client->next;
      if (client->sock == RFB_INVALID_SOCKET)
        rfbClientConnectionGone(client);
      client = next;
    }
  };
  reap_closed();
  // Do not parse incoming messages that can trigger library replies while a
  // JPEG message is partially written: the two RFB messages must not interleave.
  std::vector<std::pair<rfbClientPtr, int>> deferred;
  for (auto *client = server_->clientHead; client != nullptr; client = client->next) {
    const auto *data = static_cast<const ClientData *>(client->clientData);
    if (client->sock != RFB_INVALID_SOCKET && data != nullptr && data->pending_jpeg.owner != nullptr) {
      deferred.emplace_back(client, client->sock);
      FD_CLR(client->sock, &server_->allFds);
    }
  }
  // Automatic framebuffer sending would consume requests before real pixels.
  rfbCheckFds(server_, 0);
  for (const auto &[original, fd] : deferred) {
    for (auto *live = server_->clientHead; live != nullptr; live = live->next) {
      if (live == original && live->sock == fd) {
        FD_SET(fd, &server_->allFds);
        server_->maxFd = std::max(server_->maxFd, fd);
        break;
      }
    }
  }
  reap_closed();
}

bool VNCServer::QueueJPEG(rfbClientPtr client, const JPEGFrame &frame,
                          int encoding, std::string &error) {
  auto *data = static_cast<ClientData *>(client->clientData);
  if (data == nullptr || data->pending_jpeg.owner != nullptr || client->ublen != 0) {
    error = "VNC output already pending";
    return false;
  }
  rfbFramebufferUpdateMsg update{};
  update.type = rfbFramebufferUpdate;
  update.nRects = Swap16IfLE(1);
  rfbFramebufferUpdateRectHeader rectangle{};
  rectangle.r.w = Swap16IfLE(frame.width);
  rectangle.r.h = Swap16IfLE(frame.height);
  rectangle.encoding = Swap32IfLE(encoding);
  static_assert(sizeof(update) + sizeof(rectangle) + 4 <= 20);
  std::memcpy(data->jpeg_header.data(), &update, sizeof(update));
  std::memcpy(data->jpeg_header.data() + sizeof(update), &rectangle, sizeof(rectangle));
  data->jpeg_header_size = sizeof(update) + sizeof(rectangle);
  if (encoding == rfbEncodingTight) {
    data->jpeg_header[data->jpeg_header_size++] = rfbTightJpeg << 4;
    auto size = frame.size;
    data->jpeg_header[data->jpeg_header_size++] = (size & 0x7f) | (size > 0x7f ? 0x80 : 0);
    if (size > 0x7f) {
      data->jpeg_header[data->jpeg_header_size++] = ((size >> 7) & 0x7f) | (size > 0x3fff ? 0x80 : 0);
      if (size > 0x3fff)
        data->jpeg_header[data->jpeg_header_size++] = (size >> 14) & 0xff;
    }
  }
  data->pending_jpeg = frame;
  data->jpeg_written = 0;
  data->jpeg_encoding = encoding;
  data->jpeg_started = std::chrono::steady_clock::now();
  ClearClientRegions(client);
  return true;
}

bool VNCServer::SendJPEG(rfbClientPtr client, const JPEGFrame &frame,
                         std::string &error) {
  if (frame.owner == nullptr || frame.data == nullptr || frame.size == 0 || frame.size > 0x3fffff) {
    error = "invalid VNC Tight JPEG frame";
    return false;
  }
  return QueueJPEG(client, frame, rfbEncodingTight, error);
}

bool VNCServer::SendNativeJPEG(rfbClientPtr client, const JPEGFrame &frame,
                               std::string &error) {
  if (frame.owner == nullptr || frame.data == nullptr || frame.size < 4 ||
      frame.size > static_cast<std::size_t>(INT_MAX) || frame.data[0] != 0xff ||
      frame.data[1] != 0xd8 || frame.data[frame.size - 2] != 0xff || frame.data[frame.size - 1] != 0xd9) {
    error = "invalid native VNC JPEG frame";
    return false;
  }
  return QueueJPEG(client, frame, kNativeJPEGEncoding, error);
}

bool VNCServer::ProcessFrame(std::string &error) {
  JPEGFrame frame;
  std::uint64_t sequence = 0;
  {
    std::lock_guard lock(frame_mutex_);
    frame = latest_frame_;
    sequence = latest_sequence_;
  }
  if (sequence == 0 || frame.owner == nullptr)
    return true;

  if (frame.width != width_ || frame.height != height_) {
    if (HasPendingJPEG())
      return true;
    if (!Resize(frame.width, frame.height, error))
      return false;
    return true;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto jpeg_interval = std::chrono::microseconds(1000000 / config_.fps);
  const int framebuffer_fps = std::min(config_.fps, 15);
  const auto framebuffer_interval =
      std::chrono::microseconds(1000000 / std::max(framebuffer_fps, 1));

  bool sent_jpeg = false;
  bool sent_framebuffer = false;
  bool decoded = false;
  std::vector<rfbClientPtr> close_clients;
  auto iterator = rfbGetClientIterator(server_);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (data == nullptr || data->pending_jpeg.owner != nullptr || !data->need_update ||
        (!data->force_update && data->last_sequence == sequence))
      continue;
    const bool native_jpeg = ClientWantsNativeJPEG(client);
    const bool tight_jpeg = !native_jpeg && ClientWantsTightJPEG(client);
    const bool direct_jpeg = native_jpeg || tight_jpeg;
    if (!data->logged_encoding) {
      std::cerr << "onekvm-protocol-vnc: " << client->host << " using "
                << (native_jpeg
                        ? "native JPEG"
                        : (tight_jpeg ? "Tight JPEG"
                                      : "Raw/ZRLE framebuffer"))
                << " (preferred=" << client->preferredEncoding << ")\n";
      data->logged_encoding = true;
    }
    if (direct_jpeg) {
      if (last_jpeg_sent_ != std::chrono::steady_clock::time_point{} &&
          now - last_jpeg_sent_ < jpeg_interval - jpeg_interval / 10 && !data->force_update)
        continue;
    } else {
      if (last_framebuffer_processed_ != std::chrono::steady_clock::time_point{} &&
          now - last_framebuffer_processed_ < framebuffer_interval &&
          !data->force_update)
        continue;
      if (!decoded) {
        std::string decode_error;
        if (!DecodeLatestFrame(frame, sequence, decode_error)) {
          std::cerr << "onekvm-protocol-vnc: " << decode_error << '\n';
          close_clients.push_back(client);
          continue;
        }
        decoded = true;
      }
    }
    std::string send_error;
    bool framebuffer_sent = false;
    const bool ok = native_jpeg
                        ? SendNativeJPEG(client, frame, send_error)
                        : (tight_jpeg
                               ? SendJPEG(client, frame, send_error)
                               : SendFramebuffer(client, data->force_update,
                                                 framebuffer_sent,
                                                 send_error));
    if (!ok) {
      std::cerr << "onekvm-protocol-vnc: " << send_error << '\n';
      close_clients.push_back(client);
      continue;
    }
    if (!direct_jpeg && !framebuffer_sent)
      continue;
    data->need_update = data->continuous_enabled;
    data->force_update = false;
    data->last_sequence = sequence;
    if (direct_jpeg)
      sent_jpeg = true;
    else
      sent_framebuffer = true;
  }
  rfbReleaseClientIterator(iterator);
  CloseClients(close_clients);
  if (sent_jpeg) {
    if (last_jpeg_sent_ == std::chrono::steady_clock::time_point{})
      last_jpeg_sent_ = now;
    else {
      last_jpeg_sent_ += jpeg_interval;
      if (last_jpeg_sent_ + jpeg_interval <= now)
        last_jpeg_sent_ += jpeg_interval * ((now - last_jpeg_sent_) / jpeg_interval);
    }
    FlushJPEGWrites();
  }
  // Waiting on unchanged video still incurs a decode. Keep that work within
  // the fallback FPS limit while preserving the outstanding client request.
  if (sent_framebuffer || decoded)
    last_framebuffer_processed_ = now;
  return true;
}

bool VNCServer::Run(volatile char &exit_flag, std::string &error) {
  while (exit_flag == 0 && !fatal_.load()) {
    PollClientEvents();
    SyncSubscription();
    SyncAudioSubscription();
    if (!ProcessAudio(error))
      return false;
    if (!ProcessFrame(error))
      return false;
  }
  if (fatal_.load()) {
    std::lock_guard lock(fatal_mutex_);
    error = fatal_error_;
    return false;
  }
  return true;
}

} // namespace onekvm::vnc
