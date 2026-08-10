#include "keymap.hpp"
#include "scancodes.hpp"

#include <rfb/keysym.h>

#include <cassert>

int main() {
  assert(ikvm::keyToScancode(XK_a) == USBHID_KEY_A);
  assert(ikvm::keyToScancode(XK_Z) == USBHID_KEY_Z);
  assert(ikvm::keyToScancode(XK_Return) == USBHID_KEY_RETURN);
  assert(ikvm::keyToScancode(XK_F12) == USBHID_KEY_F12);
  assert(ikvm::keyToMod(XK_Control_L) == USBHID_MOD_CTRL_L);
  assert(ikvm::keyToMod(XK_Super_R) == USBHID_MOD_META_R);
  return 0;
}
