package vnc

import (
	"bytes"
	"encoding/binary"
	"io"
	"net"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	rfb "github.com/amitbet/vnc2video"

	"github.com/onekvm/onekvm-core/pkg/av"
	"github.com/onekvm/onekvm-core/pkg/hid"
)

type recordingController struct {
	keyboard hid.KeyboardReport
	abs      hid.AbsMouseReport
	mouse    hid.MouseReport
}

type jpegEncoderStub struct {
	payload []byte
	encodes atomic.Int32
	closed  atomic.Bool
}

type noDelayConn struct {
	net.Conn
	enabled bool
}

func (c *noDelayConn) SetNoDelay(enabled bool) error {
	c.enabled = enabled
	return nil
}

func TestVNCDisablesTCPWriteBuffering(t *testing.T) {
	server, client := net.Pipe()
	defer server.Close()
	defer client.Close()

	conn := &noDelayConn{Conn: server}
	if err := setTCPNoDelay(conn); err != nil {
		t.Fatalf("set TCP no-delay: %v", err)
	}
	if !conn.enabled {
		t.Fatal("TCP no-delay was not enabled")
	}
}

func (*jpegEncoderStub) Init(av.VideoEncoderConfig) error  { return nil }
func (*jpegEncoderStub) Reset(av.VideoEncoderConfig) error { return nil }
func (e *jpegEncoderStub) Encode(*av.VideoFrame) ([]byte, error) {
	e.encodes.Add(1)
	return e.payload, nil
}
func (*jpegEncoderStub) SetQualityFactor(float64) {}
func (*jpegEncoderStub) SetKeyFrame()             {}
func (*jpegEncoderStub) Codec() av.VideoCodec     { return av.CodecMJPEG }
func (e *jpegEncoderStub) Close() error {
	e.closed.Store(true)
	return nil
}

func (*recordingController) Init(hid.HIDConfig) error { return nil }
func (*recordingController) Close() error             { return nil }
func (c *recordingController) SendKeyboard(report hid.KeyboardReport) error {
	c.keyboard = report
	return nil
}
func (c *recordingController) SendMouse(report hid.MouseReport) error {
	c.mouse = report
	return nil
}
func (c *recordingController) SendAbsoluteMouse(report hid.AbsMouseReport) error {
	c.abs = report
	return nil
}

func TestValidateConfigAllowsRemoteWithoutPassword(t *testing.T) {
	config := Config{ListenAddress: "0.0.0.0", Port: 5900}
	if err := ValidateConfig(config); err != nil {
		t.Fatal(err)
	}
	config.Password = "123456789"
	if err := ValidateConfig(config); err == nil || !strings.Contains(err.Error(), "8 bytes") {
		t.Fatalf("long password error = %v", err)
	}
}

func TestVNCSessionSurvivesFramebufferResize(t *testing.T) {
	port := availableVNCPort(t)
	sink := NewSink(Config{ListenAddress: "127.0.0.1", Port: port, Width: 2, Height: 2}, nil)
	if err := sink.Start(); err != nil {
		t.Fatal(err)
	}
	defer sink.Stop()

	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	completeNoAuthHandshake(t, conn)

	encodings := []byte{2, 0, 0, 2}
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(rfb.EncRaw))
	desktopSize := int32(rfb.EncDesktopSizePseudo)
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(desktopSize))
	if _, err := conn.Write(encodings); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); !sink.HasConsumers(); {
		if time.Now().After(deadline) {
			t.Fatal("VNC client did not become a media consumer")
		}
		time.Sleep(time.Millisecond)
	}
	if err := sink.WriteRawVideo(&av.VideoFrame{
		Width: 4, Height: 2, PixelFormat: av.PixelFormatNV12,
		Data: []byte{80, 90, 100, 110, 120, 130, 140, 150, 128, 128, 128, 128},
	}); err != nil {
		t.Fatal(err)
	}

	writeFramebufferRequest(t, conn, 2, 2)
	width, height, encoding := readFramebufferRectangle(t, conn)
	if width != 4 || height != 2 || encoding != int32(rfb.EncDesktopSizePseudo) {
		t.Fatalf("resize rectangle = %dx%d encoding %d", width, height, encoding)
	}

	writeFramebufferRequest(t, conn, 4, 2)
	width, height, encoding = readFramebufferRectangle(t, conn)
	if width != 4 || height != 2 || encoding != int32(rfb.EncRaw) {
		t.Fatalf("raw rectangle = %dx%d encoding %d", width, height, encoding)
	}
	if _, err := io.ReadFull(conn, make([]byte, int(width)*int(height)*4)); err != nil {
		t.Fatalf("read resized framebuffer: %v", err)
	}
	writeFramebufferRequest(t, conn, 4, 2)
	_, _, _ = readFramebufferRectangle(t, conn)
}

