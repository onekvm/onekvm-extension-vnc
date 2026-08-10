package vnc

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/jpeg"
	"io"
	"net"
	"sync"
	"time"

	rfb "github.com/amitbet/vnc2video"
	"github.com/rs/zerolog/log"

	"github.com/onekvm/onekvm-core/pkg/av"
	"github.com/onekvm/onekvm-core/pkg/hid"
)

type Config struct {
	ListenAddress  string
	Port           int
	Width          int
	Height         int
	Password       string
	Codec          string
	FPS            int
	JPEGEncoder    func() (av.VideoEncoder, error)
	JPEGInput      bool
	OnDemandChange func(bool)
}

type h264Frame struct {
	seq      uint64
	payload  []byte
	keyframe bool
}

// Sink serves raw source frames through RFB and routes RFB input to OneKVM HID.
type Sink struct {
	mu            sync.RWMutex
	convertMu     sync.Mutex
	jpegMu        sync.Mutex
	cfg           Config
	hid           hid.HIDController
	listener      net.Listener
	clients       map[net.Conn]struct{}
	consumers     int
	rawConsumers  int
	h264Consumers int
	jpegConsumers int
	jpegEncoder   av.VideoEncoder
	jpegFrame     []byte
	jpegSeq       uint64
	jpegReady     chan struct{}
	lastJPEGAt    time.Time
	rawFrame      *av.VideoFrame
	frame         *image.RGBA
	frameSeq      uint64
	convertedSeq  uint64
	lastFrameAt   time.Time
	h264Frames    []h264Frame
	h264Bytes     int
	h264Seq       uint64
	h264Ready     bool
	h264Bootstrap [][]byte
	videoWidth    int
	videoHeight   int
	cancel        context.CancelFunc
	wg            sync.WaitGroup
	started       bool
}

const (
	vncFrameInterval                    = time.Second / 2
	h264EncodingType   rfb.EncodingType = 0x48323634
	maxH264Frames                       = 240
	maxH264BufferBytes                  = 8 << 20
)

type clientEncoding uint8

const (
	clientEncodingNone clientEncoding = iota
	clientEncodingRaw
	clientEncodingJPEG
	clientEncodingH264
)

type clientFramebufferState struct {
	mu                  sync.Mutex
	sink                *Sink
	client              *rfb.ServerConn
	input               *inputState
	width               uint16
	height              uint16
	supportsDesktopSize bool
	encoding            clientEncoding
	jpegCursor          uint64
	h264Cursor          uint64
}

type continuousUpdateRequest struct {
	Enabled bool
	X       uint16
	Y       uint16
	Width   uint16
	Height  uint16
}

