package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"github.com/rs/zerolog/log"

	"github.com/onekvm/onekvm-core/pkg/extensionruntime"
	"github.com/onekvm/onekvm-core/pkg/protocolipc"
	"github.com/onekvm/onekvm-extension-vnc/internal/vnc"
)

const defaultConfigPath = "/run/onekvm/extensions/vnc/config.json"

type config struct {
	ServiceEnabled *bool  `json:"service_enabled"`
	ListenAddress  string `json:"listen_address"`
	Port           int    `json:"port"`
	Password       string `json:"password"`
	FPS            int    `json:"fps"`
	JPEGQuality    int    `json:"jpeg_quality"`
	MediaSocket    string `json:"media_socket"`
	AdminSocket    string `json:"admin_socket"`
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		log.Fatal().Err(err).Msg("VNC protocol process failed")
	}
}

func run(args []string) error {
	flags := flag.NewFlagSet("onekvm-protocol-vnc", flag.ContinueOnError)
	configPath := flags.String("config", extensionConfigPath(defaultConfigPath), "protocol configuration path")
	health := flags.Bool("health", false, "validate configuration and exit")
	lifecycle := flags.String("lifecycle", "", "handle an enable or disable lifecycle notification")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected arguments: %v", flags.Args())
	}
	var cfg config
	if err := extensionruntime.LoadConfig(*configPath, &cfg); err != nil && !(*health && errors.Is(err, os.ErrNotExist)) {
		return err
	}
	applyVNCDefaults(&cfg)
	if err := validateVNCConfig(cfg); err != nil {
		return err
	}
	if *health {
		return nil
	}
	if *lifecycle != "" {
		if *lifecycle != "enable" && *lifecycle != "disable" {
			return fmt.Errorf("unsupported lifecycle action %q", *lifecycle)
		}
		return nil
	}
	if !serviceEnabled(cfg) {
		return nil
	}
	identity, err := extensionruntime.LoadIdentity()
	if err != nil {
		return err
	}

	hidController := extensionruntime.NewAdminHIDController(cfg.AdminSocket, identity)
	defer hidController.Close()
	var subscriber *extensionruntime.MediaSubscriber
	sink := vnc.NewSink(vnc.Config{
		ListenAddress: cfg.ListenAddress,
		Port:          cfg.Port,
		FPS:           cfg.FPS,
		Password:      cfg.Password,
		JPEGInput:     true,
		OnDemandChange: func(active bool) {
			subscriber.SetDemand(active)
		},
	}, hidController)
	subscriber = &extensionruntime.MediaSubscriber{
		SocketPath:    cfg.MediaSocket,
		ExtensionID:   identity.ExtensionID,
		Token:         identity.Token,
		Video:         "mjpeg",
		FPS:           cfg.FPS,
		QualityFactor: float64(cfg.JPEGQuality) / 100,
		Consume: func(frame protocolipc.Frame) error {
			if frame.Codec != "mjpeg" {
				return fmt.Errorf("VNC received unexpected media codec %q", frame.Codec)
			}
			return sink.WriteJPEGFrame(frame.Payload, int(frame.Width), int(frame.Height))
		},
		OnError: func(err error) { log.Warn().Err(err).Msg("VNC media subscription interrupted") },
	}
	if err := sink.Start(); err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	<-ctx.Done()
	if err := sink.Stop(); err != nil {
		return err
	}
	subscriber.Close()
	return nil
}

func serviceEnabled(cfg config) bool {
	return cfg.ServiceEnabled == nil || *cfg.ServiceEnabled
}

func applyVNCDefaults(cfg *config) {
	if cfg.ListenAddress == "" {
		cfg.ListenAddress = "0.0.0.0"
	}
	if cfg.Port == 0 {
		cfg.Port = 5900
	}
	if cfg.FPS <= 0 {
		cfg.FPS = 60
	}
	if cfg.JPEGQuality <= 0 {
		cfg.JPEGQuality = 20
	}
	if cfg.MediaSocket == "" {
		cfg.MediaSocket = protocolipc.DefaultMediaSocket
	}
	if cfg.AdminSocket == "" {
		cfg.AdminSocket = extensionruntime.DefaultControlSocket
	}
}

func extensionConfigPath(fallback string) string {
	if root := os.Getenv("ONEKVM_EXTENSION_CONFIG"); root != "" {
		return filepath.Join(root, "config.json")
	}
	return fallback
}

func validateVNCConfig(cfg config) error {
	if err := vnc.ValidateConfig(vnc.Config{ListenAddress: cfg.ListenAddress, Port: cfg.Port, Password: cfg.Password}); err != nil {
		return err
	}
	if cfg.FPS < 1 || cfg.FPS > 60 {
		return fmt.Errorf("VNC FPS must be between 1 and 60")
	}
	if cfg.JPEGQuality < 1 || cfg.JPEGQuality > 100 {
		return fmt.Errorf("VNC JPEG quality must be between 1 and 100")
	}
	for name, path := range map[string]string{"media socket": cfg.MediaSocket, "admin socket": cfg.AdminSocket} {
		if !filepath.IsAbs(path) {
			return fmt.Errorf("VNC %s path must be absolute", name)
		}
	}
	return nil
}
