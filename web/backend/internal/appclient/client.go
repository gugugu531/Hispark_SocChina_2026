// Package appclient talks to socchina_app over the Unix control socket
// (/run/socchina/app-control.sock). The socket protocol is one JSON
// object per line: {"id":N,"op":"status"} → {"id":N,"ok":true,...}.
//
// When the socket is absent (e.g. before W3 wiring) the client
// returns pre-populated "unavailable" data rather than blocking the
// API — the frontend shows every field as N/A.
package appclient

import (
	"encoding/json"
	"errors"
	"net"
	"sync"
	"time"
)

// Status mirrors a subset of pipeline_metrics_t exposed to the web console.
type Status struct {
	State            string  `json:"state"`
	CaptureMode      string  `json:"capture_mode"`
	TargetFPS        int     `json:"target_fps"`
	DisplayFPS       float64 `json:"display_fps"`
	StreamDrops      int64   `json:"stream_drops"`
	Timeouts         int64   `json:"timeouts"`
	TransientErrors  int64   `json:"transient_errors"`
	FatalErrors      int64   `json:"fatal_errors"`
	NNEnabled        bool    `json:"nn_enabled"`
	NNDegraded       bool    `json:"nn_degraded"`
	ToneStrength     float64 `json:"tone_strength"`
	HighClipGuard    float64 `json:"high_clip_guard"`
	InferP95Ms       float64 `json:"infer_p95_ms"`
	TransactionP95Ms float64 `json:"transaction_p95_ms"`
	HDMI             bool    `json:"hdmi"`
	RTSP             bool    `json:"rtsp"`
}

// Client is a thread-safe handle to the app control socket.
type Client struct {
	mu   sync.Mutex
	addr string
}

// New returns a Client that connects to the given Unix socket address.
func New(addr string) *Client {
	return &Client{addr: addr}
}

// dialRetry attempts to connect to the Unix socket, retrying up to
// maxRetries times with delay between attempts. The C server accepts
// one connection per ~100ms control cycle; retries avoid EAGAIN when
// the listen backlog is full.
func (c *Client) dialRetry() (net.Conn, error) {
	// Single long timeout: the C server accepts one connection per ~100ms,
	// and we share the socket with authproxy. A 3s deadline with internal
	// retries is more reliable than many short DialTimeout calls that each
	// hit a full backlog immediately.
	deadline := time.Now().Add(3 * time.Second)
	for {
		conn, err := net.DialTimeout("unix", c.addr, 300*time.Millisecond)
		if err == nil {
			return conn, nil
		}
		if time.Now().After(deadline) {
			return nil, errUnavailable
		}
		time.Sleep(150 * time.Millisecond)
	}
}

