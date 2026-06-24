// socchina-web — 板端只读 Web 控制台 (W1)。
//
// 提供静态页面、REST API 和 SSE 事件流。
// 通过 Unix socket 从 socchina_app 获取流水线指标，
// 通过 HTTP 从 MediaMTX Control API 获取媒体会话状态。
// 所有后端依赖不可用时优雅降级，不崩溃。
package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"socchina-web/internal/adminclient"
	"socchina-web/internal/api"
	"socchina-web/internal/appclient"
	"socchina-web/internal/config"
	"socchina-web/internal/media"
)

var version = "dev"

func main() {
	configPath := flag.String("config", "/etc/socchina/web.conf", "config file path")
	flag.Parse()

	cfg := config.Load(*configPath)
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("[socchina-web] ")

	app := appclient.New(cfg.AppControlSock)
	med := media.New(cfg.MediamtxAPI)
	admin := adminclient.New(cfg.AdminSock)

	h := api.New(app, med, admin, cfg.StaticDir, cfg.HLSOrigin, time.Duration(cfg.SSEIntervalSec)*time.Second)
	h.RegisterRoutes()

	srv := &http.Server{
		Addr:         cfg.ListenAddr,
		Handler:      nil, // uses DefaultServeMux
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 15 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Start broadcast after server is ready.
	go func() {
		time.Sleep(100 * time.Millisecond)
		h.StartBroadcast()
	}()

	// Graceful shutdown on SIGTERM / SIGINT.
	idle := make(chan struct{})
	go func() {
		sigint := make(chan os.Signal, 1)
		signal.Notify(sigint, syscall.SIGTERM, syscall.SIGINT)
		<-sigint
		log.Println("shutting down...")
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		srv.Shutdown(ctx)
		close(idle)
	}()

	log.Printf("listening on %s (version=%s)", cfg.ListenAddr, version)
	if err := srv.ListenAndServe(); err != http.ErrServerClosed {
		log.Fatalf("listen: %v", err)
	}
	<-idle
	fmt.Fprintln(os.Stderr, "stopped")
}
