#include "vnc_server.hpp"

#include <jpeglib.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace onekvm::vnc {
namespace {

void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Write(int fd, const void *data, std::size_t size) {
  Require(send(fd, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size),
          "write RFB message");
}

void Read(int fd, void *data, std::size_t size) {
  auto *bytes = static_cast<char *>(data);
  while (size != 0) {
    pollfd descriptor{fd, POLLIN, 0};
    Require(poll(&descriptor, 1, 1000) == 1, "missing RFB update");
    const auto count = recv(fd, bytes, size, 0);
    Require(count > 0, "read RFB update");
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
}

void ExpectPending(int fd) {
  pollfd descriptor{fd, POLLIN, 0};
  Require(poll(&descriptor, 1, 10) == 0,
          "unexpected update before valid or changed video pixels");
}

JPEGFrame SolidFrame(int channel) {
  constexpr int width = 64, height = 64;
  jpeg_compress_struct info{};
  jpeg_error_mgr error{};
  info.err = jpeg_std_error(&error);
  jpeg_create_compress(&info);
  unsigned char *output = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&info, &output, &size);
  info.image_width = width;
  info.image_height = height;
  info.input_components = 3;
  info.in_color_space = JCS_RGB;
  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, 100, TRUE);
  jpeg_start_compress(&info, TRUE);
  std::array<unsigned char, width * 3> row{};
  for (int x = 0; x < width; ++x)
    row[x * 3 + channel] = 255;
  while (info.next_scanline < info.image_height) {
    JSAMPROW rows[] = {row.data()};
    jpeg_write_scanlines(&info, rows, 1);
  }
  jpeg_finish_compress(&info);
  auto owner = std::shared_ptr<void>(output, std::free);
  jpeg_destroy_compress(&info);
  return {std::move(owner), output, size, width, height, 16667};
}

struct Peer {
  int fd;
  int bits;
  ~Peer() { close(fd); }

  void Request(bool incremental) const {
    rfbFramebufferUpdateRequestMsg request{};
    request.type = rfbFramebufferUpdateRequest;
    request.incremental = incremental;
    request.w = Swap16IfLE(64);
    request.h = Swap16IfLE(64);
    Write(fd, &request, sizeof(request));
  }

  void ExpectColor(int channel) const {
    rfbFramebufferUpdateMsg update{};
    Read(fd, &update, sizeof(update));
    Require(update.type == rfbFramebufferUpdate, "expected framebuffer update");
    bool found = false;
    const int count = Swap16IfLE(update.nRects);
    for (int index = 0; index < count; ++index) {
      rfbFramebufferUpdateRectHeader rect{};
      Read(fd, &rect, sizeof(rect));
      Require(Swap32IfLE(rect.encoding) == rfbEncodingRaw,
              "expected Raw rectangle");
      const int x = Swap16IfLE(rect.r.x), y = Swap16IfLE(rect.r.y);
      const int w = Swap16IfLE(rect.r.w), h = Swap16IfLE(rect.r.h);
      std::vector<std::uint8_t> pixels(w * h * bits / 8);
      Read(fd, pixels.data(), pixels.size());
      if (x <= 32 && y <= 32 && x + w > 32 && y + h > 32) {
        const auto offset = ((32 - y) * w + 32 - x) * (bits / 8);
        std::uint16_t pixel = pixels[offset];
        if (bits == 16)
          std::memcpy(&pixel, pixels.data() + offset, sizeof(pixel));
        const int shift = bits == 8 ? (2 - channel) * 2
                                   : std::array<int, 3>{11, 5, 0}[channel];
        const int maximum = bits == 8 ? 3 : (channel == 1 ? 63 : 31);
        Require(((pixel >> shift) & maximum) == maximum,
                "framebuffer is black, stale, or has the wrong color");
        found = true;
      }
    }
    Require(found, "update did not cover the changed pixel");
  }
};

} // namespace