func TestVNCUsesHardwareH264WhenClientAdvertisesEncoding(t *testing.T) {
	port := availableVNCPort(t)
	sink := NewSink(Config{
		ListenAddress: "127.0.0.1", Port: port, Width: 1920, Height: 1080,
		Codec: string(av.CodecH264),
	}, nil)
	if err := sink.Start(); err != nil {
		t.Fatal(err)
	}
	defer sink.Stop()

	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	completeNoAuthHandshake(t, conn)

	encodings := []byte{2, 0, 0, 3}
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(h264EncodingType))
	desktopSize := int32(rfb.EncDesktopSizePseudo)
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(desktopSize))
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(rfb.EncRaw))
	if _, err := conn.Write(encodings); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		h264Consumers, rawConsumers := sink.h264Consumers, sink.rawConsumers
		sink.mu.RUnlock()
		if h264Consumers == 1 && rawConsumers == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("consumer modes = h264:%d raw:%d", h264Consumers, rawConsumers)
		}
		time.Sleep(time.Millisecond)
	}
	if !sink.NeedsEncodedVideo() {
		t.Fatal("H.264 VNC client did not request primary encoder output")
	}

	payload := []byte{
		0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1f,
		0, 0, 0, 1, 0x68, 0xce, 0x06, 0xe2,
		0, 0, 0, 1, 0x65, 0x88, 0x84,
	}
	if err := sink.WriteRawVideo(&av.VideoFrame{Width: 4, Height: 2}); err != nil {
		t.Fatal(err)
	}
	if err := sink.WriteVideo(payload, 1.0/60); err != nil {
		t.Fatal(err)
	}
	writeFramebufferRequest(t, conn, 1920, 1080)
	width, height, encoding := readFramebufferRectangle(t, conn)
	if width != 4 || height != 2 || encoding != int32(rfb.EncDesktopSizePseudo) {
		t.Fatalf("resize rectangle = %dx%d encoding %#x", width, height, encoding)
	}
	writeFramebufferRequest(t, conn, 4, 2)
	width, height, encoding = readFramebufferRectangle(t, conn)
	if width != 4 || height != 2 || encoding != int32(h264EncodingType) {
		t.Fatalf("H.264 rectangle = %dx%d encoding %#x", width, height, encoding)
	}
	header := make([]byte, 16)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatal(err)
	}
	if got := binary.BigEndian.Uint32(header[0:4]); got != uint32(len(payload)) {
		t.Fatalf("H.264 payload size = %d", got)
	}
	if got := binary.BigEndian.Uint32(header[4:8]); got != 2 {
		t.Fatalf("H.264 slice type = %d", got)
	}
	if got := binary.BigEndian.Uint32(header[8:12]); got != 4 {
		t.Fatalf("H.264 width = %d", got)
	}
	data := make([]byte, len(payload))
	if _, err := io.ReadFull(conn, data); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, payload) {
		t.Fatalf("H.264 payload = %#v", data)
	}
	if sink.frame != nil || sink.rawFrame != nil {
		t.Fatal("H.264 client caused a raw framebuffer allocation")
	}
	if err := conn.Close(); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		consumers, frames, bytes := sink.h264Consumers, len(sink.h264Frames), sink.h264Bytes
		sink.mu.RUnlock()
		if consumers == 0 && frames == 0 && bytes == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("H.264 state after disconnect = consumers:%d frames:%d bytes:%d", consumers, frames, bytes)
		}
		time.Sleep(time.Millisecond)
	}
}

