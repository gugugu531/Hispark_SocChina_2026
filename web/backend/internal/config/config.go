package config

import (
	"bufio"
	"os"
	"strings"
)

// Cfg holds the web console configuration.
type Cfg struct {
	ListenAddr      string
	StaticDir       string
	AppControlSock  string
	AdminSock       string
	MediamtxAPI     string
	HLSOrigin       string // MediaMTX LL-HLS origin, e.g. http://127.0.0.1:8888
	SSEIntervalSec  int
}

// Load reads a simple KEY=VALUE config file. Returns usable defaults when the file
// is missing or incomplete — the web server keeps running and reports degraded status
// rather than failing hard.
func Load(path string) *Cfg {
	c := &Cfg{
		ListenAddr:     ":8080",
		StaticDir:      "/usr/share/socchina-web",
		AppControlSock: "/run/socchina/app-control.sock",
		AdminSock:      "/run/socchina/admin.sock",
		MediamtxAPI:    "http://127.0.0.1:9997",
		HLSOrigin:      "http://127.0.0.1:8888",
		SSEIntervalSec: 1,
	}
	if path == "" {
		return c
	}
	f, err := os.Open(path)
	if err != nil {
		return c
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		kv := strings.SplitN(line, "=", 2)
		if len(kv) != 2 {
			continue
		}
		k := strings.TrimSpace(kv[0])
		v := strings.TrimSpace(kv[1])
		switch k {
		case "LISTEN_ADDR":
			c.ListenAddr = v
		case "STATIC_DIR":
			c.StaticDir = v
		case "APP_CONTROL_SOCK":
			c.AppControlSock = v
		case "ADMIN_SOCK":
			c.AdminSock = v
		case "MEDIAMTX_API":
			c.MediamtxAPI = v
		case "HLS_ORIGIN":
			c.HLSOrigin = v
		case "SSE_INTERVAL_SEC":
			// best-effort parse; invalid values stay at default
			if n := len(v); n > 0 {
				d := 0
				for i := 0; i < n; i++ {
					if v[i] < '0' || v[i] > '9' {
						d = -1
						break
					}
					d = d*10 + int(v[i]-'0')
				}
				if d > 0 {
					c.SSEIntervalSec = d
				}
			}
		}
	}
	return c
}
