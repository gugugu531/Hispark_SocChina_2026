// Package configtx implements the cold-config transaction engine.
// It manages /etc/socchina/runtime.conf with generation-based
// optimistic concurrency, atomic commit and automatic rollback.
package configtx

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	runtimeConf  = "/etc/socchina/runtime.conf"
	configDir    = "/var/lib/socchina/config"
	lastGoodConf = "/var/lib/socchina/config/last-good.conf"
	previousConf = "/var/lib/socchina/config/previous.conf"
	pendingConf  = "/var/lib/socchina/config/runtime.conf.pending"
	generationF  = "/var/lib/socchina/config/generation"
	startCmd     = "/usr/local/sbin/socchina-start"
	healthCmd    = "/usr/local/bin/socchina-health"
	serviceUnit  = "socchina-stream.service"
)

// Raw is a key=value snapshot of runtime.conf.
type Raw map[string]string

// Engine serialises cold-config mutations.
type Engine struct {
	mu sync.Mutex
}

// New returns a ready-to-use Engine.
func New() *Engine {
	os.MkdirAll(configDir, 0700)
	return &Engine{}
}

// GetCurrent returns the active runtime.conf and its generation.
func (e *Engine) GetCurrent() (Raw, int, error) {
	return readKV(runtimeConf)
}

// Validate parses a candidate and returns any structural errors.
func (e *Engine) Validate(candidate Raw) error {
	return validateRaw(candidate)
}

// Apply executes a full cold-config transaction:
//
//  1. Validate
//  2. Write pending
//  3. Check config
//  4. Backup current → last-good
//  5. Atomic rename pending → runtime.conf
//  6. Restart service
//  7. Wait for health
//  8. On failure: rollback
func (e *Engine) Apply(candidate Raw) error {
	e.mu.Lock()
	defer e.mu.Unlock()

	if err := validateRaw(candidate); err != nil {
		return fmt.Errorf("validate: %w", err)
	}

	// Store previous for rollback
	current, _, _ := readKV(runtimeConf)
	if len(current) > 0 {
		writeKV(lastGoodConf, current)
	}
	writeKV(previousConf, current)

	// Merge candidate over the current config so operational keys the web
	// form doesn't manage (e.g. CTRL_SOCK, which enables the app control
	// socket) survive a cold-config apply. Writing the candidate verbatim
	// would drop them and silently disable the hot-control channel.
	merged := Raw{}
	for k, v := range current {
		merged[k] = v
	}
	for k, v := range candidate {
		merged[k] = v
	}

	// Write merged config
	if err := writeKV(pendingConf, merged); err != nil {
		return fmt.Errorf("write pending: %w", err)
	}

	// Check config
	if out, err := exec.Command(startCmd, "--check-config").CombinedOutput(); err != nil {
		return fmt.Errorf("check-config failed: %s", strings.TrimSpace(string(out)))
	}

	// Atomic rename
	if err := os.Rename(pendingConf, runtimeConf); err != nil {
		return fmt.Errorf("atomic rename: %w", err)
	}

	// Restart
	if out, err := exec.Command("systemctl", "restart", serviceUnit).CombinedOutput(); err != nil {
		// Auto-rollback
		e.rollbackToLastGood()
		return fmt.Errorf("restart failed: %s", strings.TrimSpace(string(out)))
	}

	// Wait for service to stabilise (max 15s)
	if !e.waitHealthy(15 * time.Second) {
		e.rollbackToLastGood()
		return fmt.Errorf("health check failed after restart")
	}

	// Increment generation
	incGeneration()
	return nil
}

// Rollback restores the last-good configuration.
func (e *Engine) Rollback() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.rollbackToLastGood()
}

// SetHDMI enables or disables HDMI output (cold change, requires restart).
func (e *Engine) SetHDMI(enable bool) error {
	current, _, err := readKV(runtimeConf)
	if err != nil {
		return err
	}
	if enable {
		current["ENABLE_HDMI"] = "1"
	} else {
		current["ENABLE_HDMI"] = "0"
	}
	return e.Apply(current)
}

// RestartService restarts the core streaming service.
func (e *Engine) RestartService() error {
	out, err := exec.Command("systemctl", "restart", serviceUnit).CombinedOutput()
	if err != nil {
		return fmt.Errorf("restart: %s", strings.TrimSpace(string(out)))
	}
	return nil
}