func TestVNCRetainsHardwareJPEGAcrossClientReconnects(t *testing.T) {
	port := availableVNCPort(t)
	jpegPayload := []byte{0xff, 0xd8, 1, 2, 3, 0xff, 0xd9}
	encoder := &jpegEncoderStub{payload: jpegPayload}
	var factoryCalls atomic.Int32
	sink := NewSink(Config{
		ListenAddress: "127.0.0.1", Port: port, Width: 2, Height: 2,
		Codec: string(av.CodecH264), FPS: 60,
		JPEGEncoder: func() (av.VideoEncoder, error) {
			factoryCalls.Add(1)
			return encoder, nil
		},
	}, nil)
	if err := sink.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { _ = sink.Stop() }()

	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	completeNoAuthHandshake(t, conn)

	encodings := []byte{2, 0, 0, 3}
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(rfb.EncTight))
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(h264EncodingType))
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(rfb.EncRaw))
	if _, err := conn.Write(encodings); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		jpegConsumers, h264Consumers, rawConsumers := sink.jpegConsumers, sink.h264Consumers, sink.rawConsumers
		sink.mu.RUnlock()
		if jpegConsumers == 1 && h264Consumers == 0 && rawConsumers == 0 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("consumer modes = jpeg:%d h264:%d raw:%d", jpegConsumers, h264Consumers, rawConsumers)
		}
		time.Sleep(time.Millisecond)
	}
	if factoryCalls.Load() != 1 {
		t.Fatalf("JPEG encoder factory calls = %d", factoryCalls.Load())
	}
	if sink.NeedsEncodedVideo() {
		t.Fatal("Tight/JPEG VNC client requested redundant H.264 output")
	}
	writeFramebufferRequest(t, conn, 2, 2)
	if err := conn.SetReadDeadline(time.Now().Add(10 * time.Millisecond)); err != nil {
		t.Fatal(err)
	}
	var firstByte [1]byte
	if _, err := conn.Read(firstByte[:]); err == nil {
		t.Fatal("VNC returned an empty update instead of waiting for the next JPEG frame")
	} else if netErr, ok := err.(net.Error); !ok || !netErr.Timeout() {
		t.Fatalf("wait for JPEG frame: %v", err)
	}
	if err := conn.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatal(err)
	}
	if err := sink.WriteRawVideo(&av.VideoFrame{
		Width: 2, Height: 2, PixelFormat: av.PixelFormatNV21,
		Data: []byte{80, 90, 100, 110, 128, 128},
	}); err != nil {
		t.Fatal(err)
	}
	width, height, encoding := readFramebufferRectangle(t, conn)
	if width != 2 || height != 2 || encoding != int32(rfb.EncTight) {
		t.Fatalf("JPEG rectangle = %dx%d encoding %d", width, height, encoding)
	}
	control := make([]byte, 1)
	if _, err := io.ReadFull(conn, control); err != nil {
		t.Fatal(err)
	}
	if control[0] != 0x90 {
		t.Fatalf("Tight JPEG control = %#x", control[0])
	}
	length := readTightLength(t, conn)
	data := make([]byte, length)
	if _, err := io.ReadFull(conn, data); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, jpegPayload) {
		t.Fatalf("JPEG payload = %#v", data)
	}
	if encoder.encodes.Load() != 1 {
		t.Fatalf("JPEG encode calls = %d", encoder.encodes.Load())
	}
	if err := sink.WriteRawVideo(&av.VideoFrame{
		Width: 2, Height: 2, PixelFormat: av.PixelFormatNV21,
		Data: []byte{80, 90, 100, 110, 128, 128},
	}); err != nil {
		t.Fatal(err)
	}
	if encoder.encodes.Load() != 2 {
		t.Fatalf("60 FPS path throttled consecutive capture frames: %d encodes", encoder.encodes.Load())
	}

	if err := conn.Close(); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		consumers, payload := sink.jpegConsumers, sink.jpegFrame
		sink.mu.RUnlock()
		if consumers == 0 {
			if payload == nil || encoder.closed.Load() {
				t.Fatalf("JPEG encoder was released after disconnect: payload:%d closed:%v", len(payload), encoder.closed.Load())
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("JPEG state after disconnect = consumers:%d payload:%d closed:%v", consumers, len(payload), encoder.closed.Load())
		}
		time.Sleep(time.Millisecond)
	}

	second, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_ = second.SetDeadline(time.Now().Add(2 * time.Second))
	completeNoAuthHandshake(t, second)
	if _, err := second.Write(encodings); err != nil {
		t.Fatal(err)
	}
	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		consumers := sink.jpegConsumers
		sink.mu.RUnlock()
		if consumers == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("reconnected VNC client did not select JPEG")
		}
		time.Sleep(time.Millisecond)
	}
	if factoryCalls.Load() != 1 {
		t.Fatalf("JPEG encoder was recreated on reconnect: factory calls = %d", factoryCalls.Load())
	}
	if err := second.Close(); err != nil {
		t.Fatal(err)
	}
	if err := sink.Stop(); err != nil {
		t.Fatal(err)
	}
	if !encoder.closed.Load() {
		t.Fatal("JPEG encoder remained open after VNC sink stopped")
	}
}

