#pragma once

#include "protocol_ipc.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace onekvm::vnc {

class AdminHID {
public:
  AdminHID(std::string socket_path, Identity identity);
  ~AdminHID();
  AdminHID(const AdminHID &) = delete;
  AdminHID &operator=(const AdminHID &) = delete;

  bool SendKeyboard(std::uint8_t modifiers,
                    const std::vector<std::uint8_t> &keys, std::string &error);
  bool SendAbsoluteMouse(std::uint16_t buttons, std::uint16_t x,
                         std::uint16_t y, std::string &error);
  bool SendMouse(std::uint8_t buttons, std::int8_t wheel, std::string &error);

private:
  bool Post(const std::string &path, const std::string &payload,
            std::string &error);
  bool Connect(std::string &error);
  void Close();

  std::string socket_path_;
  Identity identity_;
  std::mutex mutex_;
  int socket_fd_ = -1;
};

} // namespace onekvm::vnc