type continuousUpdater struct {
	mu     sync.Mutex
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func NewSink(cfg Config, controller hid.HIDController) *Sink {
	if cfg.Width <= 0 || cfg.Height <= 0 {
		cfg.Width, cfg.Height = 1920, 1080
	}
	return &Sink{
		cfg:         cfg,
		hid:         controller,
		clients:     make(map[net.Conn]struct{}),
		jpegReady:   make(chan struct{}),
		videoWidth:  cfg.Width,
		videoHeight: cfg.Height,
	}
}

func (s *Sink) Start() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.started {
		return nil
	}
	if err := ValidateConfig(s.cfg); err != nil {
		return err
	}
	listener, err := net.Listen("tcp", net.JoinHostPort(s.cfg.ListenAddress, fmt.Sprint(s.cfg.Port)))
	if err != nil {
		return fmt.Errorf("listen VNC: %w", err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	s.listener = listener
	s.cancel = cancel
	s.started = true
	s.wg.Add(1)
	go s.acceptLoop(ctx, listener)
	return nil
}

func ValidateConfig(cfg Config) error {
	if cfg.Port < 1 || cfg.Port > 65535 {
		return fmt.Errorf("VNC port must be between 1 and 65535")
	}
	if len(cfg.Password) > 8 {
		return fmt.Errorf("VNC password must not exceed 8 bytes")
	}
	return nil
}

func (s *Sink) Stop() error {
	s.mu.Lock()
	if !s.started {
		s.mu.Unlock()
		return nil
	}
	s.started = false
	cancel := s.cancel
	listener := s.listener
	clients := make([]net.Conn, 0, len(s.clients))
	for client := range s.clients {
		clients = append(clients, client)
	}
	s.listener = nil
	s.cancel = nil
	s.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	if listener != nil {
		_ = listener.Close()
	}
	for _, client := range clients {
		_ = client.Close()
	}
	s.wg.Wait()

	// The NanoKVM vendor JPEG teardown also disrupts the shared VPSS capture
	// path. Keep the encoder warm across client disconnects and release it only
	// when the complete VNC sink stops.
	s.jpegMu.Lock()
	s.mu.Lock()
	encoder := s.jpegEncoder
	s.jpegEncoder = nil
	s.jpegFrame = nil
	s.jpegSeq = 0
	s.lastJPEGAt = time.Time{}
	s.mu.Unlock()
	s.jpegMu.Unlock()
	if encoder != nil {
		return encoder.Close()
	}
	return nil
}

func (s *Sink) WriteVideo(payload []byte, _ float64) error {
	if len(payload) == 0 {
		return nil
	}
	if len(payload) > maxH264BufferBytes {
		return fmt.Errorf("VNC H.264 access unit exceeds %d bytes", maxH264BufferBytes)
	}
	s.mu.RLock()
	enabled := s.cfg.Codec == string(av.CodecH264) && s.h264Consumers > 0
	s.mu.RUnlock()
	if !enabled {
		return nil
	}

	nalTypes := h264NALTypes(payload)
	keyframe := containsNALType(nalTypes, 5)
	parameterSet := containsNALType(nalTypes, 7) || containsNALType(nalTypes, 8)
	cloned := append([]byte(nil), payload...)

	s.mu.Lock()
	defer s.mu.Unlock()
	if parameterSet && !keyframe {
		s.h264Bootstrap = append(s.h264Bootstrap, cloned)
		if len(s.h264Bootstrap) > 4 {
			s.h264Bootstrap = s.h264Bootstrap[len(s.h264Bootstrap)-4:]
		}
		return nil
	}
	if keyframe {
		s.h264Frames = s.h264Frames[:0]
		s.h264Bytes = 0
		for _, bootstrap := range s.h264Bootstrap {
			s.appendH264FrameLocked(bootstrap, true)
		}
		s.h264Bootstrap = nil
		s.h264Ready = true
	}
	if !s.h264Ready {
		return nil
	}
	s.appendH264FrameLocked(cloned, keyframe)
	return nil
}

func (*Sink) WriteAudio([]byte, float64) error { return nil }

func (*Sink) OnControlMessage(func(av.ControlMsg)) {}

func (s *Sink) HasConsumers() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.started && s.consumers > 0
}

func (s *Sink) NeedsEncodedVideo() bool {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.started && s.h264Consumers > 0
}

func (s *Sink) WriteRawVideo(frame *av.VideoFrame) error {
	if frame == nil || frame.Width <= 0 || frame.Height <= 0 {
		return fmt.Errorf("invalid raw video frame dimensions")
	}
	s.mu.Lock()
	s.videoWidth, s.videoHeight = frame.Width, frame.Height
	s.mu.Unlock()
	s.mu.RLock()
	hasRawConsumer := s.started && s.rawConsumers > 0
	hasJPEGConsumer := s.started && s.jpegConsumers > 0
	s.mu.RUnlock()
	if !hasRawConsumer && !hasJPEGConsumer {
		return nil
	}
	if err := validateVideoFrame(frame); err != nil {
		return err
	}

	now := time.Now()
	if hasJPEGConsumer {
		if s.shouldEncodeJPEG(now) {
			if err := s.encodeJPEG(frame); err != nil {
				return err
			}
		}
	}
	if !hasRawConsumer {
		return nil
	}

	s.mu.Lock()
	if !s.lastFrameAt.IsZero() && now.Sub(s.lastFrameAt) < vncFrameInterval {
		s.mu.Unlock()
		return nil
	}
	s.lastFrameAt = now
	s.mu.Unlock()

	// Capture buffers are released after this call. Keep only a throttled copy
	// and defer the expensive RGB conversion until a VNC client requests it.
	cloned := *frame
	cloned.Data = append([]byte(nil), frame.Data...)
	cloned.Release = nil
	s.mu.Lock()
	s.rawFrame = &cloned
	s.frameSeq++
	s.mu.Unlock()
	return nil
}

// WriteJPEGFrame publishes an already hardware-encoded JPEG frame. External
// VNC processes use this path to avoid owning or linking a device encoder.
func (s *Sink) WriteJPEGFrame(payload []byte, width, height int) error {
	if len(payload) == 0 || len(payload) > maxH264BufferBytes {
		return fmt.Errorf("invalid VNC JPEG payload length %d", len(payload))
	}
	if width <= 0 || height <= 0 || width > int(^uint16(0)) || height > int(^uint16(0)) {
		return fmt.Errorf("invalid VNC JPEG dimensions %dx%d", width, height)
	}
	cloned := append([]byte(nil), payload...)
	s.mu.Lock()
	s.videoWidth, s.videoHeight = width, height
	s.jpegFrame = cloned
	s.jpegSeq++
	close(s.jpegReady)
	s.jpegReady = make(chan struct{})
	if s.rawConsumers > 0 {
		s.rawFrame = &av.VideoFrame{Width: width, Height: height, PixelFormat: av.PixelFormatMJPEG, Data: cloned}
		s.frameSeq++
	}
	s.mu.Unlock()
	return nil
}

func (s *Sink) shouldEncodeJPEG(now time.Time) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cfg.FPS >= 60 || s.lastJPEGAt.IsZero() {
		s.lastJPEGAt = now
		return true
	}
	interval := jpegFrameInterval(s.cfg.FPS)
	if now.Sub(s.lastJPEGAt) < interval-interval/10 {
		return false
	}
	s.lastJPEGAt = now
	return true
}

func jpegFrameInterval(fps int) time.Duration {
	if fps <= 0 {
		fps = 60
	}
	if fps > 60 {
		fps = 60
	}
	return time.Second / time.Duration(fps)
}