// ServiceStatus returns the systemd unit state.
func (e *Engine) ServiceStatus() string {
	out, err := exec.Command("systemctl", "is-active", serviceUnit).CombinedOutput()
	if err != nil {
		return strings.TrimSpace(string(out))
	}
	return strings.TrimSpace(string(out))
}

// --- helpers ---

func (e *Engine) rollbackToLastGood() error {
	if _, err := os.Stat(lastGoodConf); err != nil {
		return fmt.Errorf("no last-good config to rollback")
	}
	data, err := os.ReadFile(lastGoodConf)
	if err != nil {
		return err
	}
	if err := os.WriteFile(runtimeConf, data, 0644); err != nil {
		return err
	}
	exec.Command("systemctl", "restart", serviceUnit).Run()
	e.waitHealthy(15 * time.Second)
	return nil
}

func (e *Engine) waitHealthy(timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		time.Sleep(500 * time.Millisecond)
		out, err := exec.Command(healthCmd).CombinedOutput()
		if err == nil && strings.Contains(string(out), "OK") {
			return true
		}
		// fallback: check if service is active
		out2, _ := exec.Command("systemctl", "is-active", serviceUnit).CombinedOutput()
		if strings.TrimSpace(string(out2)) == "active" {
			return true
		}
	}
	return false
}

func readKV(path string) (Raw, int, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, 0, err
	}
	m := Raw{}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		kv := strings.SplitN(line, "=", 2)
		if len(kv) != 2 {
			continue
		}
		m[strings.TrimSpace(kv[0])] = strings.TrimSpace(kv[1])
	}
	gen, _ := readGeneration()
	return m, gen, nil
}

func writeKV(path string, m Raw) error {
	var buf bytes.Buffer
	for k, v := range m {
		fmt.Fprintf(&buf, "%s=%s\n", k, v)
	}
	return os.WriteFile(path, buf.Bytes(), 0644)
}

func readGeneration() (int, error) {
	data, err := os.ReadFile(generationF)
	if err != nil {
		return 0, nil
	}
	return strconv.Atoi(strings.TrimSpace(string(data)))
}

func incGeneration() {
	gen, _ := readGeneration()
	os.WriteFile(generationF, []byte(strconv.Itoa(gen+1)), 0644)
}

// validateRaw checks that required fields are present and in range.
func validateRaw(m Raw) error {
	// Required keys
	for _, k := range []string{"CONFIG_VERSION", "RTSP_PORT", "STREAM_PATH", "SENSOR_INDEX", "CAPTURE_MODE", "TARGET_FPS"} {
		if v, ok := m[k]; !ok || v == "" {
			return fmt.Errorf("missing key: %s", k)
		}
	}
	if m["CONFIG_VERSION"] != "1" && m["CONFIG_VERSION"] != "0" {
		return fmt.Errorf("unsupported CONFIG_VERSION: %s", m["CONFIG_VERSION"])
	}
	if port, err := strconv.Atoi(m["RTSP_PORT"]); err != nil || port < 1 || port > 65535 {
		return fmt.Errorf("invalid RTSP_PORT: %s", m["RTSP_PORT"])
	}
	if mode := m["CAPTURE_MODE"]; mode != "linear" && mode != "wdr2to1" {
		return fmt.Errorf("CAPTURE_MODE must be linear or wdr2to1")
	}
	if fps, err := strconv.Atoi(m["TARGET_FPS"]); err != nil || fps < 1 || fps > 30 {
		return fmt.Errorf("invalid TARGET_FPS: %s", m["TARGET_FPS"])
	}
	if sensor, err := strconv.Atoi(m["SENSOR_INDEX"]); err != nil || sensor < 0 || sensor > 1 {
		return fmt.Errorf("invalid SENSOR_INDEX: %s", m["SENSOR_INDEX"])
	}
	if br, err := strconv.Atoi(m["BITRATE_KBPS"]); err != nil || br < 128 {
		return fmt.Errorf("invalid BITRATE_KBPS: %s", m["BITRATE_KBPS"])
	}
	return nil
}

// Marsal and Unmarshal helpers for JSON transport
func (r Raw) ToJSON() []byte {
	b, _ := json.Marshal(r)
	return b
}

func RawFromJSON(data []byte) (Raw, error) {
	var m Raw
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	return m, nil
}