class VNCServerTest {
public:
  static void DirectJPEGDoesNotBlockOtherClients(int encoding) {
    Config config;
    config.port = -1;
    config.media_socket = "/nonexistent-vnc-test-media";
    config.admin_socket = "/nonexistent-vnc-test-hid";
    std::string error;
    VNCServer server(config, {}, {64, 64, 16667}, error);
    Require(server.Ready(), error.c_str());
    auto fast = AddPeer(server, 16);
    auto slow = AddPeer(server, 16);
    auto *slow_client = server.server_->clientHead;
    auto closing = AddPeer(server, 16);
    Require(server.server_->clientHead->sock > slow_client->sock, "closing peer must have highest fd");
    int small_buffer = 1024;
    Require(setsockopt(slow_client->sock, SOL_SOCKET, SO_SNDBUF,
                      &small_buffer, sizeof(small_buffer)) == 0, "small send buffer");
    const std::array<std::uint8_t, 8> native{2, 0, 0, 1, 0, 0, 0, 21};
    Write(slow.fd, native.data(), native.size());
    server.PollClientEvents();
    if (encoding == 21) {
      Write(fast.fd, native.data(), native.size());
    } else {
      const std::array<std::uint8_t, 12> tight{2, 0, 0, 2, 0, 0, 0, 7, 255, 255, 255, 233};
      Write(fast.fd, tight.data(), tight.size());
    }
    server.PollClientEvents();
    auto small = SolidFrame(0);
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    bytes->insert(bytes->end(), small.data, small.data + 2);
    // Valid large APP segments make a JPEG large enough to force partial writes.
    for (int i = 0; i < 20; ++i) {
      bytes->insert(bytes->end(), {0xff, 0xef, 0xff, 0xf0});
      bytes->insert(bytes->end(), 65518, 0);
    }
    bytes->insert(bytes->end(), small.data + 2, small.data + small.size);
    std::weak_ptr<std::vector<std::uint8_t>> retained = bytes;
    slow.Request(false);
    server.PollClientEvents();
    server.SetFrame({bytes, bytes->data(), bytes->size(), 64, 64, 16667});
    bytes.reset();
    Process(server);
    auto *slow_data = static_cast<VNCServer::ClientData *>(slow_client->clientData);
    Require(slow_data->pending_jpeg.owner != nullptr && slow_data->jpeg_written > 0,
            "expected retained partially written JPEG");
    Require(shutdown(closing.fd, SHUT_RDWR) == 0, "close highest fd peer");
    server.PollClientEvents();
    Require(FD_ISSET(slow_client->sock, &server.server_->allFds) &&
                server.server_->maxFd >= slow_client->sock,
            "pending client lost fd scan range after higher fd closed");
    auto latest = SolidFrame(1);
    server.SetFrame(latest);
    fast.Request(false);
    server.PollClientEvents();
    const auto start = std::chrono::steady_clock::now();
    Process(server);
    Require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
            "slow JPEG blocked foreground");
    rfbFramebufferUpdateMsg update{};
    rfbFramebufferUpdateRectHeader rect{};
    Read(fast.fd, &update, sizeof(update));
    Read(fast.fd, &rect, sizeof(rect));
    Require(Swap16IfLE(update.nRects) == 1 && static_cast<int>(Swap32IfLE(rect.encoding)) == encoding,
            "wrong direct JPEG header");
    if (encoding == 7) {
      std::uint8_t control;
      Read(fast.fd, &control, 1);
      Require(control == (rfbTightJpeg << 4), "Tight JPEG control");
      std::size_t size = 0;
      for (int shift : {0, 7, 14}) {
        std::uint8_t value;
        Read(fast.fd, &value, 1);
        size |= static_cast<std::size_t>(shift == 14 ? value : value & 0x7f) << shift;
        if (shift == 14 || !(value & 0x80)) break;
      }
      Require(size == latest.size, "Tight JPEG length");
    }
    std::vector<std::uint8_t> received(latest.size);
    Read(fast.fd, received.data(), received.size());
    Require(std::memcmp(received.data(), latest.data, latest.size) == 0,
            "fast JPEG corrupted by another client's partial send");
    Require(!retained.expired(), "in-flight JPEG owner released early");
    slow_data->jpeg_started -= std::chrono::seconds(3);
    server.FlushJPEGWrites();
    Require(retained.expired(), "timed-out JPEG still retained");
    server.PollClientEvents();
    Require(server.clients_ == 1, "slow client not isolated");
    auto following = SolidFrame(2);
    server.SetFrame(following);
    fast.Request(false);
    server.PollClientEvents();
    Process(server);
    Read(fast.fd, &update, sizeof(update));
    Read(fast.fd, &rect, sizeof(rect));
    Require(Swap16IfLE(update.nRects) == 1 && static_cast<int>(Swap32IfLE(rect.encoding)) == encoding,
            "fast client stopped after slow client closed");
    if (encoding == 7) {
      std::uint8_t value;
      Read(fast.fd, &value, 1);
      Require(value == (rfbTightJpeg << 4), "following Tight control");
      for (int shift : {0, 7, 14}) {
        Read(fast.fd, &value, 1);
        if (shift == 14 || !(value & 0x80)) break;
      }
    }
    received.resize(following.size);
    Read(fast.fd, received.data(), received.size());
    Require(std::memcmp(received.data(), following.data, following.size) == 0,
            "following fast JPEG was corrupted");
    server.PollClientEvents();
  }
  static void AudioWaitsForJPEG() {
    Config config;
    config.port = -1;
    config.media_socket = "/nonexistent-vnc-test-media";
    config.admin_socket = "/nonexistent-vnc-test-hid";
    std::string error;
    VNCServer server(config, {}, {64, 64, 16667}, error);
    Require(server.Ready(), error.c_str());
    auto peer = AddPeer(server, 16);
    auto *client = server.server_->clientHead;
    auto *data = static_cast<VNCServer::ClientData *>(client->clientData);
    data->audio_enabled = true;
    data->audio_frequency = 48000;
    data->pending_jpeg = SolidFrame(0);
    server.QueueAudio(std::vector<std::int16_t>(960, 1234));
    Require(server.ProcessAudio(error), error.c_str());
    ExpectPending(peer.fd);
    Require(data->deferred_audio_samples == 960, "audio dropped behind JPEG");
    data->pending_jpeg = {};
    Require(server.ProcessAudio(error), error.c_str());
    std::array<std::uint8_t, 4> begin{};
    Read(peer.fd, begin.data(), begin.size());
    Require(begin == std::array<std::uint8_t, 4>{255, 1, 0, 1}, "audio begin missing");
    std::array<std::uint8_t, 8> header{};
    Read(peer.fd, header.data(), header.size());
    Require(header == std::array<std::uint8_t, 8>{255, 1, 0, 2, 0, 0, 7, 128},
            "deferred audio length or message order changed");
    std::vector<std::uint8_t> pcm(1920);
    Read(peer.fd, pcm.data(), pcm.size());
    Require(pcm[0] == 0xd2 && pcm[1] == 0x04, "deferred PCM corrupted");
    Require(data->deferred_audio.empty(), "deferred audio not drained");
    data->pending_jpeg = SolidFrame(1);
    server.QueueAudio(std::vector<std::int16_t>(48002, 0));
    Require(server.ProcessAudio(error), error.c_str());
    Require(server.clients_ == 0, "audio overload did not close slow client");
    server.PollClientEvents();
  }
  static void Process(VNCServer &server) {
    // Exercise delivery without waiting for the normal FPS limiter.
    server.last_framebuffer_processed_ = {};
    std::string error;
    Require(server.ProcessFrame(error), error.c_str());
  }

  static Peer AddPeer(VNCServer &server, int bits) {
    int sockets[2];
    Require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0, "socketpair");
    auto *client = rfbNewClient(server.server_, sockets[0]);
    Require(client != nullptr, "create RFB client");
    std::array<char, 12> version{};
    Read(sockets[1], version.data(), version.size());
    // Authentication is covered separately; start at the normal RFB message
    // parser so these tests isolate framebuffer scheduling and pixel delivery.
    client->state = rfbClientRec::RFB_NORMAL;
    rfbSetPixelFormatMsg format{};
    format.type = rfbSetPixelFormat;
    format.format.bitsPerPixel = bits;
    format.format.depth = bits == 8 ? 6 : 16;
    format.format.bigEndian = server.server_->serverFormat.bigEndian;
    format.format.trueColour = TRUE;
    format.format.redMax = Swap16IfLE(bits == 8 ? 3 : 31);
    format.format.greenMax = Swap16IfLE(bits == 8 ? 3 : 63);
    format.format.blueMax = Swap16IfLE(bits == 8 ? 3 : 31);
    format.format.redShift = bits == 8 ? 4 : 11;
    format.format.greenShift = bits == 8 ? 2 : 5;
    format.format.blueShift = 0;
    Write(sockets[1], &format, sizeof(format));
    server.PollClientEvents();
    // Raw plus ContinuousUpdates pseudo-encoding.
    const std::array<std::uint8_t, 12> encodings{
        2, 0, 0, 2, 0, 0, 0, 0, 255, 255, 254, 199};
    Write(sockets[1], encodings.data(), encodings.size());
    server.PollClientEvents();
    return {sockets[1], bits};
  }

  static void Run(int bits) {
    Config config;
    config.port = -1;
    config.media_socket = "/nonexistent-vnc-test-media";
    config.admin_socket = "/nonexistent-vnc-test-hid";
    std::string error;
    VNCServer server(config, {}, {64, 64, 16667}, error);
    Require(server.Ready(), error.c_str());
    server.server_->deferUpdateTime = 0;
    auto first = AddPeer(server, bits);
    first.Request(false);
    server.PollClientEvents();
    Process(server);
    ExpectPending(first.fd);  // No initial black/stale framebuffer.
    server.SetFrame(SolidFrame(0));
    Process(server);
    first.ExpectColor(0);

    first.Request(true);
    server.PollClientEvents();
    server.SetFrame(SolidFrame(0));
    Process(server);
    ExpectPending(first.fd);  // Unchanged video must keep the request alive.
    server.SetFrame(SolidFrame(1));
    Process(server);
    first.ExpectColor(1);  // Changed video arrives without another request.

    auto slow = AddPeer(server, bits);
    slow.Request(false);
    server.PollClientEvents();
    Process(server);
    slow.ExpectColor(1);
    first.Request(true);
    server.PollClientEvents();
    server.SetFrame(SolidFrame(2));
    Process(server);
    first.ExpectColor(2);
    slow.Request(true);
    server.PollClientEvents();
    Process(server);
    slow.ExpectColor(2);  // Damage survives the faster client's delivery.

    const std::array<std::uint8_t, 10> continuous{
        150, 1, 0, 0, 0, 0, 0, 64, 0, 64};
    Write(first.fd, continuous.data(), continuous.size());
    server.PollClientEvents();
    Process(server);
    first.ExpectColor(2);
    server.SetFrame(SolidFrame(0));
    Process(server);
    first.ExpectColor(0);  // Continuous mode does not need per-frame requests.
  }
};

} // namespace onekvm::vnc

int main() {
  try {
    onekvm::vnc::VNCServerTest::Run(8);
    onekvm::vnc::VNCServerTest::Run(16);
    onekvm::vnc::VNCServerTest::DirectJPEGDoesNotBlockOtherClients(21);
    onekvm::vnc::VNCServerTest::DirectJPEGDoesNotBlockOtherClients(7);
    onekvm::vnc::VNCServerTest::AudioWaitsForJPEG();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
