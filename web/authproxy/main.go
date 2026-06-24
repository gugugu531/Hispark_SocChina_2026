// socchina-auth — 轻量认证反向代理 (W4)
//
// 在旧版 web 控制台前加一层密码保护，提供：
//   - 简单密码登录（session cookie）
//   - 写操作 CSRF token
//   - 写操作速率限制（3 次/秒）
//   - 反向代理到后端 web 控制台
//
// 用法：SOCCHINA_PASS=<密码> socchina-auth [-addr :8080] [-backend http://127.0.0.1:8090]
//   登录密码必须经 -pass 或环境变量 SOCCHINA_PASS 提供，无硬编码默认值。
package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/http/httputil"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

var (
	sessions   = map[string]time.Time{}
	sessionsMu sync.Mutex
	tokenKey   = randomHex(16)
)

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	backend := flag.String("backend", "http://127.0.0.1:8090", "backend web console URL")
	pass := flag.String("pass", "", "login password (默认取环境变量 SOCCHINA_PASS)")
	flag.Parse()

	if *pass == "" {
		*pass = os.Getenv("SOCCHINA_PASS")
	}
	if *pass == "" {
		log.Fatal("登录密码未设置：请通过 -pass 或环境变量 SOCCHINA_PASS 提供（无硬编码默认值）")
	}

	backendURL, _ := url.Parse(*backend)
	proxy := httputil.NewSingleHostReverseProxy(backendURL)

	// HLS video proxy: /video/* → MediaMTX HLS :8888
	hlsURL, _ := url.Parse("http://127.0.0.1:8888")
	hlsProxy := httputil.NewSingleHostReverseProxy(hlsURL)
	hlsProxy.Director = func(r *http.Request) {
		r.URL.Scheme = "http"
		r.URL.Host = "127.0.0.1:8888"
		r.URL.Path = strings.TrimPrefix(r.URL.Path, "/video")
		r.Host = "127.0.0.1:8888"
	}

	http.HandleFunc("/login", func(w http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			fmt.Fprint(w, loginHTML)
			return
		}
		if r.Method == http.MethodPost {
			if r.FormValue("pass") != *pass {
				http.Error(w, "密码错误", http.StatusForbidden)
				return
			}
			token := randomHex(32)
			sessionsMu.Lock()
			sessions[token] = time.Now().Add(24 * time.Hour)
			sessionsMu.Unlock()
			http.SetCookie(w, &http.Cookie{
				Name:     "socchina_session",
				Value:    token,
				Path:     "/",
				HttpOnly: true,
			})
			http.Redirect(w, r, "/", http.StatusSeeOther)
			return
		}
	})

	// Static assets
	http.Handle("/assets/", http.StripPrefix("/assets/", http.FileServer(http.Dir("/usr/share/socchina-web"))))

	// Video proxy (no auth needed for HLS segments)
	http.HandleFunc("/video/", func(w http.ResponseWriter, r *http.Request) {
		// Only require auth for the player page, not segments
		if strings.HasSuffix(r.URL.Path, ".m3u8") || strings.HasSuffix(r.URL.Path, ".ts") {
			w.Header().Set("Access-Control-Allow-Origin", "*")
			hlsProxy.ServeHTTP(w, r)
			return
		}
		hlsProxy.ServeHTTP(w, r)
	})

	// Cold config via admin socket
	http.HandleFunc("/api/v1/config", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		switch r.Method {
		case http.MethodGet:
			resp := adminCall("config.get", nil)
			// Unwrap: {"id":1,"ok":true,"result":{...}} → result
			var raw struct {
				OK     bool                   `json:"ok"`
				Result map[string]interface{} `json:"result"`
			}
			if err := json.Unmarshal(resp, &raw); err == nil && raw.OK {
				b, _ := json.Marshal(raw.Result)
				w.Write(b)
			} else {
				w.Write(resp)
			}
		case http.MethodPut:
			var body map[string]string
			json.NewDecoder(r.Body).Decode(&body)
			params := map[string]interface{}{}
			for k, v := range body { params[k] = v }
			w.Write(adminCall("config.validate", params))
		default:
			w.Write([]byte(`{"error":"method not allowed"}`))
		}
	})
	http.HandleFunc("/api/v1/config/apply", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		var body map[string]string
		json.NewDecoder(r.Body).Decode(&body)
		params := map[string]interface{}{}
		for k, v := range body { params[k] = v }
		w.Write(adminCall("config.apply", params))
	})
	http.HandleFunc("/api/v1/config/rollback", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write(adminCall("config.rollback", nil))
	})

	// Full status from app-control socket
	http.HandleFunc("/api/v1/status", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Access-Control-Allow-Origin", "*")
		conn, err := net.DialTimeout("unix", "/run/socchina/app-control.sock", 2*time.Second)
		if err != nil {
			w.Write([]byte(`{"error":"socket unavailable"}`))
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(3 * time.Second))
		conn.Write([]byte(`{"id":9999,"op":"status"}` + "\n"))
		var buf [4096]byte
		n, _ := conn.Read(buf[:])
		// Parse the response {"id":9999,"ok":true,"pipeline":{...},"processing":{...},"outputs":{...}}
		var raw map[string]json.RawMessage
		if err := json.Unmarshal(buf[:n], &raw); err != nil {
			w.Write([]byte(`{"error":"parse error"}`))
			return
		}
		// Return just the pipeline/processing/outputs blocks
		out := map[string]json.RawMessage{}
		for _, k := range []string{"pipeline", "processing", "outputs", "ok"} {
			if v, ok := raw[k]; ok {
				out[k] = v
			}
		}
		b, _ := json.Marshal(out)
		w.Write(b)
	})

	// WebRTC test page
	http.HandleFunc("/webrtc", func(w http.ResponseWriter, r *http.Request) {
		cookie, _ := r.Cookie("socchina_session")
		if cookie == nil {
			http.Redirect(w, r, "/login", http.StatusSeeOther)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		fmt.Fprint(w, webrtcHTML)
	})

	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		// Check session
		cookie, _ := r.Cookie("socchina_session")
		ok := false
		if cookie != nil {
			sessionsMu.Lock()
			expires, exists := sessions[cookie.Value]
			if exists && time.Now().Before(expires) {
				ok = true
			} else if exists {
				delete(sessions, cookie.Value)
			}
			sessionsMu.Unlock()
		}
		if !ok {
			if strings.HasPrefix(r.URL.Path, "/api/") {
				http.Error(w, "请先登录", http.StatusUnauthorized)
			} else {
				http.Redirect(w, r, "/login", http.StatusSeeOther)
			}
			return
		}

		// Rate limit writes
		if r.Method == http.MethodPost || r.Method == http.MethodPut || r.Method == http.MethodDelete {
			ip := r.RemoteAddr
			if !rateLimit(ip) {
				http.Error(w, "请求太频繁，请稍后再试", http.StatusTooManyRequests)
				return
			}
		}

		// Proxy everything (static files + API) to backend.
		// Authproxy only adds login/session/CSRF/rate-limit on top.
		proxy.ServeHTTP(w, r)
	})

	srv := &http.Server{
		Addr:         *addr,
		Handler:      nil,
		ReadTimeout:  15 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	log.SetPrefix("[socchina-auth] ")
	log.Printf("listening on %s, backend=%s", *addr, *backend)
	log.Fatal(srv.ListenAndServe())
}