func (s *Sink) encodeJPEG(frame *av.VideoFrame) error {
	s.jpegMu.Lock()
	defer s.jpegMu.Unlock()
	s.mu.RLock()
	encoder := s.jpegEncoder
	s.mu.RUnlock()
	if encoder == nil {
		return nil
	}
	payload, err := encoder.Encode(frame)
	if err != nil {
		return fmt.Errorf("encode VNC hardware JPEG: %w", err)
	}
	if len(payload) == 0 {
		return nil
	}
	s.mu.Lock()
	s.jpegFrame = append([]byte(nil), payload...)
	s.jpegSeq++
	close(s.jpegReady)
	s.jpegReady = make(chan struct{})
	s.mu.Unlock()
	return nil
}

func (s *Sink) acceptLoop(ctx context.Context, listener net.Listener) {
	defer s.wg.Done()
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			log.Warn().Err(err).Msg("VNC accept failed")
			continue
		}
		s.mu.Lock()
		if !s.started {
			s.mu.Unlock()
			_ = conn.Close()
			return
		}
		s.clients[conn] = struct{}{}
		s.mu.Unlock()
		s.wg.Add(1)
		go s.serveClient(conn)
	}
}

func (s *Sink) serveClient(conn net.Conn) {
	defer s.wg.Done()
	defer func() {
		s.mu.Lock()
		delete(s.clients, conn)
		s.mu.Unlock()
		_ = conn.Close()
	}()
	if err := setTCPNoDelay(conn); err != nil {
		log.Warn().Err(err).Str("remote", conn.RemoteAddr().String()).Msg("disable VNC TCP write buffering failed")
	}
	remote := conn.RemoteAddr().String()
	log.Info().Str("remote", remote).Msg("VNC client connected")
	if err := s.runClient(conn); err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
		log.Warn().Err(err).Str("remote", remote).Msg("VNC client disconnected")
	} else {
		log.Info().Str("remote", remote).Msg("VNC client disconnected")
	}
}

func setTCPNoDelay(conn net.Conn) error {
	tcp, ok := conn.(interface{ SetNoDelay(bool) error })
	if !ok {
		return nil
	}
	return tcp.SetNoDelay(true)
}

func (s *Sink) runClient(conn net.Conn) error {
	s.mu.RLock()
	width, height := s.videoWidth, s.videoHeight
	password := s.cfg.Password
	s.mu.RUnlock()
	security := []rfb.SecurityHandler{&rfb.ServerAuthNone{}}
	if password != "" {
		challenge := make([]byte, 16)
		if _, err := rand.Read(challenge); err != nil {
			return fmt.Errorf("create VNC challenge: %w", err)
		}
		security = []rfb.SecurityHandler{&rfb.ServerAuthVNC{
			Challenge: challenge,
			Password:  []byte(password),
		}}
	}
	cfg := &rfb.ServerConfig{
		Width:            uint16(width),
		Height:           uint16(height),
		DesktopName:      []byte("OneKVM"),
		PixelFormat:      rfb.PixelFormat32bit,
		SecurityHandlers: security,
		Encodings:        []rfb.Encoding{&tightJPEGEncoding{}, &h264Encoding{}, &rawEncoding{}, &desktopSizeEncoding{}, &continuousUpdatesEncoding{}},
		Messages:         rfb.DefaultClientMessages,
	}
	client, err := rfb.NewServerConn(conn, cfg)
	if err != nil {
		return fmt.Errorf("create VNC session: %w", err)
	}
	for _, handler := range rfb.DefaultServerHandlers[:4] {
		if err := handler.Handle(client); err != nil {
			return fmt.Errorf("VNC handshake: %w", err)
		}
	}

	s.mu.Lock()
	firstConsumer := s.consumers == 0
	s.consumers++
	s.rawConsumers++
	demandChange := s.cfg.OnDemandChange
	s.mu.Unlock()
	if firstConsumer && demandChange != nil {
		demandChange(true)
	}
	input := newInputState(s.hid, uint16(width), uint16(height))
	state := &clientFramebufferState{
		sink: s, client: client, input: input,
		width: uint16(width), height: uint16(height), encoding: clientEncodingRaw,
	}
	var updater continuousUpdater
	defer func() {
		updater.Stop()
		state.Close()
		s.mu.Lock()
		s.consumers--
		lastConsumer := s.consumers == 0
		demandChange := s.cfg.OnDemandChange
		s.mu.Unlock()
		if lastConsumer && demandChange != nil {
			demandChange(false)
		}
	}()

	defer func() { input.release() }()
	for {
		var messageType rfb.ClientMessageType
		if err := binary.Read(client, binary.BigEndian, &messageType); err != nil {
			return err
		}
		var message rfb.ClientMessage
		switch messageType {
		case rfb.SetPixelFormatMsgType:
			message = &rfb.SetPixelFormat{}
		case rfb.SetEncodingsMsgType:
			message = &rfb.SetEncodings{}
		case rfb.FramebufferUpdateRequestMsgType:
			message = &rfb.FramebufferUpdateRequest{}
		case rfb.KeyEventMsgType:
			message = &rfb.KeyEvent{}
		case rfb.PointerEventMsgType:
			message = &rfb.PointerEvent{}
		case rfb.ClientCutTextMsgType:
			message = &rfb.ClientCutText{}
		case 150: // EnableContinuousUpdates
			request, err := readContinuousUpdateRequest(client)
			if err != nil {
				return fmt.Errorf("read VNC continuous update request: %w", err)
			}
			if request.Enabled {
				updater.Start(state, request)
			} else {
				updater.Stop()
			}
			continue
		default:
			handled, err := discardClientExtension(client, messageType)
			if err != nil {
				return err
			}
			if handled {
				continue
			}
			return fmt.Errorf("unsupported VNC client message type %d", messageType)
		}
		parsed, err := message.Read(client)
		if err != nil {
			return fmt.Errorf("read VNC client message %d: %w", messageType, err)
		}
		switch value := parsed.(type) {
		case *rfb.SetPixelFormat:
			if value.PF.TrueColor == 0 || (value.PF.BPP != 16 && value.PF.BPP != 32) {
				return fmt.Errorf("unsupported VNC pixel format: %s", value.PF.String())
			}
			if err := client.SetPixelFormat(value.PF); err != nil {
				return fmt.Errorf("set VNC pixel format: %w", err)
			}
		case *rfb.SetEncodings:
			supportsDesktopSize := false
			supportsH264 := false
			supportsTight := false
			for _, encoding := range value.Encodings {
				if encoding == rfb.EncDesktopSizePseudo {
					supportsDesktopSize = true
				}
				if encoding == h264EncodingType {
					supportsH264 = true
				}
				if encoding == rfb.EncTight {
					supportsTight = true
				}
			}
			desired := clientEncodingRaw
			if supportsTight && (s.cfg.JPEGInput || s.cfg.JPEGEncoder != nil) {
				desired = clientEncodingJPEG
			} else if supportsH264 && s.cfg.Codec == string(av.CodecH264) {
				desired = clientEncodingH264
			}
			if err := state.SetEncoding(desired, supportsDesktopSize); err != nil {
				log.Warn().Err(err).Str("remote", conn.RemoteAddr().String()).Msg("VNC hardware JPEG unavailable, using fallback encoding")
				desired = clientEncodingRaw
				if supportsH264 && s.cfg.Codec == string(av.CodecH264) {
					desired = clientEncodingH264
				}
				if fallbackErr := state.SetEncoding(desired, supportsDesktopSize); fallbackErr != nil {
					return fallbackErr
				}
			}
			log.Info().Str("remote", conn.RemoteAddr().String()).Str("encoding", desired.String()).Msg("VNC encoding selected")
		case *rfb.FramebufferUpdateRequest:
			if err := state.Write(value); err != nil {
				return fmt.Errorf("write VNC framebuffer: %w", err)
			}
		case *rfb.KeyEvent:
			input.key(value)
		case *rfb.PointerEvent:
			input.pointer(value)
		}
	}
}

