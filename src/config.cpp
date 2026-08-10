#include "config.hpp"

#include <arpa/inet.h>
#include <json-c/json.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>

namespace onekvm::vnc {
namespace {

constexpr std::size_t kMaxConfigBytes = 1U << 20;

bool ReadFile(const std::string &path, bool allow_missing, bool &missing,
              std::string &data, std::string &error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (allow_missing && errno == ENOENT) {
      missing = true;
      return true;
    }
    error = "open protocol configuration " + path + ": " + std::strerror(errno);
    return false;
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::size_t>(size) > kMaxConfigBytes) {
    error = "protocol configuration exceeds 1 MiB";
    return false;
  }
  input.seekg(0, std::ios::beg);
  data.assign(std::istreambuf_iterator<char>(input),
              std::istreambuf_iterator<char>());
  if (!input.eof() && input.fail()) {
    error = "read protocol configuration " + path;
    return false;
  }
  return true;
}

bool GetBoolean(json_object *root, const char *name, bool &value,
                std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_boolean)) {
    error = std::string("configuration field ") + name + " must be boolean";
    return false;
  }
  value = json_object_get_boolean(object) != 0;
  return true;
}

bool GetInteger(json_object *root, const char *name, int &value,
                std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_int)) {
    error = std::string("configuration field ") + name + " must be integer";
    return false;
  }
  const auto parsed = json_object_get_int64(object);
  if (parsed < std::numeric_limits<int>::min() ||
      parsed > std::numeric_limits<int>::max()) {
    error = std::string("configuration field ") + name + " is out of range";
    return false;
  }
  value = static_cast<int>(parsed);
  return true;
}

bool GetString(json_object *root, const char *name, std::string &value,
               std::string &error) {
  json_object *object = nullptr;
  if (!json_object_object_get_ex(root, name, &object))
    return true;
  if (!json_object_is_type(object, json_type_string)) {
    error = std::string("configuration field ") + name + " must be string";
    return false;
  }
  value = json_object_get_string(object);
  return true;
}

} // namespace

bool LoadConfig(const std::string &path, bool allow_missing, Config &config,
                std::string &error) {
  std::string data;
  bool missing = false;
  if (!ReadFile(path, allow_missing, missing, data, error))
    return false;
  if (missing)
    return ValidateConfig(config, error);

  json_tokener *tokener = json_tokener_new_ex(32);
  if (tokener == nullptr) {
    error = "allocate JSON parser";
    return false;
  }
  json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
  json_object *root = json_tokener_parse_ex(tokener, data.data(),
                                            static_cast<int>(data.size()));
  const auto parse_error = json_tokener_get_error(tokener);
  auto parse_end = json_tokener_get_parse_end(tokener);
  json_tokener_free(tokener);
  while (parse_end < data.size() &&
         std::isspace(static_cast<unsigned char>(data[parse_end]))) {
    ++parse_end;
  }
  if (parse_error != json_tokener_success || root == nullptr ||
      parse_end != data.size()) {
    if (root != nullptr)
      json_object_put(root);
    error = "decode protocol configuration " + path + ": invalid JSON";
    return false;
  }
  if (!json_object_is_type(root, json_type_object)) {
    json_object_put(root);
    error = "protocol configuration must be an object";
    return false;
  }

  static const std::set<std::string_view> allowed = {
      "service_enabled", "listen_address", "port", "access_control_enabled",
      "password",        "fps",            "jpeg_quality", "media_socket",
      "admin_socket"};
  json_object_object_foreach(root, key, value) {
    (void)value;
    if (!allowed.contains(key)) {
      error = std::string("unknown protocol configuration field ") + key;
      json_object_put(root);
      return false;
    }
  }

  const bool parsed =
      GetBoolean(root, "service_enabled", config.service_enabled, error) &&
      GetString(root, "listen_address", config.listen_address, error) &&
      GetInteger(root, "port", config.port, error) &&
      GetBoolean(root, "access_control_enabled",
                 config.access_control_enabled, error) &&
      GetString(root, "password", config.password, error) &&
      GetInteger(root, "fps", config.fps, error) &&
      GetInteger(root, "jpeg_quality", config.jpeg_quality, error) &&
      GetString(root, "media_socket", config.media_socket, error) &&
      GetString(root, "admin_socket", config.admin_socket, error);
  json_object_put(root);
  return parsed && ValidateConfig(config, error);
}

bool ValidateConfig(Config &config, std::string &error) {
  if (config.port < 1 || config.port > 65535) {
    error = "VNC port must be between 1 and 65535";
    return false;
  }
  if (config.listen_address.empty() || config.listen_address.size() > 64) {
    error = "VNC listen address must contain 1 to 64 characters";
    return false;
  }
  in_addr address4{};
  in6_addr address6{};
  if (inet_pton(AF_INET, config.listen_address.c_str(), &address4) != 1 &&
      inet_pton(AF_INET6, config.listen_address.c_str(), &address6) != 1) {
    error = "VNC listen address must be an IPv4 or IPv6 address";
    return false;
  }
  if (config.password.size() > 8) {
    error = "VNC password must not exceed 8 bytes";
    return false;
  }
  if (config.access_control_enabled && config.password.empty()) {
    error = "VNC password is required when access control is enabled";
    return false;
  }
  if (config.fps < 1 || config.fps > 60) {
    error = "VNC FPS must be between 1 and 60";
    return false;
  }
  if (config.jpeg_quality < 1 || config.jpeg_quality > 100) {
    error = "VNC JPEG quality must be between 1 and 100";
    return false;
  }
  if (!std::filesystem::path(config.media_socket).is_absolute()) {
    error = "VNC media socket path must be absolute";
    return false;
  }
  if (!std::filesystem::path(config.admin_socket).is_absolute()) {
    error = "VNC admin socket path must be absolute";
    return false;
  }
  return true;
}

std::string DefaultConfigPath() {
  const char *root = std::getenv("ONEKVM_EXTENSION_CONFIG");
  return root != nullptr && root[0] != '\0'
             ? std::string(root) + "/config.json"
             : "/run/onekvm/extensions/vnc/config.json";
}

} // namespace onekvm::vnc
