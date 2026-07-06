/* SocChina Web Console — W1 只读前端 */

(function () {
  'use strict';

  // ── 状态 ──
  let videoMode = 'webrtc';  // 'webrtc' | 'hls' | 'unavailable'
  const $ = (id) => document.getElementById(id);

  // ── SSE ──
  function connectSSE() {
    const es = new EventSource('/api/v1/events');
    es.onopen = function () {
      $('connection-dot').className = 'dot connected';
      $('connection-dot').title = 'SSE 已连接';
    };
    es.onmessage = function (e) {
      try {
        const data = JSON.parse(e.data);
        updateDashboard(data);
      } catch (_) { /* ignore malformed */ }
    };
    es.onerror = function () {
      $('connection-dot').className = 'dot disconnected';
      $('connection-dot').title = 'SSE 断开，5s 后重连';
      es.close();
      setTimeout(connectSSE, 5000);
    };
  }

  // ── 仪表盘更新 ──
  function updateDashboard(data) {
    const p = data.pipeline || {};
    const pr = data.processing || {};
    const o = data.outputs || {};
    const h = data.health || {};

    // 流水线
    setBadge('p-state', p.state || 'UNAVAILABLE');
    setVal('p-mode', p.capture_mode);
    setVal('p-target-fps', p.target_fps, '');
    setFPS('p-display-fps', p.display_fps);
    setNum('p-stream-drops', p.stream_drops, 'err');
    setNum('p-timeouts', p.timeouts, 'warn');
    setNum('p-transient', p.transient_errors, 'warn');
    setNum('p-fatal', p.fatal_errors, 'err');

    // 全局状态徽章
    const state = p.state || 'UNAVAILABLE';
    const badge = $('state-badge');
    badge.textContent = state;
    badge.className = 'badge ' + state;

    // 处理
    setBool('pr-nn', pr.nn_enabled);
    setDegraded('pr-degraded', pr.nn_degraded);
    setVal('pr-tone', pr.tone_strength != null ? pr.tone_strength.toFixed(2) : null);
    setVal('pr-clip', pr.high_clip_guard != null ? pr.high_clip_guard.toFixed(1) : null);
    setMs('pr-infer-p95', pr.infer_p95_ms);
    setMs('pr-txn-p95', pr.transaction_p95_ms);

    // 输出
    setBool('o-hdmi', o.hdmi);
    setBool('o-rtsp', o.rtsp);
    setBool('o-webrtc', o.webrtc);
    setBool('o-hls', o.hls);
    setNum('o-viewers', o.viewers, '');

    // 健康（admin_ok 由后端 SSE health 块带出，不再每秒轮询 /api/v1/config）
    setBool('h-app', h.app_ok);
    setBool('h-media', h.media_ok);
    setBool('h-admin', h.admin_ok);

    // 热控件回填（原为覆盖 updateDashboard 的猴子补丁，现直接内联）
    syncHotFromStatus(data);

    // 如果应用不可用，切换到 HLS 视频模式
    if (p.state === 'UNAVAILABLE' && videoMode === 'webrtc') {
      switchToHLS();
    }
  }

  // ── 视频 (§3.3 WebRTC 优先, LL-HLS 回退) ──
  var hlsInst = null;
  var webrtcPc = null;
  var webrtcTimer = null;

  function initVideo() {
    tryWebRTC();
  }

  function tryWebRTC() {
    $('video-mode').textContent = 'WebRTC 连接中…';
    if (webrtcTimer) clearTimeout(webrtcTimer);
    cleanupWebRTC();

    try {
      webrtcPc = new RTCPeerConnection({iceServers: [{urls: 'stun:stun.l.google.com:19302'}]});
      webrtcPc.addTransceiver('video', {direction: 'recvonly'});
      webrtcPc.onicecandidate = function(e) {
        if (e.candidate) return;
        var whepUrl = 'http://' + window.location.hostname + ':8889/live/whep';
        fetch(whepUrl, {
          method: 'POST',
          headers: {'Content-Type': 'application/sdp'},
          body: webrtcPc.localDescription.sdp
        }).then(function(r) {
          if (!r.ok) throw new Error('whep failed');
          return r.text();
        }).then(function(sdp) {
          webrtcPc.setRemoteDescription(new RTCSessionDescription({type: 'answer', sdp: sdp}));
        }).catch(function() {
          switchToHLS('WebRTC 信令不可用');
        });
      };
      webrtcPc.ontrack = function(e) {
        var v = $('live-video');
        v.srcObject = e.streams[0];
        $('video-placeholder').classList.add('hidden');
        v.classList.add('active');
        videoMode = 'webrtc'; setVideoModeBtn('webrtc');
        $('video-mode').textContent = 'WebRTC ✓ 低延迟';
        $('video-latency').textContent = '';
        if (webrtcTimer) { clearTimeout(webrtcTimer); webrtcTimer = null; }
      };
      webrtcPc.createOffer().then(function(o) {
        return webrtcPc.setLocalDescription(o);
      });
      // Timeout: if no track within 8s, fall back to HLS
      webrtcTimer = setTimeout(function() {
        if (videoMode !== 'webrtc') switchToHLS('WebRTC 超时');
      }, 8000);
    } catch(e) {
      switchToHLS('WebRTC 不支持');
    }
  }

  function cleanupWebRTC() {
    if (webrtcPc) { webrtcPc.close(); webrtcPc = null; }
  }

  function switchToHLS(reason) {
    if (videoMode === 'hls') return;
    cleanupWebRTC();
    if (webrtcTimer) { clearTimeout(webrtcTimer); webrtcTimer = null; }
    if (typeof Hls === 'undefined') {
      $('video-mode').textContent = 'HLS 不可用（hls.js 未加载）';
      return;
    }
    videoMode = 'hls'; setVideoModeBtn('hls');
    $('video-mode').textContent = 'LL-HLS' + (reason ? ' (' + reason + ')' : '');
    $('video-latency').textContent = '高延迟模式';
    var video = $('live-video');
    if (hlsInst) hlsInst.destroy();
    if (Hls.isSupported()) {
      hlsInst = new Hls({lowLatencyMode: true, maxBufferSize: 2*1000*1000, maxBufferLength: 2});
      hlsInst.loadSource('/hls/live/index.m3u8');
      hlsInst.attachMedia(video);
      hlsInst.on(Hls.Events.MANIFEST_PARSED, function() {
        video.play().catch(function(){});
        $('video-placeholder').classList.add('hidden');
        video.classList.add('active');
      });
      hlsInst.on(Hls.Events.ERROR, function(_ev, data) {
        if (data.fatal) $('video-latency').textContent = 'HLS 致命错误，请重连';
      });
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      video.src = '/hls/live/index.m3u8';
      video.play().catch(function(){});
      $('video-placeholder').classList.add('hidden');
      video.classList.add('active');
    }
  }

  // ── Helper ──
  function setVal(id, val, suffix) {
    const el = $(id);
    if (val == null || val === '') {
      el.textContent = '—';
      el.className = 'v na';
    } else if (suffix === undefined) {
      el.textContent = String(val);
      el.className = 'v';
    } else {
      el.textContent = String(val) + (suffix || '');
      el.className = 'v';
    }
  }

  function setNum(id, val, cls) {
    var el = $(id);
    if (val == null) { el.textContent = '—'; el.className = 'v na'; return; }
    el.textContent = String(val);
    el.className = 'v' + (val > 0 && cls ? ' ' + cls : '');
  }

  function setFPS(id, val) {
    var el = $(id);
    if (val == null || val <= 0) { el.textContent = '—'; el.className = 'v na'; return; }
    el.textContent = val.toFixed(1);
    el.className = 'v' + (val >= 29.5 ? ' ok' : val < 15 ? ' err' : ' warn');
  }

  function setMs(id, val) {
    var el = $(id);
    if (val == null || val <= 0) { el.textContent = '—'; el.className = 'v na'; return; }
    el.textContent = val.toFixed(2) + ' ms';
    el.className = 'v' + (val < 3 ? ' ok' : val < 10 ? '' : ' warn');
  }

  function setBool(id, val) {
    var el = $(id);
    if (val == null) { el.textContent = '—'; el.className = 'v na'; return; }
    el.textContent = val ? '✓' : '✗';
    el.className = 'v' + (val ? ' ok' : '');
  }

  function setDegraded(id, val) {
    var el = $(id);
    if (val == null) { el.textContent = '—'; el.className = 'v na'; return; }
    el.textContent = val ? '⚠ 是' : '否';
    el.className = 'v' + (val ? ' warn' : ' ok');
  }

  function setBadge(id, val) {
    var el = $(id);
    el.textContent = val || '—';
    el.className = 'v ' + (val || '');
  }

  // ── 时钟 ──
  function tick() {
    var now = new Date();
    $('clock').textContent =
      now.getHours().toString().padStart(2, '0') + ':' +
      now.getMinutes().toString().padStart(2, '0') + ':' +
      now.getSeconds().toString().padStart(2, '0');
  }

  // ── 视频控制 ──
  function setVideoModeBtn(activeId) {
    ['webrtc','hls','rtsp'].forEach(function(m) {
      var btn = $('btn-'+m);
      if (btn) btn.className = (m === activeId) ? 'vid-mode-btn active' : 'vid-mode-btn';
    });
  }
  $('btn-fullscreen').addEventListener('click', function () {
    var v = $('live-video');
    if (v.requestFullscreen) v.requestFullscreen();
    else if (v.webkitRequestFullscreen) v.webkitRequestFullscreen();
  });
  $('btn-webrtc').addEventListener('click', function () {
    if (hlsInst) { hlsInst.destroy(); hlsInst = null; }
    var v = $('live-video'); v.srcObject = null; v.src = ''; v.classList.remove('active');
    $('video-placeholder').classList.remove('hidden');
    videoMode = ''; setVideoModeBtn('webrtc'); tryWebRTC();
  });
  $('btn-hls').addEventListener('click', function () {
    if (hlsInst) { hlsInst.destroy(); hlsInst = null; }
    cleanupWebRTC();
    var v = $('live-video'); v.srcObject = null; v.src = ''; v.classList.remove('active');
    $('video-placeholder').classList.remove('hidden');
    videoMode = ''; setVideoModeBtn('hls'); switchToHLS();
  });
  $('btn-rtsp').addEventListener('click', function () {
    setVideoModeBtn('rtsp');
    var url = 'rtsp://' + window.location.hostname + ':8554/live';
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(url).then(
        function() { $('video-latency').textContent = '已复制: ' + url; },
        function() { $('video-latency').textContent = url + ' (VLC/ffplay)'; });
    } else {
      $('video-latency').textContent = url + ' (VLC/ffplay)';
    }
    setTimeout(function() { $('video-latency').textContent = ''; }, 5000);
  });
  $('btn-reconnect').addEventListener('click', function () {
    if (hlsInst) { hlsInst.destroy(); hlsInst = null; }
    var v = $('live-video');
    v.srcObject = null;
    v.src = '';
    v.classList.remove('active');
    $('video-placeholder').classList.remove('hidden');
    videoMode = '';
    initVideo();
  });

  // ── 热控制 (W3) ──
  var hotDefs = [
    {key: 'enhancement_enabled', label: '全局增强',          type: 'checkbox', hot: true},
    {key: 'nn_clut_enabled',     label: 'AI 色彩增强',       type: 'checkbox', hot: true},
    {key: 'tone_enabled',        label: '色调映射',          type: 'checkbox', hot: true},
    {key: 'tone_strength',       label: '色调映射强度',      type: 'range',  min: 0, max: 1,    step: 0.01, hot: true},
    {key: 'nn_high_clip_guard',  label: '高光保护阈值',      type: 'range',  min: 0, max: 100,  step: 1,    hot: true},
    {key: 'drc_mode',            label: '动态范围压缩',      type: 'select', opts: ['off','auto'], hot: true},
    {key: 'drc_strength',        label: '压缩强度',          type: 'range',  min: 0, max: 1023, step: 1,    hot: true},
    {key: 'ldci_mode',           label: '局部对比度增强',    type: 'select', opts: ['off','auto'], hot: true}
  ];

  var presets = {
    vivid:   {label: '鲜艳增强', enhancement_enabled: true,  nn_clut_enabled: true,  tone_enabled: true,  tone_strength: 0.70, nn_high_clip_guard: 10, drc_mode: 'auto', drc_strength: 512, ldci_mode: 'auto'},
    natural: {label: '自然提升', enhancement_enabled: true,  nn_clut_enabled: true,  tone_enabled: true,  tone_strength: 0.30, nn_high_clip_guard: 5,  drc_mode: 'auto', drc_strength: 512, ldci_mode: 'auto'},
    bypass:  {label: '直通旁路', enhancement_enabled: false, nn_clut_enabled: false, tone_enabled: false, tone_strength: 0.0,  nn_high_clip_guard: 3,  drc_mode: 'off',  drc_strength: 0,   ldci_mode: 'off'},
    custom:  {label: '手动配置', enhancement_enabled: true,  nn_clut_enabled: true,  tone_enabled: true,  tone_strength: 0.25, nn_high_clip_guard: 3,  drc_mode: 'auto', drc_strength: 512, ldci_mode: 'auto'}
  };

  var curHot = {};       // latest from SSE
  var hotDebounce = null;
  var HOT_DEBOUNCE_MS = 200;

  function buildHotControls() {
    var html = '';
    hotDefs.forEach(function(d) {
      if (d.type === 'range') {
        html += '<div class="ctrl-row">' +
          '<label>' + d.label + '</label>' +
          '<input type="range" id="hc-' + d.key + '" min="' + d.min + '" max="' + d.max +
          '" step="' + d.step + '" value="' + (curHot[d.key] != null ? curHot[d.key] : d.min) + '">' +
          '<span class="val" id="hv-' + d.key + '"></span>' +
          '</div>';
      } else if (d.type === 'checkbox') {
        html += '<div class="ctrl-row">' +
          '<label>' + d.label + '</label>' +
          '<input type="checkbox" id="hc-' + d.key + '"' + (curHot[d.key] ? ' checked' : '') + '>' +
          '<span class="val" id="hv-' + d.key + '"></span>' +
          '</div>';
      } else if (d.type === 'select') {
        html += '<div class="ctrl-row">' +
          '<label>' + d.label + '</label>' +
          '<select id="hc-' + d.key + '">' +
          d.opts.map(function(o) {
            return '<option value="' + o + '"' + (curHot[d.key] === o ? ' selected' : '') + '>' + o + '</option>';
          }).join('') +
          '</select>' +
          '<span class="val" id="hv-' + d.key + '"></span>' +
          '</div>';
      }
    });
    $('hot-controls').innerHTML = html;

    // Wire change events with debounce
    hotDefs.forEach(function(d) {
      var el = $('hc-' + d.key);
      if (!el) return;
      el.addEventListener('input', function() {
        updateHotVal(d);
        scheduleHotApply();
      });
      el.addEventListener('change', function() {
        updateHotVal(d);
      });
      // Initial display
      updateHotVal(d);
    });
  }

  function buildPresetBar() {
    var html = '';
    Object.keys(presets).forEach(function(k) {
      var p = presets[k];
      html += '<button id="pr-' + k + '" onclick="applyPreset(\'' + k + '\')">' + p.label + '</button>';
    });
    $('preset-bar').innerHTML = html;
  }

  function updateHotVal(d) {
    var el = $('hc-' + d.key);
    var vl = $('hv-' + d.key);
    if (!el || !vl) return;
    var v;
    if (d.type === 'checkbox') {
      v = el.checked;
      vl.textContent = v ? '开' : '关';
    } else if (d.type === 'select') {
      v = el.value;
      vl.textContent = v;
    } else {
      v = parseFloat(el.value);
      vl.textContent = v.toFixed(d.step < 1 ? 2 : 1);
    }
  }

  window.applyPreset = function(key) {
    var p = presets[key];
    if (!p) return;
    hotDefs.forEach(function(d) {
      if (p[d.key] != null) {
        curHot[d.key] = p[d.key];
        var el = $('hc-' + d.key);
        if (!el) return;
        if (d.type === 'checkbox') el.checked = p[d.key];
        else el.value = p[d.key];
        updateHotVal(d);
      }
    });
    // Highlight active preset
    Object.keys(presets).forEach(function(k) {
      var btn = $('pr-' + k);
      if (btn) btn.className = (k === key) ? 'active' : '';
    });
    sendHotParams();
    $('hot-status').textContent = '预设: ' + p.label;
    setTimeout(function() { $('hot-status').textContent = ''; }, 2000);
  };

  function readHotParams() {
    var p = {};
    hotDefs.forEach(function(d) {
      var el = $('hc-' + d.key);
      if (!el) return;
      if (d.type === 'checkbox') p[d.key] = el.checked;
      else if (d.type === 'select') p[d.key] = el.value;
      else p[d.key] = parseFloat(el.value);
    });
    return p;
  }

  function scheduleHotApply() {
    if (hotDebounce) clearTimeout(hotDebounce);
    hotDebounce = setTimeout(sendHotParams, HOT_DEBOUNCE_MS);
  }

  function sendHotParams() {
    if (hotDebounce) { clearTimeout(hotDebounce); hotDebounce = null; }
    var p = readHotParams();
    curHot = p;
    $('hot-status').textContent = '应用...';
    fetch('/api/v1/control', {
      method: 'PATCH',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(p)
    })
    .then(function(r) { return r.json(); })
    .then(function(d) {
      if (d.error) { $('hot-status').textContent = '失败: ' + d.error; }
      else { $('hot-status').textContent = '已应用 ✓'; }
      setTimeout(function() { $('hot-status').textContent = ''; }, 2000);
    })
    .catch(function() {
      $('hot-status').textContent = '请求失败';
      setTimeout(function() { $('hot-status').textContent = ''; }, 3000);
    });
  }

  // Wire apply button
  function wireHotApply() {
    $('btn-hot-apply').addEventListener('click', sendHotParams);
  }

  // Sync controls from SSE data (one-way: backend → controls only when user hasn't touched them)
  function syncHotFromStatus(data) {
    var pr = data.processing || {};
    var changed = false;
    hotDefs.forEach(function(d) {
      var mapped = d.key; // same name as SSE field
      if (d.key === 'tone_strength') mapped = 'tone_strength';
      if (d.key === 'high_clip_guard') mapped = 'high_clip_guard';
      if (d.key === 'nn_clut_enabled') mapped = 'nn_enabled';
      // Only sync if value actually changed and user isn't mid-edit
      if (pr[mapped] != null && curHot[d.key] !== pr[mapped]) {
        curHot[d.key] = pr[mapped];
        var el = $('hc-' + d.key);
        if (el && document.activeElement !== el) {
          if (d.type === 'checkbox') el.checked = pr[mapped];
          else el.value = pr[mapped];
          updateHotVal(d);
        }
      }
    });
  }

  // ── 冷配置 (W2) ──
  function loadConfig() {
    fetch('/api/v1/config')
      .then(function(r) { return r.json(); })
      .then(function(d) {
        if (d.error) { $('cfg-status').textContent = d.error; return; }
        var c = d.config || {};
        $('cfg-mode').value = c.CAPTURE_MODE || 'linear';
        $('cfg-fps').value = c.TARGET_FPS || '30';
        $('cfg-bitrate').value = c.BITRATE_KBPS || '3000';
        $('cfg-hdmi').value = c.ENABLE_HDMI || '0';
        $('cfg-status').textContent = 'gen ' + (d.generation || 0);
      })
      .catch(function() { $('cfg-status').textContent = 'admin 未连接'; });
  }

  // ── 冷配置进度 (§8 冷配置进度条) ──
  var CFG_STEPS = ['校验', '保存', '重启', '等待相机', '等待视频', '健康检查'];
  var cfgStep = 0;
  var cfgStepTimer = null;

  function showProgress(text, cls) {
    var el = $('cfg-progress');
    el.classList.remove('hidden', 'done', 'fail');
    if (cls) el.classList.add(cls);
    $('cfg-progress-text').textContent = text;
  }
  function hideProgress() {
    var el = $('cfg-progress');
    el.classList.add('hidden');
    el.classList.remove('done', 'fail');
    if (cfgStepTimer) { clearInterval(cfgStepTimer); cfgStepTimer = null; }
    cfgStep = 0;
  }
  function advanceProgress() {
    if (cfgStep < CFG_STEPS.length) {
      showProgress('正在' + CFG_STEPS[cfgStep] + '… (' + (cfgStep+1) + '/' + CFG_STEPS.length + ')');
      cfgStep++;
    }
  }
  function errRecovery(errMsg) {
    var hint = '';
    if (errMsg.indexOf('check-config') >= 0) hint = '建议：检查配置值是否在合法范围内';
    else if (errMsg.indexOf('restart') >= 0) hint = '建议：确认 systemd 服务状态，或尝试回滚';
    else if (errMsg.indexOf('health') >= 0) hint = '建议：等待服务完全启动后重试，检查相机连接';
    else if (errMsg.indexOf('unavailable') >= 0) hint = '建议：确认 admin 服务正在运行 (systemctl status socchina-admin)';
    else hint = '建议：尝试回滚到上次正常配置，或检查板端日志';
    return hint;
  }

  $('btn-apply').addEventListener('click', function() {
    if (!confirm('冷配置会重启视频流，画面将中断约 10 秒。确定继续？')) return;
    var cfg = {
      CONFIG_VERSION: '1',
      CAPTURE_MODE: $('cfg-mode').value,
      TARGET_FPS: $('cfg-fps').value,
      BITRATE_KBPS: $('cfg-bitrate').value,
      ENABLE_HDMI: $('cfg-hdmi').value,
      RTSP_BIND_ADDR: '127.0.0.1',
      RTSP_PORT: '8555',
      STREAM_PATH: 'internal',
      SENSOR_INDEX: '1',
      NN_HIGH_CLIP_GUARD: '3.0',
      MODEL_PATH: '/root/socchina-2026/cotf_paramnet_256x144_lcdp_best_e0167_fp16_aipp.om'
    };

    // Start progress animation (§8 冷配置进度)
    cfgStep = 0;
    showProgress('正在校验… (1/' + CFG_STEPS.length + ')');
    cfgStepTimer = setInterval(advanceProgress, 2500);

    fetch('/api/v1/config/apply', {method: 'POST', body: JSON.stringify(cfg)})
      .then(function(r) { return r.json(); })
      .then(function(d) {
        hideProgress();
        if (d.error) {
          $('cfg-status').textContent = '失败: ' + d.error;
          showProgress('失败: ' + d.error + ' — ' + errRecovery(d.error));
        } else {
          $('cfg-status').textContent = '已应用 ✓';
          showProgress('完成 ✓ — 视频流将在数秒内恢复');
          setTimeout(hideProgress, 5000);
        }
        loadConfig();
      })
      .catch(function() {
        hideProgress();
        $('cfg-status').textContent = '请求失败';
        showProgress('请求失败 — ' + errRecovery('unavailable'));
      });
  });

  $('btn-rollback').addEventListener('click', function() {
    if (!confirm('回滚到上次正常配置并重启？')) return;
    showProgress('回滚中…');
    fetch('/api/v1/config/rollback', {method: 'POST'})
      .then(function(r) { return r.json(); })
      .then(function(d) {
        $('cfg-status').textContent = d.error ? '回滚失败: ' + d.error : '已回滚 ✓';
        hideProgress();
        loadConfig();
      })
      .catch(function() { $('cfg-status').textContent = '请求失败'; hideProgress(); });
  });

  // ── 参数侧边栏 显示/隐藏 ──
  // 语义:body.sidebar-hidden 在宽屏 = 移除侧边栏(主区占满);
  //       在窄屏(≤900px) = 抽屉关闭。二者共用一个 class。
  var NARROW = window.matchMedia('(max-width: 900px)');
  function toggleSidebar() { document.body.classList.toggle('sidebar-hidden'); }
  function closeSidebar() { document.body.classList.add('sidebar-hidden'); }
  function initSidebar() {
    // 窄屏默认关闭抽屉;宽屏默认显示侧边栏
    if (NARROW.matches) document.body.classList.add('sidebar-hidden');
    var t = $('btn-sidebar-toggle'); if (t) t.addEventListener('click', toggleSidebar);
    var c = $('btn-sidebar-close');  if (c) c.addEventListener('click', closeSidebar);
    var b = $('sidebar-backdrop');   if (b) b.addEventListener('click', closeSidebar);
    // 视口跨越断点时,恢复该断点的默认可见状态,避免状态错乱
    NARROW.addEventListener('change', function (e) {
      if (e.matches) document.body.classList.add('sidebar-hidden');
      else document.body.classList.remove('sidebar-hidden');
    });
  }

  // ── 启动 ──
  tick();
  setInterval(tick, 1000);
  initSidebar();
  buildPresetBar();
  buildHotControls();
  wireHotApply();
  connectSSE();
  initVideo();
  loadConfig();
  $('footer-version').textContent = 'SOCCHINA · SS928 CoTF';
})();
