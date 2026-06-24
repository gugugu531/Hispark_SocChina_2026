// Package media provides a minimal read-only client for the MediaMTX
// Control API (http://127.0.0.1:9997). It only fetches path list and
// reader counts — the web console does not reconfigure MediaMTX.
package media

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"
)

// PathInfo mirrors a single path entry from the MediaMTX API (v3).
type PathInfo struct {
	Name     string        `json:"name"`
	Ready    bool          `json:"ready"`
	Tracks   []string      `json:"tracks"`
	BytesRec int64         `json:"bytesReceived"`
	BytesOut int64         `json:"bytesSent"`
	Readers  []interface{} `json:"readers"`
}

// Status aggregates the subset of MediaMTX state shown on the dashboard.
type Status struct {
	RTSP    bool `json:"rtsp"`
	WebRTC  bool `json:"webrtc"`
	HLS     bool `json:"hls"`
	Viewers int  `json:"viewers"`
}

// Client talks to the MediaMTX Control API.
type Client struct {
	baseURL string
	hc      *http.Client
}

// New returns a Client for the given MediaMTX API base URL.
func New(baseURL string) *Client {
	return &Client{
		baseURL: baseURL,
		hc:      &http.Client{Timeout: 2 * time.Second},
	}
}

// FetchStatus queries MediaMTX and returns aggregated status.
func (c *Client) FetchStatus() Status {
	if c.baseURL == "" {
		return Status{}
	}
	resp, err := c.hc.Get(c.baseURL + "/v3/paths/list")
	if err != nil {
		return Status{}
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return Status{}
	}

	var raw struct {
		ItemCount int        `json:"itemCount"`
		Items     []PathInfo `json:"items"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
		return Status{}
	}

	var s Status
	for _, p := range raw.Items {
		if p.Name == "live" {
			s.RTSP = true
			s.WebRTC = true
			s.HLS = true
			s.Viewers = len(p.Readers)
			break
		}
	}
	return s
}

// Ping returns true if the MediaMTX API is reachable (best-effort).
func (c *Client) Ping() bool {
	if c.baseURL == "" {
		return false
	}
	resp, err := c.hc.Get(c.baseURL + "/v3/paths/list")
	if err != nil {
		return false
	}
	resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// Format rate as a short human string. Exported for use in templates.
func FormatRate(bps int64) string {
	if bps < 1000 {
		return fmt.Sprintf("%d B/s", bps)
	}
	if bps < 1000*1000 {
		return fmt.Sprintf("%.1f kB/s", float64(bps)/1000)
	}
	return fmt.Sprintf("%.1f MB/s", float64(bps)/(1000*1000))
}
