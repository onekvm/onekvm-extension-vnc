#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace onekvm::vnc {

struct TileRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

constexpr int kFramebufferTileSize = 32;

bool DecodeJPEGToRGB565(const std::uint8_t *data, std::size_t size,
                        std::uint16_t *out, int width, int height,
                        std::string &error);

// Compare RGB565 frames in 32x32 tiles. A null previous buffer marks the
// whole frame dirty. If more than 40% of tiles change, one full-frame rect
// is returned so LibVNCServer can send a single update.
inline std::vector<TileRect> CollectDirtyTiles(const std::uint16_t *previous,
                                               const std::uint16_t *current,
                                               int width, int height) {
  std::vector<TileRect> tiles;
  if (current == nullptr || width <= 0 || height <= 0)
    return tiles;
  if (previous == nullptr) {
    tiles.push_back({0, 0, width, height});
    return tiles;
  }

  const int tiles_x = (width + kFramebufferTileSize - 1) / kFramebufferTileSize;
  const int tiles_y =
      (height + kFramebufferTileSize - 1) / kFramebufferTileSize;
  const int total = tiles_x * tiles_y;
  tiles.reserve(static_cast<std::size_t>(total));
  for (int ty = 0; ty < tiles_y; ++ty) {
    const int y = ty * kFramebufferTileSize;
    const int tile_h = std::min(kFramebufferTileSize, height - y);
    for (int tx = 0; tx < tiles_x; ++tx) {
      const int x = tx * kFramebufferTileSize;
      const int tile_w = std::min(kFramebufferTileSize, width - x);
      bool dirty = false;
      for (int row = 0; row < tile_h && !dirty; ++row) {
        const std::size_t offset =
            static_cast<std::size_t>(y + row) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(x);
        if (std::memcmp(previous + offset, current + offset,
                        static_cast<std::size_t>(tile_w) *
                            sizeof(std::uint16_t)) != 0)
          dirty = true;
      }
      if (dirty)
        tiles.push_back({x, y, tile_w, tile_h});
    }
  }
  if (tiles.size() * 5U >= static_cast<std::size_t>(total) * 2U) {
    tiles.clear();
    tiles.push_back({0, 0, width, height});
  }
  return tiles;
}

} // namespace onekvm::vnc