func TestVNCExternalJPEGUsesDemandAndContinuousUpdates(t *testing.T) {
	port := availableVNCPort(t)
	jpegPayload := []byte{0xff, 0xd8, 1, 2, 3, 0xff, 0xd9}
	demand := make(chan bool, 2)
	sink := NewSink(Config{
		ListenAddress: "127.0.0.1", Port: port, Width: 2, Height: 2, FPS: 60,
		JPEGInput: true, OnDemandChange: func(active bool) { demand <- active },
	}, nil)
	if err := sink.Start(); err != nil {
		t.Fatal(err)
	}
	defer sink.Stop()

	conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
	completeNoAuthHandshake(t, conn)
	select {
	case active := <-demand:
		if !active {
			t.Fatal("first VNC demand transition was inactive")
		}
	case <-time.After(time.Second):
		t.Fatal("VNC connection did not enable media demand")
	}

	encodings := []byte{2, 0, 0, 2}
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(rfb.EncTight))
	continuousEncoding := int32(rfb.EncContinuousUpdatesPseudo)
	encodings = binary.BigEndian.AppendUint32(encodings, uint32(continuousEncoding))
	if _, err := conn.Write(encodings); err != nil {
		t.Fatal(err)
	}
	continuous := []byte{150, 1}
	continuous = binary.BigEndian.AppendUint16(continuous, 0)
	continuous = binary.BigEndian.AppendUint16(continuous, 0)
	continuous = binary.BigEndian.AppendUint16(continuous, 2)
	continuous = binary.BigEndian.AppendUint16(continuous, 2)
	if _, err := conn.Write(continuous); err != nil {
		t.Fatal(err)
	}

	for deadline := time.Now().Add(time.Second); ; {
		sink.mu.RLock()
		consumers := sink.jpegConsumers
		sink.mu.RUnlock()
		if consumers == 1 {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("continuous update client did not select JPEG")
		}
		time.Sleep(time.Millisecond)
	}
	if err := sink.WriteJPEGFrame(jpegPayload, 2, 2); err != nil {
		t.Fatal(err)
	}
	width, height, encoding := readFramebufferRectangle(t, conn)
	if width != 2 || height != 2 || encoding != int32(rfb.EncTight) {
		t.Fatalf("continuous JPEG rectangle = %dx%d encoding %d", width, height, encoding)
	}
	control := make([]byte, 1)
	if _, err := io.ReadFull(conn, control); err != nil {
		t.Fatal(err)
	}
	if control[0] != 0x90 {
		t.Fatalf("continuous Tight JPEG control = %#x", control[0])
	}
	length := readTightLength(t, conn)
	data := make([]byte, length)
	if _, err := io.ReadFull(conn, data); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(data, jpegPayload) {
		t.Fatalf("continuous JPEG payload = %#v", data)
	}
	if err := conn.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case active := <-demand:
		if active {
			t.Fatal("last VNC demand transition remained active")
		}
	case <-time.After(time.Second):
		t.Fatal("VNC disconnect did not disable media demand")
	}
}