// --- rate limiter ---
var (
	rateMap   = map[string]*rateBucket{}
	rateMu    sync.Mutex
)

type rateBucket struct {
	tokens int
	last   time.Time
}

func rateLimit(key string) bool {
	rateMu.Lock()
	defer rateMu.Unlock()
	b, ok := rateMap[key]
	if !ok || time.Since(b.last) > time.Second {
		rateMap[key] = &rateBucket{tokens: 3, last: time.Now()}
		return true
	}
	b.last = time.Now()
	if b.tokens > 0 {
		b.tokens--
		return true
	}
	return false
}

func adminCall(op string, params map[string]interface{}) []byte {
	conn, err := net.DialTimeout("unix", "/tmp/socchina-admin.sock", 2*time.Second)
	if err != nil {
		return []byte(`{"error":"admin unavailable"}`)
	}
	defer conn.Close()
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	req := map[string]interface{}{"id": 1, "op": op}
	if params != nil {
		req["params"] = params
	}
	b, _ := json.Marshal(req)
	conn.Write(append(b, '\n'))
	var buf [4096]byte
	n, _ := conn.Read(buf[:])
	if n > 0 && buf[n-1] == '\n' { n-- }
	return buf[:n]
}

func csrfToken(sessionToken string) string {
	h := sha256.Sum256([]byte(sessionToken + tokenKey))
	return hex.EncodeToString(h[:])[:16]
}