func (s *Sink) writeFramebuffer(client *rfb.ServerConn, request *rfb.FramebufferUpdateRequest, clientWidth, clientHeight uint16, supportsDesktopSize bool, encoding clientEncoding, jpegCursor, h264Cursor *uint64) (bool, error) {
	if encoding == clientEncodingJPEG {
		return s.writeJPEGFramebuffer(client, clientWidth, clientHeight, supportsDesktopSize, jpegCursor)
	}
	if encoding == clientEncodingH264 {
		return s.writeH264Framebuffer(client, clientWidth, clientHeight, supportsDesktopSize, h264Cursor)
	}
	frame, err := s.currentFrame()
	if err != nil {
		return false, err
	}
	if frame == nil {
		return false, nil
	}
	frameWidth, frameHeight := frame.Rect.Dx(), frame.Rect.Dy()
	if supportsDesktopSize && (frameWidth != int(clientWidth) || frameHeight != int(clientHeight)) {
		if frameWidth > int(^uint16(0)) || frameHeight > int(^uint16(0)) {
			return false, fmt.Errorf("VNC framebuffer dimensions exceed protocol limits")
		}
		width, height := uint16(frameWidth), uint16(frameHeight)
		resize := &rfb.Rectangle{
			Width: width, Height: height,
			EncType: rfb.EncDesktopSizePseudo, Enc: &desktopSizeEncoding{},
		}
		if err := (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{resize}}).Write(client); err != nil {
			return false, err
		}
		client.SetWidth(width)
		client.SetHeight(height)
		return true, nil
	}
	bounds := frame.Bounds()
	x0 := min(int(request.X), bounds.Dx())
	y0 := min(int(request.Y), bounds.Dy())
	x1 := min(x0+int(request.Width), bounds.Dx())
	y1 := min(y0+int(request.Height), bounds.Dy())
	if x1 <= x0 || y1 <= y0 {
		return false, (&rfb.FramebufferUpdate{}).Write(client)
	}
	enc := &rawEncoding{frame: frame}
	rect := &rfb.Rectangle{
		X:       uint16(x0),
		Y:       uint16(y0),
		Width:   uint16(x1 - x0),
		Height:  uint16(y1 - y0),
		EncType: rfb.EncRaw,
		Enc:     enc,
	}
	return false, (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{rect}}).Write(client)
}