func TestJPEGFrameIntervalUsesConfiguredFPS(t *testing.T) {
	if got, want := jpegFrameInterval(60), time.Second/60; got != want {
		t.Fatalf("60 FPS interval = %v, want %v", got, want)
	}
	if got, want := jpegFrameInterval(120), time.Second/60; got != want {
		t.Fatalf("clamped interval = %v, want %v", got, want)
	}
}

func TestDiscardKnownClientExtensions(t *testing.T) {
	tests := []struct {
		messageType rfb.ClientMessageType
		body        []byte
	}{
		{messageType: 248, body: append([]byte{0, 0, 0, 0, 0, 0, 0, 3}, 1, 2, 3)},
		{messageType: 251, body: append([]byte{0, 0, 80, 0, 40, 1, 0}, make([]byte, 16)...)},
		{messageType: 255, body: make([]byte, 11)},
	}
	for _, test := range tests {
		reader := bytes.NewReader(test.body)
		handled, err := discardClientExtension(reader, test.messageType)
		if err != nil || !handled || reader.Len() != 0 {
			t.Fatalf("extension %d: handled=%v remaining=%d err=%v", test.messageType, handled, reader.Len(), err)
		}
	}
}

func TestReadContinuousUpdateRequest(t *testing.T) {
	payload := []byte{1, 0, 10, 0, 20, 2, 128, 1, 224}
	request, err := readContinuousUpdateRequest(bytes.NewReader(payload))
	if err != nil {
		t.Fatal(err)
	}
	if !request.Enabled || request.X != 10 || request.Y != 20 || request.Width != 640 || request.Height != 480 {
		t.Fatalf("continuous update request = %+v", request)
	}
}

func TestVNCSecurityHandshake(t *testing.T) {
	for _, test := range []struct {
		name         string
		password     string
		securityType byte
	}{
		{name: "local none", securityType: byte(rfb.SecTypeNone)},
		{name: "VNC password", password: "secret", securityType: byte(rfb.SecTypeVNC)},
	} {
		t.Run(test.name, func(t *testing.T) {
			port := availableVNCPort(t)
			sink := NewSink(Config{ListenAddress: "127.0.0.1", Port: port, Width: 2, Height: 2, Password: test.password}, nil)
			if err := sink.Start(); err != nil {
				t.Fatal(err)
			}
			defer sink.Stop()

			conn, err := net.DialTimeout("tcp", net.JoinHostPort("127.0.0.1", strconv.Itoa(port)), time.Second)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
			version := make([]byte, 12)
			if _, err := io.ReadFull(conn, version); err != nil {
				t.Fatal(err)
			}
			if string(version) != "RFB 003.008\n" {
				t.Fatalf("version = %q", version)
			}
			if _, err := conn.Write(version); err != nil {
				t.Fatal(err)
			}
			security := make([]byte, 2)
			if _, err := io.ReadFull(conn, security); err != nil {
				t.Fatal(err)
			}
			if security[0] != 1 || security[1] != test.securityType {
				t.Fatalf("security offer = %#v", security)
			}
		})
	}
}

