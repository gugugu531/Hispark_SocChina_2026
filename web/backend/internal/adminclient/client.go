// Package adminclient talks to socchina-admin over the Unix admin socket
// (/run/socchina/admin.sock). The socket protocol is one JSON object per
// line: {"id":N,"op":"...","params":{...}} → {"id":N,"ok":true,...}.
package adminclient

import (
	"encoding/json"
	"net"
	"sync/atomic"
	"time"
)

// Client is a handle to the admin socket.
type Client struct {
	addr string
	seq  int64
}

// New returns a Client for the given socket address.
func New(addr string) *Client {
	return &Client{addr: addr}
}

// Per-op response deadlines. Short ops (get/validate) return promptly, but
// cold-config transactions on the admin side restart socchina-stream and wait
// for health (up to ~15s), and a failed apply additionally rolls back with a
// second restart+health wait. A blanket 5s deadline made those legitimately
// slow ops falsely report "admin socket unavailable" even though the board
// completed the transaction. Give the long ops a deadline that exceeds the
// admin's worst-case synchronous time.
const (
	shortOpTimeout = 5 * time.Second
	applyTimeout   = 45 * time.Second // restart+health(15s) ×2 (apply + rollback) + margin
	restartTimeout = 20 * time.Second // single service restart
)

// call sends a request with the default short-op deadline.
func (c *Client) call(op string, params map[string]interface{}) (map[string]interface{}, error) {
	return c.callTimeout(op, params, shortOpTimeout)
}

// callTimeout sends a request and returns the raw response map, using the
// given response deadline.
func (c *Client) callTimeout(op string, params map[string]interface{}, timeout time.Duration) (map[string]interface{}, error) {
	if c.addr == "" {
		return nil, errUnavailable
	}
	id := int(atomic.AddInt64(&c.seq, 1))

	req := map[string]interface{}{"id": id, "op": op}
	if params != nil {
		req["params"] = params
	}
	reqB, _ := json.Marshal(req)
	reqB = append(reqB, '\n')

	conn, err := net.DialTimeout("unix", c.addr, 2*time.Second)
	if err != nil {
		return nil, errUnavailable
	}
	defer conn.Close()

	conn.SetDeadline(time.Now().Add(timeout))
	if _, err := conn.Write(reqB); err != nil {
		return nil, errUnavailable
	}

	var buf [8192]byte
	n, err := conn.Read(buf[:])
	if err != nil || n == 0 {
		return nil, errUnavailable
	}
	if buf[n-1] == '\n' {
		n--
	}

	var resp struct {
		OK     bool                   `json:"ok"`
		Error  string                 `json:"error"`
		Result map[string]interface{} `json:"result"`
	}
	if err := json.Unmarshal(buf[:n], &resp); err != nil {
		return nil, errUnavailable
	}
	if !resp.OK {
		if resp.Error != "" {
			return resp.Result, &OpError{resp.Error}
		}
		return resp.Result, errFailed
	}
	return resp.Result, nil
}

// --- exported ops ---

// GetConfig returns the current runtime config and generation.
func (c *Client) GetConfig() (map[string]string, int, error) {
	result, err := c.call("config.get", nil)
	if err != nil {
		return nil, 0, err
	}
	cfg, _ := result["config"].(map[string]interface{})
	out := map[string]string{}
	for k, v := range cfg {
		if s, ok := v.(string); ok {
			out[k] = s
		}
	}
	gen := 0
	if g, ok := result["generation"]; ok {
		if f, ok := g.(float64); ok {
			gen = int(f)
		}
	}
	return out, gen, nil
}

// ValidateConfig checks a candidate config without applying it.
func (c *Client) ValidateConfig(cfg map[string]string) error {
	params := map[string]interface{}{}
	for k, v := range cfg {
		params[k] = v
	}
	_, err := c.call("config.validate", params)
	return err
}

// ApplyConfig submits a new config and triggers the cold-restart transaction.
func (c *Client) ApplyConfig(cfg map[string]string) error {
	params := map[string]interface{}{}
	for k, v := range cfg {
		params[k] = v
	}
	_, err := c.callTimeout("config.apply", params, applyTimeout)
	return err
}

// Rollback restores the last-good configuration.
func (c *Client) Rollback() error {
	_, err := c.callTimeout("config.rollback", nil, applyTimeout)
	return err
}

// SetHDMI enables or disables HDMI output (cold change → full apply transaction).
func (c *Client) SetHDMI(enable bool) error {
	_, err := c.callTimeout("hdmi.set", map[string]interface{}{"enable": enable}, applyTimeout)
	return err
}

// RestartService restarts the core streaming service via admin socket.
func (c *Client) RestartService() error {
	_, err := c.callTimeout("service.restart", nil, restartTimeout)
	return err
}

// Ping returns true if the admin socket is reachable.
func (c *Client) Ping() bool {
	if c.addr == "" {
		return false
	}
	conn, err := net.DialTimeout("unix", c.addr, 500*time.Millisecond)
	if err != nil {
		return false
	}
	conn.Close()
	return true
}

// --- errors ---

type OpError struct{ Msg string }

func (e *OpError) Error() string { return e.Msg }

var (
	errUnavailable = &OpError{"admin socket unavailable"}
	errFailed      = &OpError{"admin request failed"}
)
