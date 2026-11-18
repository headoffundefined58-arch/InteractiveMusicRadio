/* InteractiveMusicRadio - front-end controller (2.2).
 *
 * Talks to the SKSE side through the PrismaUI bridge.  The contract:
 *
 *   C++ -> JS:  window.imrUpdate(payload)      payload = JSON *string*
 *   JS  -> C++: window.imrCommand(text)        text = "verb\nkey=value\n..."
 *
 * The payload is a string, not an object, on purpose: C++ hands it over through
 * InteropCall's string argument, so a song title with a quote or a backslash in
 * it can never produce broken JavaScript source.
 *
 * Two source kinds only: kind 0 = 桌面版 (a music program), kind 1 = 网页版
 * (the same service in the browser).  Desktop / web each own their own lyric
 * window, so switching cards never reuses the other's placement or lyric.
 *
 * 2.2 rendering: the bridge pushes a snapshot every 100 ms.  Rebuilding both
 * source lists on every push was the single largest source of UI latency, so
 * the lists / static rows are now rebuilt only when their data signature
 * changes; the progress bar is the only per-frame write.  A pending seek keeps
 * the thumb at the chosen point until C++ confirms the jump, which stops the
 * bar from visibly snapping back to the old SMTC position.
 */
(function () {
  "use strict";

  var META = {
    "qqmusic":     { name: "QQ音乐",            short: "Q",  tag: "桌面" },
    "netease":     { name: "网易云音乐",        short: "云", tag: "桌面" },
    "soda":        { name: "汽水音乐",          short: "汽", tag: "桌面" },
    "web-qqmusic": { name: "QQ音乐 网页版",     short: "Q",  tag: "网页" },
    "web-netease": { name: "网易云音乐 网页版", short: "云", tag: "网页" },
    "web-soda":    { name: "汽水音乐 网页版",   short: "汽", tag: "网页" }
  };

  var MODE_NAMES = ["随机播放", "顺序播放", "单曲循环", "列表循环"];

  var state = {
    sources: [],
    active: "",
    lyrics: false,
    pm: -1,          // PlayMode of the linked source; -1 = player has none
    px: 0, py: 0,    // panel offset from its centred default
    lk: true,        // lyric window locked (click-through)
    lkind: 0,        // 0 desktop / 1 web - which lyric window is active
    subsrc: 0,       // 0 none / 1 video OCR / 2 network LRC
    vsen: 1,         // video-subtitle master toggle
    vsavail: 0,      // OCR language pack available
    vsactive: 0,     // capture worker running
    subtext: "",     // freshest OCR subtitle line
    hotkey: "F10",
    bind: 0,         // 1 = recording a new hotkey
    offset: 0,       // lyric sync nudge, ms
    seeking: false,
    seekPct: 0,
    pendingSeek: null,  // { pos, at } - keeps the thumb put until C++ confirms
    demo: false
  };

  var sawUpdate = false;
  var demoTimer = 0;
  var drag = null;     // panel drag in progress
  var lastListSig = "";
  var lastSettingsSig = "";

  var el = {
    app: document.getElementById("app"),
    dragHandle: document.getElementById("dragHandle"),
    desktop: document.getElementById("desktopList"),
    web: document.getElementById("webList"),
    badge: document.getElementById("linkBadge"),
    label: document.getElementById("linkLabel"),
    npTitle: document.getElementById("npTitle"),
    npMeta: document.getElementById("npMeta"),
    npBar: document.getElementById("npBar"),
    npProgress: document.getElementById("npProgress"),
    npThumb: document.getElementById("npThumb"),
    npElapsed: document.getElementById("npElapsed"),
    npDuration: document.getElementById("npDuration"),
    prevBtn: document.getElementById("prevBtn"),
    playBtn: document.getElementById("playBtn"),
    playIcon: document.getElementById("playIcon"),
    nextBtn: document.getElementById("nextBtn"),
    lyricsSwitch: document.getElementById("lyricsSwitch"),
    lyricsLockSwitch: document.getElementById("lyricsLockSwitch"),
    lyricKind: document.getElementById("lyricKind"),
    subTag: document.getElementById("subTag"),
    videoSubSwitch: document.getElementById("videoSubSwitch"),
    subState: document.getElementById("subState"),
    subPreview: document.getElementById("subPreview"),
    subPreviewTag: document.getElementById("subPreviewTag"),
    subPreviewText: document.getElementById("subPreviewText"),
    offsetValue: document.getElementById("offsetValue"),
    offsetMinus: document.getElementById("offsetMinus"),
    offsetPlus: document.getElementById("offsetPlus"),
    hotkeyBtn: document.getElementById("hotkeyBtn"),
    hotkeyReset: document.getElementById("hotkeyReset"),
    volumeSlider: document.getElementById("volumeSlider"),
    volumeValue: document.getElementById("volumeValue"),
    closeBtn: document.getElementById("closeBtn"),
    modeBtn: document.getElementById("modeBtn"),
    modeText: document.getElementById("modeText"),
    modeMenu: document.getElementById("modeMenu"),
    modeItems: Array.prototype.slice.call(document.querySelectorAll(".mode-item"))
  };

  /* ---------------------------------------------------------------- bridge -- */

  function send(text) {
    if (typeof window.imrCommand === "function") {
      window.imrCommand(text);
      return true;
    }
    return false;
  }

  // "verb\nkey=value\n…" - newline separated, so a value may contain anything.
  function sendSimple(verb, kv) {
    var lines = [verb];
    if (kv) {
      Object.keys(kv).forEach(function (k) { lines.push(k + "=" + kv[k]); });
    }
    return send(lines.join("\n"));
  }

  function linkedSource() {
    for (var i = 0; i < state.sources.length; i++) {
      if (state.sources[i].id === state.active) return state.sources[i];
    }
    return null;
  }

  function fmtTime(t) {
    t = Math.max(0, Math.floor(t || 0));
    var m = Math.floor(t / 60), s = t % 60;
    return m + ":" + (s < 10 ? "0" : "") + s;
  }

  /* ---------------------------------------------------------------- render -- */

  function buildRow(s) {
    var meta = META[s.id] || { name: s.id, short: "?", tag: "" };

    var li = document.createElement("li");
    if (!s.run) li.className = "idle";
    if (s.id === state.active) li.className += " linked";

    var icon = document.createElement("div");
    icon.className = "s-icon";
    icon.textContent = meta.short;

    var body = document.createElement("div");
    body.className = "s-body";

    var name = document.createElement("div");
    name.className = "s-name";
    name.appendChild(document.createTextNode(meta.name));
    var tag = document.createElement("span");
    tag.className = "s-tag";
    tag.textContent = meta.tag;
    name.appendChild(tag);

    var info = document.createElement("div");
    info.className = "s-meta";
    if (s.title) {
      var t = document.createElement("b");
      t.textContent = s.title;
      info.appendChild(t);
      if (s.artist) info.appendChild(document.createTextNode(" · " + s.artist));
    } else if (s.run) {
      info.appendChild(document.createTextNode("等待播放"));
    } else {
      info.appendChild(document.createTextNode("未检测到正在运行的软件"));
    }

    body.appendChild(name);
    body.appendChild(info);

    var st = document.createElement("div");
    st.className = "s-state";
    if (!s.run) st.textContent = "未启动";
    else if (s.id === state.active) st.textContent = s.playing ? "已连接·播放" : "已连接";
    else if (s.ok) st.textContent = s.playing ? "播放中" : "已暂停";
    else st.textContent = "运行中";

    li.appendChild(icon);
    li.appendChild(body);
    li.appendChild(st);

    li.addEventListener("click", function () {
      sendSimple("select", { id: s.id });
      state.active = s.id;
      render();
    });
    return li;
  }

  function renderList(host, kind) {
    host.innerHTML = "";
    var any = false;
    for (var i = 0; i < state.sources.length; i++) {
      var s = state.sources[i];
      if (s.kind !== kind) continue;
      host.appendChild(buildRow(s));
      any = true;
    }
    if (!any) {
      var li = document.createElement("li");
      li.className = "idle";
      var body = document.createElement("div");
      body.className = "s-body";
      var txt = document.createElement("div");
      txt.className = "s-meta";
      txt.textContent = kind === 0 ? "没有可用的桌面音源" : "没有可用的网页音源";
      body.appendChild(txt);
      li.appendChild(body);
      host.appendChild(li);
    }
  }

  function renderNowPlaying() {
    var s = linkedSource();
    if (!s) {
      el.npTitle.textContent = "尚未接入音乐源";
      el.npMeta.textContent = "点击左侧或右侧任意一个音源开始";
      return;
    }
    el.npTitle.textContent = s.title || "等待音乐软件开始播放";
    var parts = [];
    if (s.artist) parts.push(s.artist);
    if (s.album) parts.push("《" + s.album + "》");
    el.npMeta.textContent = parts.length ? parts.join("  ·  ") : "—";
  }

  function renderProgress() {
    var s = linkedSource();
    var dur = s ? (s.dur || 0) : 0;
    var pct = 0;
    var pos = 0;
    if (state.seeking) {
      pct = state.seekPct * 100;
      pos = state.seekPct * dur;
    } else if (state.pendingSeek && dur > 1) {
      // C++ has not confirmed the jump yet (or the player is slow to report
      // it): keep showing the point the user chose and let it run forward.
      pos = Math.min(dur, state.pendingSeek.pos +
        (Date.now() - state.pendingSeek.at) / 1000);
      pct = (pos / dur) * 100;
    } else if (s && dur > 1 && s.pos >= 0) {
      pct = Math.max(0, Math.min(100, (s.pos / dur) * 100));
      pos = s.pos;
    }
    el.npProgress.style.width = pct + "%";
    el.npThumb.style.left = pct + "%";
    el.npElapsed.textContent = fmtTime(pos);
    el.npDuration.textContent = fmtTime(dur);
  }

  function renderBadge() {
    el.badge.className = "link-badge";
    var s = linkedSource();
    if (state.demo) {
      el.badge.className += " listening";
      el.label.textContent = "预览模式 · 未接入游戏";
      return;
    }
    if (s && s.ok) {
      el.badge.className += " linked";
      el.label.textContent = "已接管 · 本地音乐让位";
    } else if (s) {
      el.badge.className += " listening";
      el.label.textContent = "已选择 · 等待音乐软件";
    } else {
      el.label.textContent = "未连接";
    }
  }

  function renderMode() {
    var s = linkedSource();
    var known = !!(s && state.pm >= 0 && state.pm < MODE_NAMES.length);
    el.modeText.textContent = known ? MODE_NAMES[state.pm] : "播放模式";
    el.modeBtn.classList.toggle("on", known);
    el.modeBtn.disabled = !s;
    for (var i = 0; i < el.modeItems.length; i++) {
      var b = el.modeItems[i];
      var m = parseInt(b.getAttribute("data-mode"), 10);
      b.classList.toggle("on", known && m === state.pm);
      b.disabled = !s;
    }
  }

  function renderSettings() {
    el.lyricsSwitch.checked = !!state.lyrics;
    el.lyricsLockSwitch.checked = !!state.lk;
    el.lyricKind.textContent = state.lkind === 1 ? "网页版" : "桌面版";
    var subText = state.subsrc === 1 ? "歌词：视频字幕"
      : state.subsrc === 2 ? "歌词：网络" : "歌词：—";
    el.subTag.textContent = subText;
    el.subTag.classList.toggle("video", state.subsrc === 1);

    // Video-subtitle engine state - a single line that always explains what
    // the capture is doing, so a dead capture is never a mystery.
    el.videoSubSwitch.checked = !!state.vsen;
    var st = "";
    var stClass = "";
    if (!state.vsen) { st = "字幕：已关闭"; }
    else if (!state.vsavail) { st = "字幕：缺中文 OCR"; stClass = "err"; }
    else if (state.lkind !== 1) { st = "字幕：接入网页源后启用"; }
    else if (!state.lyrics) { st = "字幕：开桌面歌词后启用"; }
    else if (state.vsactive) { st = state.subsrc === 1 ? "字幕：识别中" : "字幕：等待画面"; stClass = state.subsrc === 1 ? "live" : ""; }
    else { st = "字幕：寻找播放窗口"; }
    el.subState.textContent = st;
    el.subState.className = "sub-state" + (stClass ? " " + stClass : "");

    // Live OCR line preview: only while a video subtitle is actually showing.
    if (state.subsrc === 1 && state.subtext) {
      el.subPreview.hidden = false;
      el.subPreviewText.textContent = state.subtext;
      el.subPreviewTag.textContent = "视频字幕";
    } else {
      el.subPreview.hidden = true;
    }

    el.offsetValue.textContent = (state.offset >= 0 ? "+" : "") +
      (state.offset / 1000).toFixed(2) + "s";
    el.hotkeyBtn.textContent = state.bind === 1
      ? "按下新热键…（Esc 取消）"
      : "⌨ 热键：" + (state.hotkey || "未设置");
    el.hotkeyBtn.classList.toggle("recording", state.bind === 1);
  }

  function renderControls() {
    var s = linkedSource();
    var canControl = !!(s && s.ok);
    el.prevBtn.disabled = !canControl;
    el.nextBtn.disabled = !canControl;
    el.playBtn.disabled = !canControl;

    el.playIcon.innerHTML = (s && s.playing)
      ? '<path d="M7 5h4v14H7zM13 5h4v14h-4z"/>'
      : '<path d="M8 5v14l11-7z"/>';

    if (s && typeof s.vol === "number") {
      el.volumeSlider.value = Math.round(s.vol * 100);
      el.volumeValue.textContent = el.volumeSlider.value + "%";
    }
  }

  function applyPanel() {
    el.app.style.transform = "translate(" + state.px + "px," + state.py + "px)";
  }

  // Everything that decides the *structure* of the two lists and the static
  // rows.  Position/duration are deliberately excluded: they change on every
  // push and are handled by renderProgress() alone.
  function listSignature() {
    var p = [state.active, state.demo ? "1" : "0", state.pm];
    for (var i = 0; i < state.sources.length; i++) {
      var s = state.sources[i];
      p.push([s.id, s.kind, s.run ? 1 : 0, s.ok ? 1 : 0, s.title, s.artist,
        s.album, s.playing ? 1 : 0, s.vol, s.muted ? 1 : 0].join("~"));
    }
    return p.join("|");
  }

  function settingsSignature() {
    return [state.lyrics ? 1 : 0, state.lk ? 1 : 0, state.lkind, state.subsrc,
      state.vsen ? 1 : 0, state.vsavail ? 1 : 0, state.vsactive ? 1 : 0,
      state.subtext, state.bind, state.hotkey, state.offset].join("|");
  }

  function render() {
    var sig = listSignature();
    if (sig !== lastListSig) {
      lastListSig = sig;
      renderList(el.desktop, 0);
      renderList(el.web, 1);
      renderNowPlaying();
      renderBadge();
      renderControls();
      renderMode();
    }
    renderProgress();
    var ss = settingsSignature();
    if (ss !== lastSettingsSig) {
      lastSettingsSig = ss;
      renderSettings();
    }
  }

  /* -------------------------------------------------------- C++ -> JS entry -- */

  window.imrUpdate = function (raw) {
    var d = raw;
    if (typeof raw === "string") {
      try { d = JSON.parse(raw); } catch (e) { return; }
    }
    if (!d) return;

    sawUpdate = true;
    if (demoTimer) { clearTimeout(demoTimer); demoTimer = 0; }
    state.demo = false;

    state.sources = d.sources || [];
    state.active = d.active || "";
    state.lyrics = d.lyrics === 1 || d.lyrics === true;
    state.pm = (typeof d.pm === "number") ? d.pm : -1;
    state.lk = d.lk !== 0 && d.lk !== false;
    state.lkind = d.lkind === 1 ? 1 : 0;
    state.subsrc = typeof d.subsrc === "number" ? d.subsrc : 0;
    state.vsen = d.vsen !== 0 && d.vsen !== false;
    state.vsavail = d.vsavail === 1 || d.vsavail === true;
    state.vsactive = d.vsactive === 1 || d.vsactive === true;
    state.subtext = typeof d.subtext === "string" ? d.subtext : "";
    state.bind = d.bind === 1 ? 1 : 0;
    state.hotkey = d.hotkey || "未设置";
    state.offset = typeof d.offset === "number" ? d.offset : 0;

    // Drop the pending-seek guard once the reported position catches up with
    // the chosen point (or after 3 s, so a refused seek cannot freeze the bar).
    if (state.pendingSeek) {
      var s = linkedSource();
      var guess = state.pendingSeek.pos +
        (Date.now() - state.pendingSeek.at) / 1000;
      if ((s && typeof s.pos === "number" && Math.abs(s.pos - guess) < 1.5) ||
          Date.now() - state.pendingSeek.at > 3000) {
        state.pendingSeek = null;
      }
    }

    // Do not yank the card back to its saved place while it is being dragged.
    if (!drag) {
      state.px = typeof d.px === "number" ? d.px : 0;
      state.py = typeof d.py === "number" ? d.py : 0;
      applyPanel();
    }
    render();
  };

  /* -------------------------------------------------------- JS -> C++ entry -- */

  el.prevBtn.addEventListener("click", function () { sendSimple("transport", { act: "prev" }); });
  el.nextBtn.addEventListener("click", function () { sendSimple("transport", { act: "next" }); });
  el.playBtn.addEventListener("click", function () { sendSimple("transport", { act: "toggle" }); });

  el.lyricsSwitch.addEventListener("change", function () {
    state.lyrics = el.lyricsSwitch.checked;
    sendSimple("lyrics", { on: state.lyrics ? 1 : 0 });
  });

  el.videoSubSwitch.addEventListener("change", function () {
    state.vsen = el.videoSubSwitch.checked;
    sendSimple("videosub", { on: state.vsen ? 1 : 0 });
    renderSettings();
  });

  el.lyricsLockSwitch.addEventListener("change", function () {
    state.lk = el.lyricsLockSwitch.checked;
    sendSimple("lyricslock", { on: state.lk ? 1 : 0 });
  });

  el.offsetMinus.addEventListener("click", function () {
    state.offset -= 250;
    renderSettings();
    sendSimple("lyricoffset", { delta: -250 });
  });
  el.offsetPlus.addEventListener("click", function () {
    state.offset += 250;
    renderSettings();
    sendSimple("lyricoffset", { delta: 250 });
  });

  el.hotkeyBtn.addEventListener("click", function (e) {
    e.stopPropagation();
    if (state.bind === 1) {
      state.bind = 0;
      sendSimple("hotkey", { act: "cancel" });
    } else {
      state.bind = 1;
      sendSimple("hotkey", { act: "capture" });
    }
    renderSettings();
  });
  el.hotkeyReset.addEventListener("click", function (e) {
    e.stopPropagation();
    state.bind = 0;
    state.hotkey = "F10";
    sendSimple("hotkey", { act: "reset" });
    renderSettings();
  });

  el.volumeSlider.addEventListener("input", function () {
    var v = parseInt(el.volumeSlider.value, 10) / 100;
    el.volumeValue.textContent = el.volumeSlider.value + "%";
    if (state.active) sendSimple("volume", { v: v.toFixed(2), mute: 0 });
  });

  el.closeBtn.addEventListener("click", function () { sendSimple("close"); });

  /* ----------------------------------------------------------- play mode -- */
  el.modeBtn.addEventListener("click", function (e) {
    e.stopPropagation();
    el.modeMenu.hidden = !el.modeMenu.hidden;
  });
  document.addEventListener("click", function () { el.modeMenu.hidden = true; });
  el.modeItems.forEach(function (btn) {
    btn.addEventListener("click", function (e) {
      e.stopPropagation();
      var mode = parseInt(btn.getAttribute("data-mode"), 10);
      if (isNaN(mode) || mode < 0 || mode >= MODE_NAMES.length) return;
      sendSimple("playmode", { mode: mode });
      state.pm = mode;
      renderMode();
      el.modeMenu.hidden = true;
    });
  });

  /* ------------------------------------------------------- panel dragging -- */
  el.dragHandle.addEventListener("mousedown", function (e) {
    if (e.target && e.target.tagName === "BUTTON") return;
    drag = { x: e.clientX, y: e.clientY, px: state.px, py: state.py };
    el.app.classList.add("dragging");
    e.preventDefault();
  });

  function pctFromEvent(e) {
    var r = el.npBar.getBoundingClientRect();
    if (!r.width) return 0;
    return Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
  }

  document.addEventListener("mousemove", function (e) {
    if (drag) {
      state.px = drag.px + (e.clientX - drag.x);
      state.py = drag.py + (e.clientY - drag.y);
      // Keep at least a corner of the card reachable at any resolution.
      var maxX = window.innerWidth * 0.45;
      var maxY = window.innerHeight * 0.45;
      state.px = Math.max(-maxX, Math.min(maxX, state.px));
      state.py = Math.max(-maxY, Math.min(maxY, state.py));
      applyPanel();
    }
    if (state.seeking) {
      state.seekPct = pctFromEvent(e);
      renderProgress();
    }
  });

  document.addEventListener("mouseup", function () {
    if (drag) {
      drag = null;
      el.app.classList.remove("dragging");
      sendSimple("movepanel", {
        x: Math.round(state.px), y: Math.round(state.py)
      });
    }
    if (state.seeking) {
      state.seeking = false;
      el.npBar.classList.remove("seeking");
      var s = linkedSource();
      if (s && s.dur > 1) {
        var pos = state.seekPct * s.dur;
        state.pendingSeek = { pos: pos, at: Date.now() };
        sendSimple("seek", { pos: pos.toFixed(2) });
        renderProgress();
      }
    }
  });

  el.npBar.addEventListener("mousedown", function (e) {
    var s = linkedSource();
    if (!s || !s.dur || s.dur <= 1) return;
    state.pendingSeek = null;
    state.seeking = true;
    state.seekPct = pctFromEvent(e);
    el.npBar.classList.add("seeking");
    renderProgress();
    e.preventDefault();
  });

  /* --------------------------------------------------------------- offline -- */
  // Browser preview: show a demo frame so the design can be reviewed without
  // the game.  Deliberately delayed - PrismaUI injects window.imrCommand as
  // part of its own load sequence, which may finish after this script runs.
  demoTimer = setTimeout(function () {
    demoTimer = 0;
    if (sawUpdate) return;
    if (typeof window.imrCommand === "function") return;

    state.demo = true;
    state.sources = [
      { id: "qqmusic", kind: 0, run: 1, ok: 1, title: "晴天", artist: "周杰伦",
        album: "叶惠美", playing: 1, pos: 72, dur: 269, vol: 1 },
      { id: "netease", kind: 0, run: 1, ok: 0, title: "", artist: "", album: "",
        playing: 0, pos: 0, dur: 0, vol: 0.8 },
      { id: "soda", kind: 0, run: 0, ok: 0, title: "", artist: "", album: "",
        playing: 0, pos: 0, dur: 0, vol: 1 },
      { id: "web-qqmusic", kind: 1, run: 1, ok: 0, title: "", artist: "", album: "",
        playing: 0, pos: 0, dur: 0, vol: 1 },
      { id: "web-netease", kind: 1, run: 0, ok: 0, title: "", artist: "", album: "",
        playing: 0, pos: 0, dur: 0, vol: 1 },
      { id: "web-soda", kind: 1, run: 0, ok: 0, title: "", artist: "", album: "",
        playing: 0, pos: 0, dur: 0, vol: 1 }
    ];
    state.active = "qqmusic";
    state.pm = 1;
    state.hotkey = "F10";
    state.vsen = 1;
    state.vsavail = 1;
    render();
  }, 1500);

  render();
})();
