// socchina-admin — 板端冷配置事务管理服务 (W2)。
//
// 监听 /run/socchina/admin.sock，通过 Unix socket 提供
// 受限管理操作：config.validate / config.apply / config.rollback /
// hdmi.set / service.restart / service.status。
// 禁止任意 shell 命令和用户指定任意路径。
package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"os/user"
	"strconv"
	"syscall"

	"socchina-admin/internal/configtx"
)

const sockPath = "/tmp/socchina-admin.sock"

type request struct {
	ID     int                    `json:"id"`
	Op     string                 `json:"op"`
	Params map[string]interface{} `json:"params,omitempty"`
}

type response struct {
	ID      int         `json:"id"`
	OK      bool        `json:"ok"`
	Error   string      `json:"error,omitempty"`
	Result  interface{} `json:"result,omitempty"`
}

var engine = configtx.New()

func main() {
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("[socchina-admin] ")

	os.Remove(sockPath)
	ln, err := net.Listen("unix", sockPath)
	if err != nil {
		log.Fatalf("listen: %v", err)
	}
	os.Chmod(sockPath, 0660)
	// Chown to socchina group so web user can access
	if grp, err := user.LookupGroup("socchina"); err == nil {
		if gid, err := strconv.Atoi(grp.Gid); err == nil {
			os.Chown(sockPath, 0, gid)
		}
	}
	log.Printf("listening on %s", sockPath)

	go func() {
		sig := make(chan os.Signal, 1)
		signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
		<-sig
		ln.Close()
		os.Remove(sockPath)
		os.Exit(0)
	}()

	for {
		conn, err := ln.Accept()
		if err != nil {
			continue
		}
		go handle(conn)
	}
}

func handle(conn net.Conn) {
	defer conn.Close()
	var buf [8192]byte
	n, err := conn.Read(buf[:])
	if err != nil || n == 0 {
		return
	}

	// Trim trailing newline
	data := buf[:n]
	if len(data) > 0 && data[len(data)-1] == '\n' {
		data = data[:len(data)-1]
	}

	var req request
	if err := json.Unmarshal(data, &req); err != nil {
		writeResp(conn, response{ID: 0, OK: false, Error: "bad json"})
		return
	}

	var resp response
	resp.ID = req.ID

	switch req.Op {
	case "config.validate":
		candidate, err := configtx.RawFromJSON(marshalParams(req.Params))
		if err != nil {
			resp.Error = err.Error()
		} else if err := engine.Validate(candidate); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
		}

	case "config.apply":
		candidate, err := configtx.RawFromJSON(marshalParams(req.Params))
		if err != nil {
			resp.Error = err.Error()
		} else if err := engine.Apply(candidate); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
			_, gen, _ := engine.GetCurrent()
			resp.Result = map[string]int{"generation": gen}
		}

	case "config.rollback":
		if err := engine.Rollback(); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
		}

	case "hdmi.set":
		enable := false
		if v, ok := req.Params["enable"]; ok {
			if b, ok := v.(bool); ok {
				enable = b
			}
		}
		if err := engine.SetHDMI(enable); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
		}

	case "service.restart":
		if err := engine.RestartService(); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
		}

	case "service.status":
		resp.OK = true
		resp.Result = map[string]string{"state": engine.ServiceStatus()}

	case "config.get":
		current, gen, err := engine.GetCurrent()
		if err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
			resp.Result = map[string]interface{}{
				"config":     current,
				"generation": gen,
			}
		}

	default:
		resp.Error = fmt.Sprintf("unknown op: %s", req.Op)
	}

	writeResp(conn, resp)
}

func writeResp(conn net.Conn, resp response) {
	b, _ := json.Marshal(resp)
	conn.Write(append(b, '\n'))
}

func marshalParams(p map[string]interface{}) []byte {
	if p == nil {
		return []byte("{}")
	}
	b, _ := json.Marshal(p)
	return b
}

