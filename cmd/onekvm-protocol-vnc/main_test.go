package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestHealthUsesDefaultsWithoutRuntimeIdentity(t *testing.T) {
	t.Setenv("ONEKVM_EXTENSION_ID", "")
	t.Setenv("CREDENTIALS_DIRECTORY", "")
	missing := filepath.Join(t.TempDir(), "missing.json")
	if err := run([]string{"--health", "--config", missing}); err != nil {
		t.Fatal(err)
	}
}

func TestLifecycleNotification(t *testing.T) {
	configPath := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(configPath, []byte(`{}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"--lifecycle", "disable", "--config", configPath}); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"--lifecycle", "invalid", "--config", configPath}); err == nil {
		t.Fatal("invalid lifecycle action was accepted")
	}
}

func TestServiceEnabledDefaultsToTrue(t *testing.T) {
	if !serviceEnabled(config{}) {
		t.Fatal("missing service_enabled disabled VNC")
	}
	disabled := false
	if serviceEnabled(config{ServiceEnabled: &disabled}) {
		t.Fatal("service_enabled=false enabled VNC")
	}
}

func TestDisabledServiceExits(t *testing.T) {
	configPath := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(configPath, []byte(`{"service_enabled":false}`), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := run([]string{"--config", configPath}); err != nil {
		t.Fatal(err)
	}
}
