#pragma once

#include <string>

namespace onekvm::vnc {

struct Config {
  bool service_enabled = true;
  std::string listen_address = "0.0.0.0";
  int port = 5900;
  bool access_control_enabled = false;
  std::string password;
  int fps = 60;
  int jpeg_quality = 20;
  std::string media_socket = "/run/onekvm/extension-media.sock";
  std::string admin_socket = "/run/onekvm/extension-control.sock";
};

bool LoadConfig(const std::string &path, bool allow_missing, Config &config,
                std::string &error);
bool ValidateConfig(Config &config, std::string &error);
std::string DefaultConfigPath();

} // namespace onekvm::vnc