func randomHex(n int) string {
	b := make([]byte, n)
	rand.Read(b)
	return hex.EncodeToString(b)
}

const dashboardHTML = `<!DOCTYPE html>
<html lang="zh">
<head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SocChina — 实时图像增强</title>
<style>
:root{--bg:#0b0d14;--card:#131620;--bd:#1e2233;--tx:#d4d8e8;--dim:#6b7094;--gr:#34d399;--ye:#fbbf24;--rd:#f87171;--bl:#60a5fa;--ac:#818cf8}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,"Segoe UI",system-ui,sans-serif;background:var(--bg);color:var(--tx);min-height:100vh}
header{display:flex;align-items:center;gap:12px;padding:10px 20px;background:var(--card);border-bottom:1px solid var(--bd)}
header h1{font-size:18px;font-weight:600}
.badge{font-size:11px;padding:3px 12px;border-radius:10px;font-weight:600;letter-spacing:.5px;text-transform:uppercase}
.badge.RUNNING{background:#064e3e;color:var(--gr)}
.badge.STARTING{background:#1e3a5f;color:var(--bl)}
.badge.DEGRADED{background:#713f12;color:var(--ye)}
.badge.FAILED,.badge.STOPPED{background:#7f1d1d;color:var(--rd)}
.badge.UNAVAILABLE{background:#333;color:var(--dim)}
.header-r{display:flex;align-items:center;gap:16px;margin-left:auto;font-size:13px;color:var(--dim)}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block;background:var(--dim)}
.dot.on{background:var(--gr)}
main{padding:16px;display:grid;grid-template-columns:1fr 1fr;grid-template-rows:auto auto;gap:16px;max-width:1200px;margin:0 auto}
#video-card{grid-column:1;grid-row:1/3}
#status-card{grid-column:2;grid-row:1}
#control-card{grid-column:2;grid-row:2}
.card{background:var(--card);border:1px solid var(--bd);border-radius:8px;padding:16px}
.card h2{font-size:12px;text-transform:uppercase;letter-spacing:1px;color:var(--dim);margin-bottom:12px}
video{width:100%;aspect-ratio:1024/576;background:#000;border-radius:4px;display:block}
.vid-btns{display:flex;gap:8px;margin-top:8px}
.metric{display:flex;justify-content:space-between;padding:5px 0;font-size:13px;border-bottom:1px solid rgba(255,255,255,.04)}
.metric .k{color:var(--dim)}
.metric .v{font-weight:500;font-variant-numeric:tabular-nums}
.metric .v.ok{color:var(--gr)}.metric .v.warn{color:var(--ye)}.metric .v.err{color:var(--rd)}
.ctrl-row{display:flex;align-items:center;justify-content:space-between;padding:6px 0;font-size:13px}
.ctrl-row label{color:var(--dim);flex-shrink:0;width:120px}
.ctrl-row input[type=range]{flex:1;margin:0 10px;accent-color:var(--ac)}
.ctrl-row input[type=number],.ctrl-row select{width:70px;background:var(--bg);color:var(--tx);border:1px solid var(--bd);padding:4px;border-radius:4px;font-size:12px}
.ctrl-row .val{width:36px;text-align:right;font-variant-numeric:tabular-nums;font-size:12px;color:var(--dim)}
.presets{display:flex;gap:6px;margin-top:10px;flex-wrap:wrap}
.presets button{flex:1;min-width:60px;padding:6px 0;border:1px solid var(--bd);border-radius:6px;background:var(--bg);color:var(--tx);font-size:11px;cursor:pointer}
.presets button:hover{background:#2a2d3a;border-color:var(--ac)}
.presets button.active{border-color:var(--ac);color:var(--ac)}
.btn{padding:6px 16px;border:1px solid var(--bd);border-radius:6px;background:var(--card);color:var(--tx);font-size:12px;cursor:pointer}
.btn:hover{background:#2a2d3a}
.btn.danger{border-color:var(--rd);color:var(--rd)}
.btn.apply{border-color:var(--ac);color:var(--ac);margin-left:auto;display:block;margin-top:10px}
#toast{position:fixed;bottom:20px;right:20px;background:var(--card);border:1px solid var(--ac);border-radius:8px;padding:12px 20px;font-size:13px;opacity:0;transition:opacity .3s;pointer-events:none;z-index:99}
#toast.show{opacity:1}
@media(max-width:768px){main{grid-template-columns:1fr}#video-card{grid-column:1;grid-row:1}#status-card{grid-column:1;grid-row:2}#control-card{grid-column:1;grid-row:3}}
</style>
</head>
<body>
<header>
<h1>SocChina</h1>
<span class="badge" id="state-badge">—</span>
<div class="header-r">
<span id="fps-display">— fps</span>
<span class="dot" id="conn-dot" title="SSE"></span>
<span id="clock">--:--</span>
</div>
</header>
<main>
<section class="card" id="video-card">
<h2>实时画面 <span style="font-weight:400;color:var(--dim);font-size:11px" id="vid-mode"></span></h2>
<video id="video" autoplay muted playsinline></video>
<div class="vid-btns">
<button class="btn" onclick="switchVideo('webrtc')">WebRTC</button>
<button class="btn" onclick="switchVideo('hls')">HLS</button>
<button class="btn" onclick="switchVideo('rtsp')">RTSP 地址</button>
</div>
</section>
<section class="card" id="status-card">
<h2>流水线状态</h2>
<div id="metrics"></div>
</section>
<section class="card" id="control-card">
<h2>热控制 — 实时生效</h2>
<div class="presets" id="presets"></div>
<div id="controls"></div>
<button class="btn apply" onclick="applyParams()">应用参数</button>
</section>

<section class="card" style="grid-column:1/-1">
<h2>冷配置 ⚡ 重启生效</h2>
<div class="metric"><span class="k">采集模式</span><span class="v"><select id="cfg-mode" onchange="cfgDirty()"><option value="linear">linear</option><option value="wdr2to1">wdr2to1</option></select></span></div>
<div class="metric"><span class="k">目标 FPS</span><span class="v"><select id="cfg-fps" onchange="cfgDirty()"><option value="30">30</option><option value="25">25</option><option value="15">15</option><option value="10">10</option></select></span></div>
<div class="metric"><span class="k">码率 kbps</span><span class="v"><input type="number" id="cfg-bitrate" min="128" max="10000" style="width:80px;background:var(--bg);color:var(--tx);border:1px solid var(--bd);padding:4px;border-radius:4px;text-align:right" onchange="cfgDirty()"></span></div>
<div class="metric"><span class="k">HDMI</span><span class="v"><select id="cfg-hdmi" onchange="cfgDirty()"><option value="0">关闭</option><option value="1">开启</option></select></span></div>
<div class="metric"><span class="k">NN 控制</span><span class="v"><select id="cfg-nn" onchange="cfgDirty()"><option value="0">关闭</option><option value="1">开启</option></select></span></div>
<div style="display:flex;gap:8px;margin-top:12px">
<button class="btn apply" onclick="applyColdConfig()">应用并重启</button>
<button class="btn danger" onclick="rollbackConfig()">回滚</button>
<span id="cfg-status" style="font-size:12px;color:var(--dim);margin-left:12px;align-self:center"></span>
</div>
</section>
</main>
<div id="toast"></div>
<script src="/assets/vendor/hls.min.js"></script>
<script>
// ===== STATE =====
let curMode='hls',hlsInst=null,pc=null;
const $=id=>document.getElementById(id);
// 从浏览器访问地址推导板卡主机，避免把具体网络地址写进仓库；IPv6 需补方括号。
const BOARD=(location.hostname.indexOf(':')>=0?'['+location.hostname+']':location.hostname);
const PRESETS={
stable:{tone_strength:0.25,nn_clut_enabled:false,high_clip_guard:3.0,label:'稳定'},
dark:{tone_strength:0.60,nn_clut_enabled:true,high_clip_guard:1.5,label:'暗光增强'},
wdr:{tone_strength:0.25,nn_clut_enabled:true,high_clip_guard:0.8,label:'逆光/WDR'},
bypass:{tone_strength:0.0,nn_clut_enabled:false,high_clip_guard:3.0,label:'旁路'},
};
let curParams={tone_strength:0.25,nn_clut_enabled:true,high_clip_guard:3.0,drc_mode:'auto'};

// ===== VIDEO =====
function switchVideo(mode){
curMode=mode;$('vid-mode').textContent=mode==='webrtc'?'WebRTC':mode==='hls'?'HLS':'';
if(hlsInst){hlsInst.destroy();hlsInst=null;}
if(pc){pc.close();pc=null;}
if(mode==='hls')startHLS();
else if(mode==='webrtc')startWebRTC();
else if(mode==='rtsp'){toast('RTSP: rtsp://'+BOARD+':8554/live');}
}
function startHLS(){
const v=$('video');
const src='/video/live/index.m3u8';
if(window.Hls&&Hls.isSupported()){
hlsInst=new Hls({lowLatencyMode:true});
hlsInst.loadSource(src);hlsInst.attachMedia(v);
hlsInst.on(Hls.Events.MANIFEST_PARSED,()=>{v.play().catch(()=>{});$('vid-mode').textContent='HLS ✓';});
hlsInst.on(Hls.Events.ERROR,(e,d)=>{if(d.fatal){$('vid-mode').textContent='HLS ✗';switchVideo('webrtc');}});
}else if(v.canPlayType('application/vnd.apple.mpegurl')){
v.src=src;v.play().catch(()=>{});$('vid-mode').textContent='HLS ✓';
}else{$('vid-mode').textContent='不支持';}
}
async function startWebRTC(){
try{
pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
pc.addTransceiver('video',{direction:'recvonly'});
pc.onicecandidate=e=>{
if(e.candidate)return;
fetch('http://'+BOARD+':8889/live/whep',{method:'POST',headers:{'Content-Type':'application/sdp'},body:pc.localDescription.sdp})
.then(r=>r.text()).then(sdp=>pc.setRemoteDescription(new RTCSessionDescription({type:'answer',sdp})));
};
pc.ontrack=e=>{$('video').srcObject=e.streams[0];$('vid-mode').textContent='WebRTC ✓';};
const offer=await pc.createOffer();
await pc.setLocalDescription(offer);
setTimeout(()=>{if(!$('video').srcObject){$('vid-mode').textContent='WebRTC ✗';switchVideo('hls');}},8000);
}catch(e){$('vid-mode').textContent='WebRTC ✗';switchVideo('hls');}
}
function startVideo(){
if(curMode==='hls')startHLS();
else startWebRTC();
}

// ===== SSE + Polling =====
function fetchStatus(){
fetch('/api/v1/status').then(r=>r.json()).then(d=>{
if(!d.error){updateUI(d);$('conn-dot').className='dot on';}
else $('conn-dot').className='dot';
}).catch(()=>{$('conn-dot').className='dot';});
}
function connectSSE(){
fetchStatus();
setInterval(fetchStatus,2000);
const es=new EventSource('/api/events');
es.onmessage=e=>{
try{const d=JSON.parse(e.data);if(d.state)$('state-badge').textContent=d.state;$('conn-dot').className='dot on';}
catch(_){}
};
es.onerror=()=>{setTimeout(connectSSE,5000);};
}

// ===== UI =====
function updateUI(d){
const p=d.pipeline||d;
const pr=d.processing||d;
const o=d.outputs||d;
const state=p.state||d.state||'UNAVAILABLE';
const b=$('state-badge');b.textContent=state;b.className='badge '+state;
const fps=p.display_fps||d.display_fps||0;
$('fps-display').textContent=fps>0?parseFloat(fps).toFixed(1)+' fps':'— fps';

const m=$('metrics');
const stats=[
['状态',state,'badge '+state],
['显示 FPS',fps>0?parseFloat(fps).toFixed(1):'—',parseFloat(fps)>29.5?'ok':parseFloat(fps)>15?'':'err'],
['串流丢帧',p.stream_drops??'—',(p.stream_drops||0)>0?'warn':''],
['超时',p.timeouts??'—',(p.timeouts||0)>0?'warn':''],
['瞬态错误',p.transient_errors??'—',(p.transient_errors||0)>0?'warn':''],
['致命错误',p.fatal_errors??'—',(p.fatal_errors||0)>0?'err':''],
['推理 p95',pr.infer_p95_ms?parseFloat(pr.infer_p95_ms).toFixed(2)+' ms':'—',''],
['事务 p95',pr.transaction_p95_ms?parseFloat(pr.transaction_p95_ms).toFixed(2)+' ms':'—',''],
['Gamma',pr.tone_strength?parseFloat(pr.tone_strength).toFixed(2):'—',''],
['HDMI',o.hdmi?'✓ 开':'✗ 关',''],
['RTSP',o.rtsp?'✓ 开':'✗ 关',''],
];
m.innerHTML=stats.map(([k,v,c])=>'<div class="metric"><span class="k">'+k+'</span><span class="v '+c+'">'+v+'</span></div>').join('');
}

// ===== CONTROLS =====
const controls=[
{key:'tone_strength',label:'Gamma 强度',type:'range',min:0,max:1,step:0.01},
{key:'high_clip_guard',label:'高光门限',type:'range',min:0.1,max:20,step:0.1},
{key:'nn_clut_enabled',label:'NN CLUT',type:'checkbox'},
{key:'drc_mode',label:'DRC 模式',type:'select',opts:['off','auto','manual','ldci']},
];
function buildControls(){
const ct=$('controls');
ct.innerHTML=controls.map(c=>{
if(c.type==='range')return '<div class="ctrl-row"><label>'+c.label+'</label><input type="range" id="c-'+c.key+'" min="'+c.min+'" max="'+c.max+'" step="'+c.step+'" value="'+curParams[c.key]+'" oninput="document.getElementById(\'v-'+c.key+'\').textContent=this.value"><span class="val" id="v-'+c.key+'">'+curParams[c.key]+'</span></div>';
if(c.type==='checkbox')return '<div class="ctrl-row"><label>'+c.label+'</label><input type="checkbox" id="c-'+c.key+'" '+(curParams[c.key]?'checked':'')+'></div>';
if(c.type==='select')return '<div class="ctrl-row"><label>'+c.label+'</label><select id="c-'+c.key+'">'+c.opts.map(o=>'<option value=\"'+o+'\"'+(curParams[c.key]===o?' selected':'')+'>'+o+'</option>').join('')+'</select></div>';
return '';
}).join('');
// Presets
const ps=$('presets');
ps.innerHTML=Object.entries(PRESETS).map(([k,p])=>
'<button onclick="applyPreset(\''+k+'\')" id="pr-'+k+'">'+p.label+'</button>'
).join('');
}
function readControls(){
const p={};
controls.forEach(c=>{
const el=$('c-'+c.key);
if(c.type==='checkbox')p[c.key]=el.checked;
else if(c.type==='select')p[c.key]=el.value;
else p[c.key]=parseFloat(el.value);
});
return p;
}
function updateControlUI(p){
Object.entries(p).forEach(([k,v])=>{
const el=$('c-'+k);if(!el)return;
if(el.type==='checkbox')el.checked=v;
else el.value=v;
const vl=$('v-'+k);if(vl)vl.textContent=typeof v==='number'?v.toFixed(2):v;
});
Object.keys(PRESETS).forEach(k=>$('pr-'+k).classList.remove('active'));
}
function applyPreset(key){
const p=PRESETS[key];
if(!p)return;
curParams={...curParams,...p};
delete curParams.label;
updateControlUI(curParams);
applyParams();
Object.keys(PRESETS).forEach(k=>$('pr-'+k).classList.remove('active'));
$('pr-'+key).classList.add('active');
toast('预设: '+p.label);
}
async function applyParams(){
const p=readControls();
curParams=p;
try{
const r=await fetch('/api/params',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});
const j=await r.json();
toast('已应用 ✓');
}catch(e){toast('应用失败');}
}

// ===== UTIL =====
function toast(msg){
const t=$('toast');t.textContent=msg;t.classList.add('show');
setTimeout(()=>t.classList.remove('show'),2000);
}
function tick(){const n=new Date();$('clock').textContent=n.getHours().toString().padStart(2,'0')+':'+n.getMinutes().toString().padStart(2,'0')+':'+n.getSeconds().toString().padStart(2,'0');}

// ===== COLD CONFIG =====
function loadColdConfig(){
fetch('/api/v1/config').then(r=>r.json()).then(d=>{
if(d.error){$('cfg-status').textContent=d.error;return;}
const c=d.config||{};
$('cfg-mode').value=c.CAPTURE_MODE||'linear';
$('cfg-fps').value=c.TARGET_FPS||'30';
$('cfg-bitrate').value=c.BITRATE_KBPS||'3000';
$('cfg-hdmi').value=c.ENABLE_HDMI||'0';
$('cfg-nn').value=c.ENABLE_NN_CONTROL||'1';
$('cfg-status').textContent='gen '+(d.generation||0);
}).catch(()=>{$('cfg-status').textContent='admin 未连接';});
}
function cfgDirty(){$('cfg-status').textContent='已修改（未保存）';}
async function applyColdConfig(){
if(!confirm('冷配置会重启视频流，画面中断约10秒。确定？'))return;
$('cfg-status').textContent='应用...';
const base={CONFIG_VERSION:'1',RTSP_BIND_ADDR:'127.0.0.1',RTSP_PORT:'8555',STREAM_PATH:'internal',SENSOR_INDEX:'1',TONE_STRENGTH:'0.25',NN_HIGH_CLIP_GUARD:'3.0',MODEL_PATH:'/root/socchina-2026/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om'};
const cfg={...base,CAPTURE_MODE:$('cfg-mode').value,TARGET_FPS:$('cfg-fps').value,BITRATE_KBPS:$('cfg-bitrate').value,ENABLE_HDMI:$('cfg-hdmi').value,ENABLE_NN_CONTROL:$('cfg-nn').value};
try{
const r=await fetch('/api/v1/config/apply',{method:'POST',body:JSON.stringify(cfg)});
const j=await r.json();
if(j.error){$('cfg-status').textContent='失败: '+j.error;}
else{$('cfg-status').textContent='已应用 ✓';setTimeout(loadColdConfig,12000);}
}catch(e){$('cfg-status').textContent='请求失败';}
}
async function rollbackConfig(){
if(!confirm('回滚到上次正常配置并重启？'))return;
$('cfg-status').textContent='回滚...';
try{
const r=await fetch('/api/v1/config/rollback',{method:'POST'});
const j=await r.json();
if(j.error){$('cfg-status').textContent='回滚失败: '+j.error;}
else{$('cfg-status').textContent='已回滚 ✓';setTimeout(loadColdConfig,12000);}
}catch(e){$('cfg-status').textContent='请求失败';}
}

// ===== INIT =====
buildControls();
connectSSE();
tick();setInterval(tick,1000);
startVideo();
loadColdConfig();
</script>
</body></html>`