func (s *clientFramebufferState) SetEncoding(encoding clientEncoding, supportsDesktopSize bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.sink.switchClientEncoding(s.encoding, encoding); err != nil {
		return err
	}
	if encoding != s.encoding {
		s.jpegCursor = 0
		s.h264Cursor = 0
	}
	s.encoding = encoding
	s.supportsDesktopSize = supportsDesktopSize
	return nil
}

func (s *clientFramebufferState) Write(request *rfb.FramebufferUpdateRequest) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	resized, err := s.sink.writeFramebuffer(
		s.client, request, s.width, s.height, s.supportsDesktopSize,
		s.encoding, &s.jpegCursor, &s.h264Cursor,
	)
	if err != nil || !resized {
		return err
	}
	s.width, s.height = s.client.Width(), s.client.Height()
	s.input.resize(s.width, s.height)
	return nil
}

func (s *clientFramebufferState) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	_ = s.sink.switchClientEncoding(s.encoding, clientEncodingNone)
	s.encoding = clientEncodingNone
}

func readContinuousUpdateRequest(reader io.Reader) (continuousUpdateRequest, error) {
	var payload [9]byte
	if _, err := io.ReadFull(reader, payload[:]); err != nil {
		return continuousUpdateRequest{}, err
	}
	return continuousUpdateRequest{
		Enabled: payload[0] != 0,
		X:       binary.BigEndian.Uint16(payload[1:3]),
		Y:       binary.BigEndian.Uint16(payload[3:5]),
		Width:   binary.BigEndian.Uint16(payload[5:7]),
		Height:  binary.BigEndian.Uint16(payload[7:9]),
	}, nil
}

func (u *continuousUpdater) Start(state *clientFramebufferState, request continuousUpdateRequest) {
	u.Stop()
	ctx, cancel := context.WithCancel(context.Background())
	u.mu.Lock()
	u.cancel = cancel
	u.wg.Add(1)
	u.mu.Unlock()
	go func() {
		defer u.wg.Done()
		fps := state.sink.cfg.FPS
		if fps <= 0 {
			fps = 60
		}
		fps = min(fps, 60)
		interval := time.Second / time.Duration(fps)
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		framebufferRequest := &rfb.FramebufferUpdateRequest{
			Inc: 1, X: request.X, Y: request.Y, Width: request.Width, Height: request.Height,
		}
		for {
			if err := state.Write(framebufferRequest); err != nil {
				_ = state.client.Close()
				return
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
}

func (u *continuousUpdater) Stop() {
	u.mu.Lock()
	cancel := u.cancel
	u.cancel = nil
	u.mu.Unlock()
	if cancel != nil {
		cancel()
		u.wg.Wait()
	}
}

func (s *Sink) writeJPEGFramebuffer(client *rfb.ServerConn, clientWidth, clientHeight uint16, supportsDesktopSize bool, cursor *uint64) (bool, error) {
	payload, seq, width, height, ready := s.jpegSnapshot()
	if seq == 0 || seq <= *cursor {
		timer := time.NewTimer(max(2*jpegFrameInterval(s.cfg.FPS), 50*time.Millisecond))
		select {
		case <-ready:
			timer.Stop()
		case <-timer.C:
		}
		payload, seq, width, height, _ = s.jpegSnapshot()
	}
	if supportsDesktopSize && (width != int(clientWidth) || height != int(clientHeight)) {
		return s.writeDesktopSize(client, width, height)
	}
	if seq == 0 || seq <= *cursor {
		return false, (&rfb.FramebufferUpdate{}).Write(client)
	}
	if width > int(^uint16(0)) || height > int(^uint16(0)) {
		return false, fmt.Errorf("VNC framebuffer dimensions exceed protocol limits")
	}
	rect := &rfb.Rectangle{
		Width: uint16(width), Height: uint16(height),
		EncType: rfb.EncTight, Enc: &tightJPEGEncoding{payload: payload},
	}
	if err := (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{rect}}).Write(client); err != nil {
		return false, err
	}
	*cursor = seq
	return false, nil
}

func (s *Sink) jpegSnapshot() ([]byte, uint64, int, int, <-chan struct{}) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.jpegFrame, s.jpegSeq, s.videoWidth, s.videoHeight, s.jpegReady
}

func (s *Sink) writeDesktopSize(client *rfb.ServerConn, width, height int) (bool, error) {
	if width > int(^uint16(0)) || height > int(^uint16(0)) {
		return false, fmt.Errorf("VNC framebuffer dimensions exceed protocol limits")
	}
	resize := &rfb.Rectangle{
		Width: uint16(width), Height: uint16(height),
		EncType: rfb.EncDesktopSizePseudo, Enc: &desktopSizeEncoding{},
	}
	if err := (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{resize}}).Write(client); err != nil {
		return false, err
	}
	client.SetWidth(uint16(width))
	client.SetHeight(uint16(height))
	return true, nil
}

