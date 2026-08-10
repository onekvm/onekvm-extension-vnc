#include "admin_hid.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <sstream>
#include <string_view>

namespace onekvm::vnc {
namespace {

constexpr int kTimeoutMilliseconds = 2000;
constexpr std::size_t kMaxResponseHeader = 16U << 10;
constexpr std::size_t kMaxResponseBody = 64U << 10;

bool WaitFor(int fd, short events, std::string_view operation,
             std::string &error) {
  pollfd descriptor{fd, events, 0};
  int result;
  do {
    result = poll(&descriptor, 1, kTimeoutMilliseconds);
  } while (result < 0 && errno == EINTR);
  if (result == 0) {
    error = std::string(operation) + ": timed out";
    return false;
  }
  if (result < 0) {
    error = std::string(operation) + ": " + std::strerror(errno);
    return false;
  }
  if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
      (descriptor.revents & events) == 0) {
    error = std::string(operation) + ": connection closed";
    return false;
  }
  return true;
}

bool WriteAll(int fd, std::string_view data, std::string &error) {
  while (!data.empty()) {
    if (!WaitFor(fd, POLLOUT, "write OneKVM admin API", error))
      return false;
    const auto written = send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (written < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (written <= 0) {
      error = std::string("write OneKVM admin API: ") + std::strerror(errno);
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool ReadMore(int fd, std::string &data, std::size_t limit,
              std::string &error) {
  if (data.size() >= limit) {
    error = "OneKVM admin API response is too large";
    return false;
  }
  if (!WaitFor(fd, POLLIN, "read OneKVM admin API", error))
    return false;
  std::array<char, 4096> buffer{};
  const auto wanted = std::min(buffer.size(), limit - data.size());
  const auto received = recv(fd, buffer.data(), wanted, 0);
  if (received < 0 && (errno == EINTR || errno == EAGAIN))
    return ReadMore(fd, data, limit, error);
  if (received <= 0) {
    error = received == 0
                ? "OneKVM admin API closed the connection"
                : std::string("read OneKVM admin API: ") + std::strerror(errno);
    return false;
  }
  data.append(buffer.data(), static_cast<std::size_t>(received));
  return true;
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool ParseResponse(int fd, int &status, bool &close_connection,
                   std::string &error) {
  std::string response;
  std::size_t header_end = std::string::npos;
  while ((header_end = response.find("\r\n\r\n")) == std::string::npos) {
    if (!ReadMore(fd, response, kMaxResponseHeader, error))
      return false;
  }

  const auto first_line_end = response.find("\r\n");
  if (first_line_end == std::string::npos ||
      response.compare(0, 5, "HTTP/") != 0) {
    error = "OneKVM admin API returned an invalid HTTP response";
    return false;
  }
  const auto first_space = response.find(' ', 5);
  if (first_space == std::string::npos || first_space + 4 > first_line_end) {
    error = "OneKVM admin API returned an invalid HTTP status";
    return false;
  }
  const auto status_text =
      std::string_view(response).substr(first_space + 1, 3);
  const auto converted =
      std::from_chars(status_text.data(), status_text.data() + 3, status);
  if (converted.ec != std::errc{} || converted.ptr != status_text.data() + 3) {
    error = "OneKVM admin API returned an invalid HTTP status";
    return false;
  }

  std::size_t content_length = std::numeric_limits<std::size_t>::max();
  std::size_t line_start = first_line_end + 2;
  while (line_start < header_end) {
    const auto line_end = response.find("\r\n", line_start);
    if (line_end == std::string::npos || line_end > header_end)
      break;
    const auto colon = response.find(':', line_start);
    if (colon != std::string::npos && colon < line_end) {
      auto name = Lower(response.substr(line_start, colon - line_start));
      auto value = response.substr(colon + 1, line_end - colon - 1);
      const auto begin = value.find_first_not_of(" \t");
      value = begin == std::string::npos ? "" : value.substr(begin);
      if (name == "content-length") {
        const auto parsed = std::from_chars(
            value.data(), value.data() + value.size(), content_length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != value.data() + value.size() ||
            content_length > kMaxResponseBody) {
          error = "OneKVM admin API returned an invalid Content-Length";
          return false;
        }
      } else if (name == "connection" && Lower(value) == "close") {
        close_connection = true;
      } else if (name == "transfer-encoding" && Lower(value) != "identity") {
        error = "OneKVM admin API returned an unsupported transfer encoding";
        return false;
      }
    }
    line_start = line_end + 2;
  }
  if (content_length == std::numeric_limits<std::size_t>::max()) {
    error = "OneKVM admin API response has no Content-Length";
    return false;
  }

  const auto body_start = header_end + 4;
  while (response.size() - body_start < content_length) {
    if (!ReadMore(fd, response, body_start + content_length, error))
      return false;
  }
  return true;
}

} // namespace

AdminHID::AdminHID(std::string socket_path, Identity identity)
    : socket_path_(std::move(socket_path)), identity_(std::move(identity)) {}

AdminHID::~AdminHID() { Close(); }

bool AdminHID::Connect(std::string &error) {
  if (socket_fd_ >= 0)
    return true;
  if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
    error = "OneKVM admin socket path is too long";
    return false;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) {
    error = std::string("create OneKVM admin socket: ") + std::strerror(errno);
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
          0 &&
      errno != EINPROGRESS) {
    error = std::string("connect OneKVM admin socket: ") + std::strerror(errno);
    close(fd);
    return false;
  }
  if (!WaitFor(fd, POLLOUT, "connect OneKVM admin socket", error)) {
    close(fd);
    return false;
  }
  int socket_error = 0;
  socklen_t size = sizeof(socket_error);
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &size) < 0 ||
      socket_error != 0) {
    error = std::string("connect OneKVM admin socket: ") +
            std::strerror(socket_error == 0 ? errno : socket_error);
    close(fd);
    return false;
  }
  socket_fd_ = fd;
  return true;
}

void AdminHID::Close() {
  std::lock_guard lock(mutex_);
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
}

bool AdminHID::Post(const std::string &path, const std::string &payload,
                    std::string &error) {
  std::lock_guard lock(mutex_);
  if (!Connect(error))
    return false;
  const std::string request =
      "POST " + path +
      " HTTP/1.1\r\nHost: unix\r\nContent-Type: "
      "application/json\r\nAuthorization: Bearer " +
      identity_.token + "\r\nX-OneKVM-Extension: " + identity_.extension_id +
      "\r\nContent-Length: " + std::to_string(payload.size()) +
      "\r\nConnection: keep-alive\r\n\r\n" + payload;
  if (!WriteAll(socket_fd_, request, error)) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }
  int status = 0;
  bool close_connection = false;
  if (!ParseResponse(socket_fd_, status, close_connection, error)) {
    close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }
  if (close_connection) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  if (status < 200 || status >= 300) {
    error =
        "OneKVM admin API " + path + " returned HTTP " + std::to_string(status);
    return false;
  }
  return true;
}

bool AdminHID::SendKeyboard(std::uint8_t modifiers,
                            const std::vector<std::uint8_t> &keys,
                            std::string &error) {
  std::ostringstream payload;
  payload << "{\"modifiers\":" << static_cast<unsigned>(modifiers)
          << ",\"keys\":[";
  for (std::size_t index = 0; index < keys.size(); ++index) {
    if (index != 0)
      payload << ',';
    payload << static_cast<unsigned>(keys[index]);
  }
  payload << "]}";
  return Post("/api/hid/keyboard", payload.str(), error);
}

bool AdminHID::SendAbsoluteMouse(std::uint16_t buttons, std::uint16_t x,
                                 std::uint16_t y, std::string &error) {
  const std::string payload = "{\"buttons\":" + std::to_string(buttons) +
                              ",\"x\":" + std::to_string(x) +
                              ",\"y\":" + std::to_string(y) + "}";
  return Post("/api/hid/mouse/absolute", payload, error);
}

bool AdminHID::SendMouse(std::uint8_t buttons, std::int8_t wheel,
                         std::string &error) {
  const std::string payload =
      "{\"buttons\":" + std::to_string(buttons) +
      ",\"x\":0,\"y\":0,\"wheel\":" + std::to_string(static_cast<int>(wheel)) +
      "}";
  return Post("/api/hid/mouse", payload, error);
}

} // namespace onekvm::vnc