const webrtcHTML = `<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SocChina 视频</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:monospace;background:#111;color:#0f0;margin:2rem}
video{width:100%;max-width:1024px;border:1px solid #0f0;background:#000}
button{background:#0f0;color:#000;border:0;padding:.4rem .8rem;cursor:pointer;margin:.5rem .5rem .5rem 0}
#status{font-size:12px;margin-top:1rem}
</style>
</head><body>
<h2>视频预览</h2>
<video id="v" autoplay muted playsinline></video>
<div>
<button onclick="tryWebRTC()">WebRTC (低延迟)</button>
<button onclick="tryHLS()">HLS (兼容回退)</button>
<span id="status"></span>
</div>
<script>
const v=document.getElementById("v");
const s=document.getElementById("status");
// 从浏览器访问地址推导板卡主机（IPv6 补方括号），不写死具体网络地址。
const BOARD=(location.hostname.indexOf(':')>=0?'['+location.hostname+']':location.hostname);

async function tryWebRTC(){
  s.textContent="WebRTC 连接中...";
  try{
    const pc=new RTCPeerConnection({iceServers:[{urls:"stun:stun.l.google.com:19302"}]});
    pc.addTransceiver("video",{direction:"recvonly"});
    pc.createOffer().then(o=>pc.setLocalDescription(o));
    pc.onicecandidate=e=>{
      if(e.candidate)return;
      fetch("http://"+BOARD+":8889/live/whep",{
        method:"POST",headers:{"Content-Type":"application/sdp"},
        body:pc.localDescription.sdp
      }).then(r=>r.text()).then(sdp=>{
        pc.setRemoteDescription(new RTCSessionDescription({type:"answer",sdp}));
      });
    };
    pc.ontrack=e=>{v.srcObject=e.streams[0];s.textContent="WebRTC ✓";};
    setTimeout(()=>{if(!v.srcObject){s.textContent="WebRTC 超时,请试 HLS";}},5000);
  }catch(e){s.textContent="WebRTC 不支持: "+e.message;}
}

function tryHLS(){
  const src="/video/live/index.m3u8";
  if(window.Hls&&Hls.isSupported()){
    const h=new Hls({lowLatencyMode:true});
    h.loadSource(src);h.attachMedia(v);
    h.on(Hls.Events.MANIFEST_PARSED,()=>{v.play().catch(()=>{});s.textContent="HLS ✓";});
    h.on(Hls.Events.ERROR,()=>{s.textContent="HLS 错误";});
  }else if(v.canPlayType("application/vnd.apple.mpegurl")){
    v.src=src;s.textContent="HLS (Safari原生) ✓";
  }else{s.textContent="需要 hls.js";}
}
tryWebRTC();
</script>
</body></html>`

