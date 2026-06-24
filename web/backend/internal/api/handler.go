// Package api implements the W1 read-only HTTP API (REST + SSE).
// It serves static files and aggregates data from the app control socket
// and the MediaMTX Control API, degrading gracefully when either is unavailable.
package api

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"sync"
	"time"

	"socchina-web/internal/adminclient"
	"socchina-web/internal/appclient"
	"socchina-web/internal/media"
)

// Handler bundles the HTTP handlers and shared state.
type Handler struct {
	App         *appclient.Client
	Media       *media.Client
	Admin       *adminclient.Client
	StaticDir   string
	SSEInterval time.Duration
	hlsProxy    *httputil.ReverseProxy

	mu        sync.Mutex
	listeners map[chan []byte]struct{}
}

// New creates a Handler.
func New(app *appclient.Client, med *media.Client, admin *adminclient.Client, staticDir string, hlsOrigin string, sseInterval time.Duration) *Handler {
	h := &Handler{
		App:         app,
		Media:       med,
		Admin:       admin,
		StaticDir:   staticDir,
		SSEInterval: sseInterval,
		listeners:   make(map[chan []byte]struct{}),
	}
	// Set up HLS reverse proxy to MediaMTX LL-HLS endpoint.
	// Strips the /hls/ prefix so that /hls/live/... → HLS_ORIGIN/live/...
	if hlsOrigin != "" {
		if u, err := url.Parse(hlsOrigin); err == nil {
			p := httputil.NewSingleHostReverseProxy(u)
			orig := p.Director
			p.Director = func(r *http.Request) {
				r.URL.Path = strings.TrimPrefix(r.URL.Path, "/hls")
				orig(r)
			}
			p.ModifyResponse = func(r *http.Response) error {
				// Allow CORS so browser can access HLS from different port
				r.Header.Set("Access-Control-Allow-Origin", "*")
				return nil
			}
			h.hlsProxy = p
		}
	}
	return h
}

// RegisterRoutes mounts all endpoints on the default mux.
func (h *Handler) RegisterRoutes() {
	fs := http.FileServer(http.Dir(h.StaticDir))
	http.Handle("/", fs)
	http.HandleFunc("/api/v1/status", h.statusJSON)
	http.HandleFunc("/api/v1/health", h.health)
	http.HandleFunc("/api/v1/media/status", h.mediaStatus)
	http.HandleFunc("/api/v1/events", h.events)
	// HLS 反向代理：/hls/* → MediaMTX LL-HLS :8888
	if h.hlsProxy != nil {
		http.Handle("/hls/", h.hlsProxy)
	}
	// W2 冷配置 API
	http.HandleFunc("/api/v1/config", h.configHandler)
	http.HandleFunc("/api/v1/config/apply", h.configApply)
	http.HandleFunc("/api/v1/config/rollback", h.configRollback)
	http.HandleFunc("/api/v1/hdmi", h.hdmiToggle)
	// W3 热控制
	http.HandleFunc("/api/v1/control", h.controlHandler)
	http.HandleFunc("/api/v1/presets/", h.presetsHandler)
	http.HandleFunc("/api/v1/defaults", h.defaultsHandler)
	http.HandleFunc("/api/v1/service/restart", h.serviceRestartHandler)
}

// StartBroadcast begins the periodic SSE broadcast loop. Must be called
// after the server is listening.
func (h *Handler) StartBroadcast() {
	go h.broadcastLoop()
}

// --- REST handlers ---

func (h *Handler) statusJSON(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	st := h.buildStatus()
	writeJSON(w, http.StatusOK, st)
}

func (h *Handler) health(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	appOK := h.App.Ping()
	mediaOK := h.Media.Ping()

	type healthResp struct {
		App     bool `json:"app"`
		Media   bool `json:"media"`
		Healthy bool `json:"healthy"`
	}
	resp := healthResp{App: appOK, Media: mediaOK, Healthy: true}
	if !appOK && !mediaOK {
		resp.Healthy = false
	}
	status := http.StatusOK
	if !resp.Healthy {
		status = http.StatusServiceUnavailable
	}
	writeJSON(w, status, resp)
}

func (h *Handler) mediaStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, h.Media.FetchStatus())
}

// --- SSE ---

func (h *Handler) events(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming not supported", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	ch := make(chan []byte, 8)
	h.mu.Lock()
	h.listeners[ch] = struct{}{}
	h.mu.Unlock()

	defer func() {
		h.mu.Lock()
		delete(h.listeners, ch)
		h.mu.Unlock()
		close(ch)
	}()

	// Send initial state immediately.
	h.pushState()

	ctx := r.Context()
	for {
		select {
		case <-ctx.Done():
			return
		case data := <-ch:
			if data == nil {
				return
			}
			fmt.Fprintf(w, "data: %s\n\n", data)
			flusher.Flush()
		}
	}
}

// --- internal ---

type statusPayload struct {
	Pipeline   appclient.Status `json:"pipeline"`
	Processing processingBlock  `json:"processing"`
	Outputs    outputsBlock     `json:"outputs"`
	Health     healthBlock      `json:"health"`
}

type processingBlock struct {
	NNEnabled        bool    `json:"nn_enabled"`
	NNDegraded       bool    `json:"nn_degraded"`
	ToneStrength     float64 `json:"tone_strength"`
	HighClipGuard    float64 `json:"high_clip_guard"`
	InferP95Ms       float64 `json:"infer_p95_ms"`
	TransactionP95Ms float64 `json:"transaction_p95_ms"`
}

