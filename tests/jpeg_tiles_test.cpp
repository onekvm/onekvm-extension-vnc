#include "jpeg_rgb.hpp"

#include <cstdint>
#include <vector>

int main() {
  constexpr int width = 64;
  constexpr int height = 64;
  std::vector<std::uint16_t> previous(width * height, 0);
  std::vector<std::uint16_t> current = previous;
  auto empty = onekvm::vnc::CollectDirtyTiles(nullptr, current.data(), width,
                                              height);
  if (empty.size() != 1 || empty[0].width != width || empty[0].height != height)
    return 1;

  auto none = onekvm::vnc::CollectDirtyTiles(previous.data(), current.data(),
                                             width, height);
  if (!none.empty())
    return 2;

  current[0] = 0xffff;
  auto one = onekvm::vnc::CollectDirtyTiles(previous.data(), current.data(),
                                            width, height);
  if (one.size() != 1 || one[0].x != 0 || one[0].y != 0 ||
      one[0].width != onekvm::vnc::kFramebufferTileSize)
    return 3;

  current.assign(width * height, 0xffff);
  auto many = onekvm::vnc::CollectDirtyTiles(previous.data(), current.data(),
                                             width, height);
  if (many.size() != 1 || many[0].width != width || many[0].height != height)
    return 4;
  return 0;
}
