package vnc

import (
	"sort"
	"sync"

	rfb "github.com/amitbet/vnc2video"

	"github.com/onekvm/onekvm-core/pkg/hid"
)

type inputState struct {
	mu         sync.Mutex
	controller hid.HIDController
	width      uint16
	height     uint16
	modifiers  uint8
	keys       map[rfb.Key]uint8
	buttons    uint16
}

func newInputState(controller hid.HIDController, width, height uint16) *inputState {
	return &inputState{controller: controller, width: width, height: height, keys: make(map[rfb.Key]uint8)}
}

func (s *inputState) key(event *rfb.KeyEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.controller == nil {
		return
	}
	if bit, ok := modifierBit(event.Key); ok {
		if event.Down != 0 {
			s.modifiers |= bit
		} else {
			s.modifiers &^= bit
		}
	} else if usage, ok := usbUsage(event.Key); ok {
		if event.Down != 0 {
			s.keys[event.Key] = usage
		} else {
			delete(s.keys, event.Key)
		}
	}
	keys := make([]int, 0, len(s.keys))
	for _, usage := range s.keys {
		keys = append(keys, int(usage))
	}
	sort.Ints(keys)
	report := hid.KeyboardReport{Modifiers: s.modifiers}
	for _, usage := range keys {
		if len(report.Keys) == 6 {
			break
		}
		report.Keys = append(report.Keys, uint8(usage))
	}
	_ = s.controller.SendKeyboard(report)
}

func (s *inputState) pointer(event *rfb.PointerEvent) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.controller == nil {
		return
	}
	buttons := uint16(event.Mask & 0x07)
	if event.Mask&0x20 != 0 {
		buttons |= 1 << 3
	}
	if event.Mask&0x40 != 0 {
		buttons |= 1 << 4
	}
	s.buttons = buttons
	var x, y uint16
	if s.width > 1 {
		x = uint16(uint32(event.X) * 32767 / uint32(s.width-1))
	}
	if s.height > 1 {
		y = uint16(uint32(event.Y) * 32767 / uint32(s.height-1))
	}
	_ = s.controller.SendAbsoluteMouse(hid.AbsMouseReport{Buttons: buttons, X: x, Y: y})
	wheel := int8(0)
	if event.Mask&0x08 != 0 {
		wheel = 1
	} else if event.Mask&0x10 != 0 {
		wheel = -1
	}
	if wheel != 0 {
		_ = s.controller.SendMouse(hid.MouseReport{Buttons: uint8(buttons), Wheel: wheel})
	}
}

func (s *inputState) release() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.controller == nil {
		return
	}
	if s.modifiers != 0 || len(s.keys) != 0 {
		_ = s.controller.SendKeyboard(hid.KeyboardReport{})
	}
	if s.buttons != 0 {
		_ = s.controller.SendAbsoluteMouse(hid.AbsMouseReport{})
	}
}

func (s *inputState) resize(width, height uint16) {
	s.mu.Lock()
	s.width, s.height = width, height
	s.mu.Unlock()
}

func modifierBit(key rfb.Key) (uint8, bool) {
	switch key {
	case rfb.ControlLeft:
		return 1 << 0, true
	case rfb.ShiftLeft:
		return 1 << 1, true
	case rfb.AltLeft:
		return 1 << 2, true
	case rfb.MetaLeft, rfb.SuperLeft:
		return 1 << 3, true
	case rfb.ControlRight:
		return 1 << 4, true
	case rfb.ShiftRight:
		return 1 << 5, true
	case rfb.AltRight:
		return 1 << 6, true
	case rfb.MetaRight, rfb.SuperRight:
		return 1 << 7, true
	default:
		return 0, false
	}
}

func usbUsage(key rfb.Key) (uint8, bool) {
	if key >= rfb.SmallA && key <= rfb.SmallZ {
		return 0x04 + uint8(key-rfb.SmallA), true
	}
	if key >= rfb.A && key <= rfb.Z {
		return 0x04 + uint8(key-rfb.A), true
	}
	if key >= rfb.Digit1 && key <= rfb.Digit9 {
		return 0x1e + uint8(key-rfb.Digit1), true
	}
	switch key {
	case rfb.Digit0:
		return 0x27, true
	case rfb.Return:
		return 0x28, true
	case rfb.Escape:
		return 0x29, true
	case rfb.BackSpace:
		return 0x2a, true
	case rfb.Tab:
		return 0x2b, true
	case rfb.Space:
		return 0x2c, true
	case rfb.Minus, rfb.Underscore:
		return 0x2d, true
	case rfb.Equal, rfb.Plus:
		return 0x2e, true
	case rfb.BracketLeft, rfb.BraceLeft:
		return 0x2f, true
	case rfb.BracketRight, rfb.BraceRight:
		return 0x30, true
	case rfb.Backslash, rfb.Bar:
		return 0x31, true
	case rfb.Semicolon, rfb.Colon:
		return 0x33, true
	case rfb.Apostrophe, rfb.QuoteDbl:
		return 0x34, true
	case rfb.Grave, rfb.AsciiTilde:
		return 0x35, true
	case rfb.Comma, rfb.Less:
		return 0x36, true
	case rfb.Period, rfb.Greater:
		return 0x37, true
	case rfb.Slash, rfb.Question:
		return 0x38, true
	case rfb.CapsLock:
		return 0x39, true
	case rfb.F1, rfb.F2, rfb.F3, rfb.F4, rfb.F5, rfb.F6, rfb.F7, rfb.F8, rfb.F9, rfb.F10, rfb.F11, rfb.F12:
		return 0x3a + uint8(key-rfb.F1), true
	case rfb.Key(0xff61): // XK_Print
		return 0x46, true
	case rfb.ScrollLock:
		return 0x47, true
	case rfb.Pause:
		return 0x48, true
	case rfb.Key(0xff63): // XK_Insert
		return 0x49, true
	case rfb.Home:
		return 0x4a, true
	case rfb.PageUp:
		return 0x4b, true
	case rfb.Delete:
		return 0x4c, true
	case rfb.End:
		return 0x4d, true
	case rfb.PageDown:
		return 0x4e, true
	case rfb.Right:
		return 0x4f, true
	case rfb.Left:
		return 0x50, true
	case rfb.Down:
		return 0x51, true
	case rfb.Up:
		return 0x52, true
	default:
		return 0, false
	}
}