func (s *Sink) writeH264Framebuffer(client *rfb.ServerConn, clientWidth, clientHeight uint16, supportsDesktopSize bool, cursor *uint64) (bool, error) {
	s.mu.RLock()
	var frame h264Frame
	for _, candidate := range s.h264Frames {
		if candidate.seq > *cursor {
			frame = candidate
			break
		}
	}
	width, height := s.videoWidth, s.videoHeight
	s.mu.RUnlock()
	if supportsDesktopSize && (width != int(clientWidth) || height != int(clientHeight)) {
		if width > int(^uint16(0)) || height > int(^uint16(0)) {
			return false, fmt.Errorf("VNC framebuffer dimensions exceed protocol limits")
		}
		resize := &rfb.Rectangle{
			Width: uint16(width), Height: uint16(height),
			EncType: rfb.EncDesktopSizePseudo, Enc: &desktopSizeEncoding{},
		}
		if err := (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{resize}}).Write(client); err != nil {
			return false, err
		}
		client.SetWidth(uint16(width))
		client.SetHeight(uint16(height))
		return true, nil
	}
	if frame.seq == 0 {
		return false, (&rfb.FramebufferUpdate{}).Write(client)
	}
	if width > int(^uint16(0)) || height > int(^uint16(0)) {
		return false, fmt.Errorf("VNC framebuffer dimensions exceed protocol limits")
	}
	rect := &rfb.Rectangle{
		Width: uint16(width), Height: uint16(height),
		EncType: h264EncodingType,
		Enc:     &h264Encoding{payload: frame.payload, keyframe: frame.keyframe, width: uint32(width), height: uint32(height)},
	}
	if err := (&rfb.FramebufferUpdate{NumRect: 1, Rects: []*rfb.Rectangle{rect}}).Write(client); err != nil {
		return false, err
	}
	*cursor = frame.seq
	return false, nil
}

func (s *Sink) appendH264FrameLocked(payload []byte, keyframe bool) {
	s.h264Seq++
	s.h264Frames = append(s.h264Frames, h264Frame{seq: s.h264Seq, payload: payload, keyframe: keyframe})
	s.h264Bytes += len(payload)
	if len(s.h264Frames) > maxH264Frames || s.h264Bytes > maxH264BufferBytes {
		s.resetH264BufferLocked()
	}
}

func (s *Sink) resetH264BufferLocked() {
	s.h264Frames = nil
	s.h264Bytes = 0
	s.h264Ready = false
	s.h264Bootstrap = nil
}

func (s *Sink) switchClientEncoding(from, to clientEncoding) error {
	if from == to {
		return nil
	}
	if from == clientEncodingJPEG || to == clientEncodingJPEG {
		s.jpegMu.Lock()
		defer s.jpegMu.Unlock()
	}
	if to == clientEncodingJPEG {
		s.mu.RLock()
		encoder := s.jpegEncoder
		factory := s.cfg.JPEGEncoder
		jpegInput := s.cfg.JPEGInput
		s.mu.RUnlock()
		if encoder == nil && !jpegInput {
			if factory == nil {
				return fmt.Errorf("VNC hardware JPEG encoder is not configured")
			}
			created, err := factory()
			if err != nil {
				return fmt.Errorf("create VNC hardware JPEG encoder: %w", err)
			}
			if created.Codec() != av.CodecMJPEG {
				_ = created.Close()
				return fmt.Errorf("VNC JPEG encoder returned codec %q", created.Codec())
			}
			s.mu.Lock()
			s.jpegEncoder = created
			s.mu.Unlock()
		}
	}

	s.mu.Lock()
	s.decrementEncodingLocked(from)
	s.incrementEncodingLocked(to)
	s.mu.Unlock()
	return nil
}

func (s *Sink) incrementEncodingLocked(encoding clientEncoding) {
	switch encoding {
	case clientEncodingRaw:
		s.rawConsumers++
	case clientEncodingJPEG:
		s.jpegConsumers++
	case clientEncodingH264:
		if s.h264Consumers == 0 {
			s.resetH264BufferLocked()
		}
		s.h264Consumers++
	}
}

func (s *Sink) decrementEncodingLocked(encoding clientEncoding) {
	switch encoding {
	case clientEncodingRaw:
		s.rawConsumers--
	case clientEncodingJPEG:
		s.jpegConsumers--
	case clientEncodingH264:
		s.h264Consumers--
		if s.h264Consumers == 0 {
			s.resetH264BufferLocked()
		}
	}
}

func (encoding clientEncoding) String() string {
	switch encoding {
	case clientEncodingJPEG:
		return "tight-jpeg-hardware"
	case clientEncodingH264:
		return "h264-hardware"
	default:
		return "raw-fallback"
	}
}

func (s *Sink) currentFrame() (*image.RGBA, error) {
	s.convertMu.Lock()
	defer s.convertMu.Unlock()

	s.mu.RLock()
	if s.rawFrame == nil {
		frame := s.frame
		width, height := s.videoWidth, s.videoHeight
		s.mu.RUnlock()
		if frame == nil {
			s.mu.Lock()
			if s.frame == nil {
				s.frame = image.NewRGBA(image.Rect(0, 0, width, height))
			}
			frame = s.frame
			s.mu.Unlock()
		}
		return frame, nil
	}
	if s.convertedSeq == s.frameSeq {
		frame := s.frame
		s.mu.RUnlock()
		return frame, nil
	}
	raw, seq := s.rawFrame, s.frameSeq
	s.mu.RUnlock()

	converted, err := frameToRGBA(raw)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	if seq >= s.convertedSeq {
		s.frame = converted
		s.convertedSeq = seq
	}
	frame := s.frame
	s.mu.Unlock()
	return frame, nil
}