// FetchStatus obtains a pipeline status snapshot. If the socket is not
// available the returned Status has State == "UNAVAILABLE".
func (c *Client) FetchStatus() Status {
	unavailable := Status{State: "UNAVAILABLE"}
	if c.addr == "" {
		return unavailable
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := c.dialRetry()
	if err != nil {
		return unavailable
	}
	defer conn.Close()

	conn.SetDeadline(time.Now().Add(1500 * time.Millisecond))
	req := []byte(`{"id":1,"op":"status"}` + "\n")
	if _, err := conn.Write(req); err != nil {
		return unavailable
	}

	var buf [4096]byte
	n, err := conn.Read(buf[:])
	if err != nil {
		return unavailable
	}

	// C server format: {"id":1,"ok":true,"pipeline":{...},"processing":{...},"outputs":{...}}
	var raw struct {
		OK         bool   `json:"ok"`
		Pipeline   Status `json:"pipeline"`
		Processing struct {
			NNEnabled        bool    `json:"nn_enabled"`
			NNDegraded       bool    `json:"nn_degraded"`
			ToneStrength     float64 `json:"tone_strength"`
			HighClipGuard    float64 `json:"high_clip_guard"`
			InferP95Ms       float64 `json:"infer_p95_ms"`
			TransactionP95Ms float64 `json:"transaction_p95_ms"`
		} `json:"processing"`
		Outputs struct {
			HDMI bool `json:"hdmi"`
			RTSP bool `json:"rtsp"`
		} `json:"outputs"`
	}
	if err := json.Unmarshal(buf[:n], &raw); err != nil {
		return unavailable
	}
	if !raw.OK {
		return unavailable
	}
	st := raw.Pipeline
	st.NNEnabled = raw.Processing.NNEnabled
	st.NNDegraded = raw.Processing.NNDegraded
	st.ToneStrength = raw.Processing.ToneStrength
	st.HighClipGuard = raw.Processing.HighClipGuard
	st.InferP95Ms = raw.Processing.InferP95Ms
	st.TransactionP95Ms = raw.Processing.TransactionP95Ms
	st.HDMI = raw.Outputs.HDMI
	st.RTSP = raw.Outputs.RTSP
	return st
}

// Ping returns true if the socket is reachable (best-effort, with retry).
func (c *Client) Ping() bool {
	if c.addr == "" {
		return false
	}
	conn, err := c.dialRetry()
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

// SendControl sends a set of hot parameters to the app control socket.
// The C server parses flat keys, so params are placed at top level alongside "op":"set".
func (c *Client) SendControl(params map[string]interface{}) error {
	if c.addr == "" {
		return errUnavailable
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	conn, err := c.dialRetry()
	if err != nil {
		return errUnavailable
	}
	defer conn.Close()

	conn.SetDeadline(time.Now().Add(2 * time.Second))
	// Flatten: put param keys at top level so C strstr parser finds them
	req := map[string]interface{}{"id": 2, "op": "set"}
	for k, v := range params {
		req[k] = v
	}
	reqB, _ := json.Marshal(req)
	reqB = append(reqB, '\n')
	if _, err := conn.Write(reqB); err != nil {
		return errUnavailable
	}

	var buf [4096]byte
	n, err := conn.Read(buf[:])
	if err != nil {
		return errUnavailable
	}

	var raw struct {
		OK    bool   `json:"ok"`
		Error string `json:"error"`
	}
	if err := json.Unmarshal(buf[:n], &raw); err != nil {
		return errUnavailable
	}
	if !raw.OK {
		if raw.Error != "" {
			return errors.New(raw.Error)
		}
		return errFailed
	}
	return nil
}

// PresetDefs maps preset names to parameter sets (§5.3).
// Includes all documented hot params; C server ignores unsupported keys silently.
var PresetDefs = map[string]map[string]interface{}{
	"stable": {"enhancement_enabled": true, "tone_enabled": true, "tone_strength": 0.25, "nn_clut_enabled": false, "nn_high_clip_guard": 3, "drc_mode": "auto", "drc_strength": 512, "ldci_mode": "auto"},
	"dark":   {"enhancement_enabled": true, "tone_enabled": true, "tone_strength": 0.60, "nn_clut_enabled": true, "nn_high_clip_guard": 15, "drc_mode": "auto", "drc_strength": 512, "ldci_mode": "auto"},
	"wdr":    {"enhancement_enabled": true, "tone_enabled": true, "tone_strength": 0.25, "nn_clut_enabled": true, "nn_high_clip_guard": 8, "drc_mode": "auto", "drc_strength": 512, "ldci_mode": "auto"},
	"bypass": {"enhancement_enabled": false, "tone_enabled": false, "tone_strength": 0.0, "nn_clut_enabled": false, "nn_high_clip_guard": 3, "drc_mode": "off", "drc_strength": 0, "ldci_mode": "off"},
	"custom": {"enhancement_enabled": true, "tone_enabled": true, "tone_strength": 0.25, "nn_clut_enabled": true, "nn_high_clip_guard": 3, "drc_mode": "auto", "drc_strength": 512, "ldci_mode": "auto"},
}

var (
	errUnavailable = errors.New("app socket unavailable")
	errFailed      = errors.New("request failed")
)

var errNotImplemented = errors.New("not implemented")