type outputsBlock struct {
	HDMI    bool `json:"hdmi"`
	RTSP    bool `json:"rtsp"`
	WebRTC  bool `json:"webrtc"`
	HLS     bool `json:"hls"`
	Viewers int  `json:"viewers"`
}

type healthBlock struct {
	AppOK   bool `json:"app_ok"`
	MediaOK bool `json:"media_ok"`
}

func (h *Handler) buildStatus() statusPayload {
	app := h.App.FetchStatus()
	med := h.Media.FetchStatus()

	return statusPayload{
		Pipeline: app,
		Processing: processingBlock{
			NNEnabled:        app.NNEnabled,
			NNDegraded:       app.NNDegraded,
			ToneStrength:     app.ToneStrength,
			HighClipGuard:    app.HighClipGuard,
			InferP95Ms:       app.InferP95Ms,
			TransactionP95Ms: app.TransactionP95Ms,
		},
		Outputs: outputsBlock{
			HDMI:    app.HDMI,
			RTSP:    app.RTSP,
			WebRTC:  med.WebRTC,
			HLS:     med.HLS,
			Viewers: med.Viewers,
		},
		Health: healthBlock{
			AppOK:   h.App.Ping(),
			MediaOK: h.Media.Ping(),
		},
	}
}

func (h *Handler) pushState() {
	data, _ := json.Marshal(h.buildStatus())
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.listeners {
		select {
		case ch <- data:
		default:
			// slow consumer; drop this push
		}
	}
}

func (h *Handler) broadcastLoop() {
	ticker := time.NewTicker(h.SSEInterval)
	defer ticker.Stop()
	for range ticker.C {
		h.pushState()
	}
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	if err := json.NewEncoder(w).Encode(v); err != nil {
		log.Printf("api: writeJSON: %v", err)
	}
}

// --- W2 cold config handlers ---

func (h *Handler) configHandler(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		cfg, gen, err := h.Admin.GetConfig()
		if err != nil {
			writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]interface{}{
			"config":     cfg,
			"generation": gen,
		})
	case http.MethodPut:
		var candidate map[string]string
		if err := json.NewDecoder(r.Body).Decode(&candidate); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad json"})
			return
		}
		// Validate only (dry-run)
		if err := h.Admin.ValidateConfig(candidate); err != nil {
			writeJSON(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		writeJSON(w, http.StatusOK, map[string]string{"status": "valid"})
	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (h *Handler) configApply(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var candidate map[string]string
	if err := json.NewDecoder(r.Body).Decode(&candidate); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad json"})
		return
	}
	if err := h.Admin.ApplyConfig(candidate); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "applied"})
}

func (h *Handler) configRollback(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.Admin.Rollback(); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "rolled back"})
}

func (h *Handler) hdmiToggle(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var body struct {
		Enable bool `json:"enable"`
	}
	json.NewDecoder(r.Body).Decode(&body)
	if err := h.Admin.SetHDMI(body.Enable); err != nil {
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]interface{}{"hdmi": body.Enable})
}

// --- W3 hot-control handlers ---

func (h *Handler) controlHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPatch {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	var params map[string]interface{}
	if err := json.NewDecoder(r.Body).Decode(&params); err != nil {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad json"})
		return
	}

	// Whitelist: hot parameters (§5.1). C server parses those it supports;
	// unrecognised keys are silently ignored rather than rejected.
	allowed := map[string]bool{
		"enhancement_enabled": true,
		"tone_strength":       true,
		"tone_enabled":        true,
		"nn_high_clip_guard":  true,
		"nn_clut_enabled":     true,
		"drc_mode":            true,
		"drc_strength":        true,
		"ldci_mode":           true,
	}
	filtered := map[string]interface{}{}
	for k, v := range params {
		if allowed[k] {
			filtered[k] = v
		}
	}

	if len(filtered) == 0 {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "no valid parameters"})
		return
	}

	if err := h.App.SendControl(filtered); err != nil {
		log.Printf("api: controlHandler: %v", err)
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, map[string]string{"status": "applied"})
}

func (h *Handler) presetsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Extract preset name from /api/v1/presets/{name}
	name := strings.TrimPrefix(r.URL.Path, "/api/v1/presets/")
	if name == "" {
		writeJSON(w, http.StatusBadRequest, map[string]string{"error": "missing preset name"})
		return
	}

	preset, ok := appclient.PresetDefs[name]
	if !ok {
		writeJSON(w, http.StatusNotFound, map[string]string{"error": "unknown preset: " + name})
		return
	}

	if err := h.App.SendControl(preset); err != nil {
		log.Printf("api: presetsHandler: %v", err)
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": err.Error()})
		return
	}

	writeJSON(w, http.StatusOK, map[string]interface{}{"preset": name, "status": "applied"})
}

// POST /api/v1/defaults — reset all hot parameters to safe defaults (§5.3 稳定)
func (h *Handler) defaultsHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.App.SendControl(appclient.PresetDefs["stable"]); err != nil {
		log.Printf("api: defaultsHandler: %v", err)
		writeJSON(w, http.StatusServiceUnavailable, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "defaults applied"})
}

// POST /api/v1/service/restart — restart core streaming service (requires cold config)
func (h *Handler) serviceRestartHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if err := h.Admin.RestartService(); err != nil {
		log.Printf("api: serviceRestartHandler: %v", err)
		writeJSON(w, http.StatusInternalServerError, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "service restarting"})
}