type rawEncoding struct {
	frame *image.RGBA
}

func (*rawEncoding) Type() rfb.EncodingType              { return rfb.EncRaw }
func (*rawEncoding) Supported(rfb.Conn) bool             { return true }
func (*rawEncoding) Reset() error                        { return nil }
func (*rawEncoding) Read(rfb.Conn, *rfb.Rectangle) error { return nil }

func (e *rawEncoding) Write(conn rfb.Conn, rect *rfb.Rectangle) error {
	pf := conn.PixelFormat()
	bytesPerPixel := int(pf.BPP / 8)
	line := make([]byte, int(rect.Width)*bytesPerPixel)
	var order binary.ByteOrder = binary.LittleEndian
	if pf.BigEndian != 0 {
		order = binary.BigEndian
	}
	for y := int(rect.Y); y < int(rect.Y+rect.Height); y++ {
		for x := int(rect.X); x < int(rect.X+rect.Width); x++ {
			colorOffset := e.frame.PixOffset(x, y)
			r := uint32(e.frame.Pix[colorOffset]) * uint32(pf.RedMax) / 255
			g := uint32(e.frame.Pix[colorOffset+1]) * uint32(pf.GreenMax) / 255
			b := uint32(e.frame.Pix[colorOffset+2]) * uint32(pf.BlueMax) / 255
			pixel := r<<pf.RedShift | g<<pf.GreenShift | b<<pf.BlueShift
			offset := (x - int(rect.X)) * bytesPerPixel
			if pf.BPP == 16 {
				order.PutUint16(line[offset:], uint16(pixel))
			} else {
				order.PutUint32(line[offset:], pixel)
			}
		}
		if _, err := conn.Write(line); err != nil {
			return err
		}
	}
	return nil
}

type desktopSizeEncoding struct{}

func (*desktopSizeEncoding) Type() rfb.EncodingType               { return rfb.EncDesktopSizePseudo }
func (*desktopSizeEncoding) Supported(rfb.Conn) bool              { return true }
func (*desktopSizeEncoding) Reset() error                         { return nil }
func (*desktopSizeEncoding) Read(rfb.Conn, *rfb.Rectangle) error  { return nil }
func (*desktopSizeEncoding) Write(rfb.Conn, *rfb.Rectangle) error { return nil }

type continuousUpdatesEncoding struct{}

func (*continuousUpdatesEncoding) Type() rfb.EncodingType               { return rfb.EncContinuousUpdatesPseudo }
func (*continuousUpdatesEncoding) Supported(rfb.Conn) bool              { return true }
func (*continuousUpdatesEncoding) Reset() error                         { return nil }
func (*continuousUpdatesEncoding) Read(rfb.Conn, *rfb.Rectangle) error  { return nil }
func (*continuousUpdatesEncoding) Write(rfb.Conn, *rfb.Rectangle) error { return nil }

type tightJPEGEncoding struct {
	payload []byte
}

func (*tightJPEGEncoding) Type() rfb.EncodingType              { return rfb.EncTight }
func (*tightJPEGEncoding) Supported(rfb.Conn) bool             { return true }
func (*tightJPEGEncoding) Reset() error                        { return nil }
func (*tightJPEGEncoding) Read(rfb.Conn, *rfb.Rectangle) error { return nil }
func (e *tightJPEGEncoding) Write(conn rfb.Conn, _ *rfb.Rectangle) error {
	if len(e.payload) > 0x3fffff {
		return fmt.Errorf("Tight JPEG payload exceeds protocol limit: %d", len(e.payload))
	}
	if _, err := conn.Write([]byte{0x90}); err != nil {
		return err
	}
	if _, err := conn.Write(tightLength(len(e.payload))); err != nil {
		return err
	}
	_, err := conn.Write(e.payload)
	return err
}

func tightLength(length int) []byte {
	result := []byte{byte(length & 0x7f)}
	if length > 0x7f {
		result[0] |= 0x80
		result = append(result, byte((length>>7)&0x7f))
		if length > 0x3fff {
			result[1] |= 0x80
			result = append(result, byte((length>>14)&0xff))
		}
	}
	return result
}

type h264Encoding struct {
	payload  []byte
	keyframe bool
	width    uint32
	height   uint32
}

func (*h264Encoding) Type() rfb.EncodingType              { return h264EncodingType }
func (*h264Encoding) Supported(rfb.Conn) bool             { return true }
func (*h264Encoding) Reset() error                        { return nil }
func (*h264Encoding) Read(rfb.Conn, *rfb.Rectangle) error { return nil }
func (e *h264Encoding) Write(conn rfb.Conn, _ *rfb.Rectangle) error {
	header := make([]byte, 16)
	binary.BigEndian.PutUint32(header[0:4], uint32(len(e.payload)))
	if e.keyframe {
		binary.BigEndian.PutUint32(header[4:8], 2)
	}
	binary.BigEndian.PutUint32(header[8:12], e.width)
	binary.BigEndian.PutUint32(header[12:16], e.height)
	if _, err := conn.Write(header); err != nil {
		return err
	}
	_, err := conn.Write(e.payload)
	return err
}