func TestFrameConversionAndInputMapping(t *testing.T) {
	frame, err := frameToRGBA(&av.VideoFrame{
		Width: 2, Height: 2, PixelFormat: av.PixelFormatNV12,
		Data: []byte{80, 100, 120, 140, 128, 128},
	})
	if err != nil {
		t.Fatal(err)
	}
	if frame.Rect.Dx() != 2 || frame.Rect.Dy() != 2 || frame.Pix[3] != 255 {
		t.Fatalf("converted frame = bounds %v first pixel %#v", frame.Rect, frame.Pix[:4])
	}

	controller := &recordingController{}
	input := newInputState(controller, 1920, 1080)
	input.key(&rfb.KeyEvent{Down: 1, Key: rfb.SmallA})
	if len(controller.keyboard.Keys) != 1 || controller.keyboard.Keys[0] != 0x04 {
		t.Fatalf("keyboard report = %+v", controller.keyboard)
	}
	input.pointer(&rfb.PointerEvent{Mask: 0x21, X: 1919, Y: 1079})
	if controller.abs.Buttons != 9 || controller.abs.X != 32767 || controller.abs.Y != 32767 {
		t.Fatalf("absolute mouse report = %+v", controller.abs)
	}
}

func availableVNCPort(t *testing.T) int {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	_ = listener.Close()
	return port
}

func completeNoAuthHandshake(t *testing.T, conn net.Conn) {
	t.Helper()
	version := make([]byte, 12)
	if _, err := io.ReadFull(conn, version); err != nil {
		t.Fatal(err)
	}
	if _, err := conn.Write(version); err != nil {
		t.Fatal(err)
	}
	security := make([]byte, 2)
	if _, err := io.ReadFull(conn, security); err != nil {
		t.Fatal(err)
	}
	if security[0] != 1 || security[1] != byte(rfb.SecTypeNone) {
		t.Fatalf("security offer = %#v", security)
	}
	if _, err := conn.Write([]byte{byte(rfb.SecTypeNone)}); err != nil {
		t.Fatal(err)
	}
	result := make([]byte, 4)
	if _, err := io.ReadFull(conn, result); err != nil {
		t.Fatal(err)
	}
	if binary.BigEndian.Uint32(result) != 0 {
		t.Fatalf("security result = %#v", result)
	}
	if _, err := conn.Write([]byte{1}); err != nil {
		t.Fatal(err)
	}
	serverInit := make([]byte, 24)
	if _, err := io.ReadFull(conn, serverInit); err != nil {
		t.Fatal(err)
	}
	nameLength := binary.BigEndian.Uint32(serverInit[20:])
	if _, err := io.ReadFull(conn, make([]byte, nameLength)); err != nil {
		t.Fatal(err)
	}
}

func writeFramebufferRequest(t *testing.T, conn net.Conn, width, height uint16) {
	t.Helper()
	request := []byte{byte(rfb.FramebufferUpdateRequestMsgType), 0, 0, 0, 0, 0}
	request = binary.BigEndian.AppendUint16(request, width)
	request = binary.BigEndian.AppendUint16(request, height)
	if _, err := conn.Write(request); err != nil {
		t.Fatal(err)
	}
}

func readFramebufferRectangle(t *testing.T, conn net.Conn) (uint16, uint16, int32) {
	t.Helper()
	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatal(err)
	}
	if header[0] != 0 || binary.BigEndian.Uint16(header[2:]) != 1 {
		t.Fatalf("framebuffer update header = %#v", header)
	}
	rectangle := make([]byte, 12)
	if _, err := io.ReadFull(conn, rectangle); err != nil {
		t.Fatal(err)
	}
	return binary.BigEndian.Uint16(rectangle[4:]), binary.BigEndian.Uint16(rectangle[6:]), int32(binary.BigEndian.Uint32(rectangle[8:]))
}

func readTightLength(t *testing.T, reader io.Reader) int {
	t.Helper()
	var length int
	for index, shift := 0, 0; index < 3; index, shift = index+1, shift+7 {
		var value [1]byte
		if _, err := io.ReadFull(reader, value[:]); err != nil {
			t.Fatal(err)
		}
		length |= int(value[0]&0x7f) << shift
		if value[0]&0x80 == 0 || index == 2 {
			return length
		}
	}
	return length
}
