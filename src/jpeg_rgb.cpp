#include "jpeg_rgb.hpp"

#include <jpeglib.h>

#include <csetjmp>

namespace onekvm::vnc {
namespace {

struct JPEGError {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX]{};
};

void JPEGErrorExit(j_common_ptr cinfo) {
  auto *error = reinterpret_cast<JPEGError *>(cinfo->err);
  error->pub.format_message(cinfo, error->message);
  std::longjmp(error->jump, 1);
}

std::uint16_t PackRGB565(std::uint8_t red, std::uint8_t green,
                         std::uint8_t blue) {
  return static_cast<std::uint16_t>(((red >> 3) << 11) | ((green >> 2) << 5) |
                                    (blue >> 3));
}

} // namespace

bool DecodeJPEGToRGB565(const std::uint8_t *data, std::size_t size,
                        std::uint16_t *out, int width, int height,
                        std::string &error) {
  if (data == nullptr || out == nullptr || size == 0 || width <= 0 ||
      height <= 0) {
    error = "invalid JPEG framebuffer";
    return false;
  }

  jpeg_decompress_struct info{};
  JPEGError jpeg_error{};
  info.err = jpeg_std_error(&jpeg_error.pub);
  jpeg_error.pub.error_exit = JPEGErrorExit;
  if (setjmp(jpeg_error.jump)) {
    jpeg_destroy_decompress(&info);
    error = jpeg_error.message[0] == '\0' ? "decode JPEG frame"
                                          : jpeg_error.message;
    return false;
  }

  jpeg_create_decompress(&info);
  jpeg_mem_src(&info, data, static_cast<unsigned long>(size));
  if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&info);
    error = "invalid JPEG header";
    return false;
  }
  info.out_color_space = JCS_RGB;
  jpeg_start_decompress(&info);
  if (static_cast<int>(info.output_width) != width ||
      static_cast<int>(info.output_height) != height ||
      info.output_components != 3) {
    jpeg_destroy_decompress(&info);
    error = "JPEG frame size does not match VNC framebuffer";
    return false;
  }

  std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3U);
  while (info.output_scanline < info.output_height) {
    JSAMPROW rows[] = {row.data()};
    jpeg_read_scanlines(&info, rows, 1);
    std::uint16_t *dst =
        out + static_cast<std::size_t>(info.output_scanline - 1) *
                  static_cast<std::size_t>(width);
    for (int x = 0; x < width; ++x) {
      dst[x] = PackRGB565(row[static_cast<std::size_t>(x) * 3U],
                          row[static_cast<std::size_t>(x) * 3U + 1U],
                          row[static_cast<std::size_t>(x) * 3U + 2U]);
    }
  }
  jpeg_finish_decompress(&info);
  jpeg_destroy_decompress(&info);
  return true;
}

} // namespace onekvm::vnc