func h264NALTypes(payload []byte) []byte {
	var types []byte
	for offset := 0; offset+3 < len(payload); {
		start, length := -1, 0
		for i := offset; i+3 < len(payload); i++ {
			if payload[i] == 0 && payload[i+1] == 0 && payload[i+2] == 1 {
				start, length = i, 3
				break
			}
			if i+4 <= len(payload) && payload[i] == 0 && payload[i+1] == 0 && payload[i+2] == 0 && payload[i+3] == 1 {
				start, length = i, 4
				break
			}
		}
		if start < 0 || start+length >= len(payload) {
			break
		}
		types = append(types, payload[start+length]&0x1f)
		offset = start + length + 1
	}
	if len(types) == 0 && len(payload) != 0 {
		types = append(types, payload[0]&0x1f)
	}
	return types
}

func containsNALType(types []byte, target byte) bool {
	for _, value := range types {
		if value == target {
			return true
		}
	}
	return false
}

func discardClientExtension(reader io.Reader, messageType rfb.ClientMessageType) (bool, error) {
	switch messageType {
	case 248: // ClientFence
		header := make([]byte, 8)
		if _, err := io.ReadFull(reader, header); err != nil {
			return true, err
		}
		_, err := io.CopyN(io.Discard, reader, int64(header[7]))
		return true, err
	case 251: // SetDesktopSize
		header := make([]byte, 7)
		if _, err := io.ReadFull(reader, header); err != nil {
			return true, err
		}
		_, err := io.CopyN(io.Discard, reader, int64(header[5])*16)
		return true, err
	case 255: // QEMU extended key event
		_, err := io.CopyN(io.Discard, reader, 11)
		return true, err
	default:
		return false, nil
	}
}

func validateVideoFrame(frame *av.VideoFrame) error {
	if frame == nil || frame.Width <= 0 || frame.Height <= 0 {
		return fmt.Errorf("invalid raw video frame dimensions")
	}
	if frame.PixelFormat == av.PixelFormatMJPEG {
		if len(frame.Data) == 0 {
			return fmt.Errorf("empty MJPEG frame")
		}
		return nil
	}
	width, height := frame.Width, frame.Height
	ySize := width * height
	chromaWidth := (width + 1) / 2
	chromaHeight := (height + 1) / 2
	required := ySize + width*chromaHeight
	if frame.PixelFormat == av.PixelFormatYUV420P {
		required = ySize + 2*chromaWidth*chromaHeight
	} else if frame.PixelFormat != av.PixelFormatNV12 && frame.PixelFormat != av.PixelFormatNV21 {
		return fmt.Errorf("VNC does not support raw pixel format %q", frame.PixelFormat)
	}
	if len(frame.Data) < required {
		return fmt.Errorf("raw %s frame is truncated", frame.PixelFormat)
	}
	return nil
}

func frameToRGBA(frame *av.VideoFrame) (*image.RGBA, error) {
	if err := validateVideoFrame(frame); err != nil {
		return nil, err
	}
	if frame.PixelFormat == av.PixelFormatMJPEG {
		decoded, err := jpeg.Decode(bytes.NewReader(frame.Data))
		if err != nil {
			return nil, fmt.Errorf("decode MJPEG for VNC: %w", err)
		}
		rgba := image.NewRGBA(decoded.Bounds())
		draw.Draw(rgba, rgba.Bounds(), decoded, decoded.Bounds().Min, draw.Src)
		return rgba, nil
	}

	width, height := frame.Width, frame.Height
	ySize := width * height
	chromaWidth := (width + 1) / 2
	chromaHeight := (height + 1) / 2
	rgba := image.NewRGBA(image.Rect(0, 0, width, height))
	for y := 0; y < height; y++ {
		for x := 0; x < width; x++ {
			yv := frame.Data[y*width+x]
			var cb, cr byte
			switch frame.PixelFormat {
			case av.PixelFormatYUV420P:
				chromaOffset := (y/2)*chromaWidth + x/2
				cb = frame.Data[ySize+chromaOffset]
				cr = frame.Data[ySize+chromaWidth*chromaHeight+chromaOffset]
			case av.PixelFormatNV12, av.PixelFormatNV21:
				chromaOffset := ySize + (y/2)*width + (x/2)*2
				if frame.PixelFormat == av.PixelFormatNV12 {
					cb, cr = frame.Data[chromaOffset], frame.Data[chromaOffset+1]
				} else {
					cr, cb = frame.Data[chromaOffset], frame.Data[chromaOffset+1]
				}
			default:
				return nil, fmt.Errorf("VNC does not support raw pixel format %q", frame.PixelFormat)
			}
			r, g, b := color.YCbCrToRGB(yv, cb, cr)
			offset := rgba.PixOffset(x, y)
			rgba.Pix[offset], rgba.Pix[offset+1], rgba.Pix[offset+2], rgba.Pix[offset+3] = r, g, b, 255
		}
	}
	return rgba, nil
}