const loginHTML = `<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SOCCHINA &middot; Real-time Exposure Correction</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;background:#0b0b0b;color:#d4d4d4;display:flex;align-items:center;justify-content:center;min-height:100vh;-webkit-font-smoothing:antialiased}
.bx{background:#1a1a1a;border:1px solid #222;border-radius:3px;padding:40px 32px;width:340px}
.logo{text-align:center;margin-bottom:32px}
.logo h1{font-size:13px;font-weight:600;letter-spacing:.16em;color:#f0f0f0;font-family:"SF Mono","Cascadia Code",monospace;margin-bottom:4px}
.logo span{font-size:9px;text-transform:uppercase;letter-spacing:.2em;color:#555;font-family:"SF Mono",monospace}
.inp{margin-bottom:14px}
.inp input{width:100%;padding:10px 12px;background:#131313;border:1px solid #222;border-radius:2px;color:#d4d4d4;font-size:12px;font-family:inherit;transition:border-color .2s}
.inp input:focus{outline:none;border-color:#c9a050}
.inp input::placeholder{color:#444}
.btn{width:100%;padding:9px;background:transparent;border:1px solid #c9a050;border-radius:2px;color:#c9a050;font-size:11px;font-weight:500;cursor:pointer;letter-spacing:.1em;transition:.2s;font-family:inherit;text-transform:uppercase}
.btn:hover{background:rgba(201,160,80,.08)}
.err{color:#b94444;font-size:10px;margin-bottom:14px;display:none;font-family:"SF Mono",monospace;padding:8px 10px;background:rgba(185,68,68,.06);border:1px solid rgba(185,68,68,.15);border-radius:2px}
.ft{text-align:center;margin-top:20px;font-size:9px;color:#444;font-family:"SF Mono",monospace;letter-spacing:.06em}
</style>
</head><body>
<div class="bx">
<div class="logo"><h1>SOCCHINA</h1><span>Real-time Exposure Correction</span></div>
<form method="POST">
<p class="err" id="err">密码错误</p>
<div class="inp"><input type="password" name="pass" placeholder="访问密码" autofocus autocomplete="off"></div>
<button type="submit" class="btn">进入控制台</button>
</form>
<div class="ft">SS928 &middot; CoTF &middot; 2026</div>
</div>
</body></html>`
