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
  server_->alwaysShared = TRUE;
  server_->permitFileTransfer = FALSE;
  server_->newClientHook = NewClient;
  server_->kbdAddEvent = KeyEvent;
  server_->ptrAddEvent = PointerEvent;
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
  rfbRegisterProtocolExtension(&continuous_extension_);
  continuous_extension_registered_ = true;
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
    continuous_extension_registered_ = false;
  }
}

VNCServer::~VNCServer() {
  if (subscription_ != nullptr)
    subscription_->Stop();
  if (server_ != nullptr) {
    rfbShutdownServer(server_, TRUE);
    rfbScreenCleanup(server_);
  }
  if (continuous_extension_registered_)
    rfbUnregisterProtocolExtension(&continuous_extension_);
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
  data->force_update = request->incremental == 0;
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

rfbBool VNCServer::ContinuousNewClient(rfbClientPtr, void **data) {
  *data = nullptr;
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
}

void VNCServer::SetFatal(std::string error) {
  {
    std::lock_guard lock(fatal_mutex_);
    fatal_error_ = std::move(error);
  }
  fatal_.store(true);
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

void VNCServer::ClearLatestFrame() {
  std::lock_guard lock(frame_mutex_);
  latest_frame_ = {};
  latest_sequence_ = 0;
  last_jpeg_sent_ = {};
  last_framebuffer_sent_ = {};
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
  if (client == nullptr || client->preferredEncoding != rfbEncodingTight)
    return false;
#ifdef LIBVNCSERVER_HAVE_LIBJPEG
  return client->tightQualityLevel >= 0 || client->turboQualityLevel >= 0;
#else
  return client->tightQualityLevel >= 0;
#endif
}

bool VNCServer::DecodeLatestFrame(const JPEGFrame &frame, std::string &error) {
  if (decoded_sequence_ == latest_sequence_ && !previous_rgb_.empty())
    return true;
  auto *pixels = reinterpret_cast<std::uint16_t *>(framebuffer_.data());
  const int count = static_cast<int>(width_) * static_cast<int>(height_);
  if (previous_rgb_.size() != static_cast<std::size_t>(count))
    previous_rgb_.assign(static_cast<std::size_t>(count), 0);
  else
    previous_rgb_.assign(pixels, pixels + count);
  if (!DecodeJPEGToRGB565(frame.data, frame.size, pixels, width_, height_,
                          error))
    return false;
  decoded_sequence_ = latest_sequence_;
  return true;
}

bool VNCServer::SendFramebuffer(rfbClientPtr client, bool force_update,
                                std::string &error) {
  const auto *pixels =
      reinterpret_cast<const std::uint16_t *>(framebuffer_.data());
  const auto tiles =
      force_update || previous_rgb_.empty()
          ? std::vector<TileRect>{{0, 0, width_, height_}}
          : CollectDirtyTiles(previous_rgb_.data(), pixels, width_, height_);
  if (tiles.empty())
    return true;
  LOCK(client->updateMutex);
  for (const auto &tile : tiles) {
    sraRegionPtr region =
        sraRgnCreateRect(tile.x, tile.y, tile.x + tile.width,
                         tile.y + tile.height);
    sraRgnOr(client->modifiedRegion, region);
    sraRgnDestroy(region);
  }
  UNLOCK(client->updateMutex);
  if (!rfbSendFramebufferUpdate(client, client->modifiedRegion)) {
    error = "send VNC framebuffer update";
    return false;
  }
  ClearClientRegions(client);
  return true;
}

bool VNCServer::SendJPEG(rfbClientPtr client, const JPEGFrame &frame,
                         std::string &error) {
  if (frame.owner == nullptr || frame.data == nullptr || frame.size == 0 ||
      frame.size > static_cast<std::size_t>(INT_MAX)) {
    error = "invalid VNC JPEG frame";
    return false;
  }

  auto *update = reinterpret_cast<rfbFramebufferUpdateMsg *>(client->updateBuf);
  update->type = rfbFramebufferUpdate;
  update->nRects = Swap16IfLE(1);
  client->ublen = sz_rfbFramebufferUpdateMsg;
  client->tightEncoding = rfbEncodingTight;
  if (!rfbSendTightHeader(client, 0, 0, frame.width, frame.height)) {
    error = "send VNC Tight JPEG rectangle header";
    return false;
  }
  client->updateBuf[client->ublen++] = static_cast<char>(rfbTightJpeg << 4);

  // LibVNCServer's rfbSendCompressedDataTight copies the complete JPEG into
  // its small update buffer in chunks before writing it. The media subscriber
  // already owns a stable shared payload, so send only the Tight compact length
  // through updateBuf and write the compressed bytes directly from that buffer.
  const auto payload_size = static_cast<int>(frame.size);
  std::array<std::uint8_t, 3> compact_length{};
  int compact_length_size = 1;
  compact_length[0] = static_cast<std::uint8_t>(payload_size & 0x7f);
  if (payload_size > 0x7f) {
    compact_length[0] |= 0x80;
    compact_length[1] = static_cast<std::uint8_t>((payload_size >> 7) & 0x7f);
    compact_length_size = 2;
    if (payload_size > 0x3fff) {
      compact_length[1] |= 0x80;
      compact_length[2] = static_cast<std::uint8_t>((payload_size >> 14) & 0xff);
      compact_length_size = 3;
    }
  }
  if (client->ublen + compact_length_size > UPDATE_BUF_SIZE &&
      !rfbSendUpdateBuf(client)) {
    error = "send VNC Tight JPEG frame header";
    return false;
  }
  for (int index = 0; index < compact_length_size; ++index) {
    client->updateBuf[client->ublen++] =
        static_cast<char>(compact_length[static_cast<std::size_t>(index)]);
    rfbStatRecordEncodingSentAdd(client, client->tightEncoding, 1);
  }
  if (!rfbSendUpdateBuf(client) ||
      rfbWriteExact(client, reinterpret_cast<const char *>(frame.data),
                    payload_size) < 0) {
    error = "send VNC Tight JPEG frame";
    return false;
  }
  rfbStatRecordEncodingSentAdd(client, client->tightEncoding, payload_size);
  ClearClientRegions(client);
  return true;
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
    if (data == nullptr || !data->need_update ||
        (!data->force_update && data->last_sequence == sequence))
      continue;
    const bool tight_jpeg = ClientWantsTightJPEG(client);
    if (!data->logged_encoding) {
      std::cerr << "onekvm-protocol-vnc: " << client->host << " using "
                << (tight_jpeg ? "Tight JPEG" : "Raw/ZRLE framebuffer")
                << " (preferred=" << client->preferredEncoding << ")\n";
      data->logged_encoding = true;
    }
    if (tight_jpeg) {
      if (last_jpeg_sent_ != std::chrono::steady_clock::time_point{} &&
          now - last_jpeg_sent_ < jpeg_interval && !data->force_update)
        continue;
    } else {
      if (last_framebuffer_sent_ != std::chrono::steady_clock::time_point{} &&
          now - last_framebuffer_sent_ < framebuffer_interval &&
          !data->force_update)
        continue;
      if (!decoded) {
        std::string decode_error;
        if (!DecodeLatestFrame(frame, decode_error)) {
          std::cerr << "onekvm-protocol-vnc: " << decode_error << '\n';
          close_clients.push_back(client);
          continue;
        }
        decoded = true;
      }
    }
    std::string send_error;
    const bool ok =
        tight_jpeg ? SendJPEG(client, frame, send_error)
                   : SendFramebuffer(client, data->force_update, send_error);
    if (!ok) {
      std::cerr << "onekvm-protocol-vnc: " << send_error << '\n';
      close_clients.push_back(client);
      continue;
    }
    data->need_update = data->continuous_enabled;
    data->force_update = false;
    data->last_sequence = sequence;
    if (tight_jpeg)
      sent_jpeg = true;
    else
      sent_framebuffer = true;
  }
  rfbReleaseClientIterator(iterator);
  CloseClients(close_clients);
  if (sent_jpeg)
    last_jpeg_sent_ = now;
  if (sent_framebuffer)
    last_framebuffer_sent_ = now;
  return true;
}

void VNCServer::SuppressTightJPEGUpdates() {
  auto iterator = rfbGetClientIterator(server_);
  rfbClientPtr client;
  while ((client = rfbClientIteratorNext(iterator)) != nullptr) {
    auto *data = static_cast<ClientData *>(client->clientData);
    if (data != nullptr && data->need_update && ClientWantsTightJPEG(client))
      ClearClientRegions(client);
  }
  rfbReleaseClientIterator(iterator);
}

bool VNCServer::Run(volatile char &exit_flag, std::string &error) {
  while (exit_flag == 0 && !fatal_.load()) {
    rfbProcessEvents(server_, clients_ > 0 ? 1000 : 20000);
    SyncSubscription();
    if (!ProcessFrame(error))
      return false;
    SuppressTightJPEGUpdates();
  }
  if (fatal_.load()) {
    std::lock_guard lock(fatal_mutex_);
    error = fatal_error_;
    return false;
  }
  return true;
}

} // namespace onekvm::vnc
