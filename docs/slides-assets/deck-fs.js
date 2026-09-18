(function (global) {
  var KEY = "xrd-slides-fs";
  var leaving = false;
  var embed = null;
  var FOLDERS = ["/overview/", "/oidc/", "/ssh/", "/http/", "/journalcache/", "/nfs/", "/aios/"];
  var SCRIPT_MARK = "overview/slides-assets/deck-fs.js";

  function fsOn() {
    return !!(document.fullscreenElement || document.webkitFullscreenElement);
  }
  function fsEnabled() {
    var el = document.documentElement;
    return !!(el.requestFullscreen || el.webkitRequestFullscreen);
  }
  function inFrame() {
    try { return window.self !== window.top; } catch (e) { return true; }
  }
  function wanted() {
    try { return sessionStorage.getItem(KEY) === "1"; } catch (e) { return false; }
  }
  function setWanted(on) {
    try { sessionStorage.setItem(KEY, on ? "1" : "0"); } catch (e) {}
  }
  function carry() {
    return fsOn();
  }
  function themeName() {
    return document.documentElement.classList.contains("light") ? "light" : "dark";
  }
  function isIndex() {
    return document.documentElement.getAttribute("data-xrd-root") === "./";
  }
  function workshopRoot() {
    var attr = document.documentElement.getAttribute("data-xrd-root");
    if (attr) return new URL(attr, location.href).href;
    var scripts = document.getElementsByTagName("script");
    for (var i = 0; i < scripts.length; i++) {
      var src = scripts[i].src || "";
      var n = src.indexOf(SCRIPT_MARK);
      if (n !== -1) return src.slice(0, n);
    }
    var path = location.pathname;
    for (var j = 0; j < FOLDERS.length; j++) {
      var p = path.indexOf(FOLDERS[j]);
      if (p !== -1) return location.origin + path.slice(0, p + 1);
    }
    if (/\/index\.html$/.test(path)) {
      return location.origin + path.slice(0, path.lastIndexOf("/") + 1);
    }
    if (/\/$/.test(path)) return location.origin + path;
    return new URL("./", location.href).href;
  }
  function enter() {
    if (!fsEnabled() || fsOn() || inFrame()) return Promise.resolve();
    var el = document.documentElement;
    var req = el.requestFullscreen || el.webkitRequestFullscreen;
    try {
      var p = req.call(el);
      return p && p.catch ? p.catch(function () {}) : Promise.resolve();
    } catch (e) {
      return Promise.resolve();
    }
  }
  function talkHref(src, theme, embedNav) {
    var url = new URL(src, workshopRoot());
    url.search = "";
    url.hash = "";
    url.searchParams.set("theme", theme);
    if (carry() && !embedNav && !inFrame()) url.searchParams.set("fs", "1");
    return url.href;
  }
  function rewriteLinks() {
    var theme = themeName();
    document.querySelectorAll("a.talk, a.talk-link").forEach(function (a) {
      var src = a.getAttribute("data-src");
      if (src) a.href = talkHref(src, theme);
    });
    document.querySelectorAll("a.home-index").forEach(function (a) {
      a.href = talkHref("index.html", theme);
    });
  }
  function ensureCss() {
    if (document.getElementById("xrd-fs-embed-css")) return;
    var style = document.createElement("style");
    style.id = "xrd-fs-embed-css";
    style.textContent = "iframe.xrd-fs-embed{position:fixed;inset:0;width:100%;height:100%;border:0;z-index:99999;background:#000}";
    document.head.appendChild(style);
  }
  function closeEmbed() {
    if (!embed) return;
    embed.remove();
    embed = null;
  }
  function openEmbed(url) {
    ensureCss();
    closeEmbed();
    embed = document.createElement("iframe");
    embed.className = "xrd-fs-embed";
    embed.title = "Presentation";
    embed.src = url;
    embed.setAttribute("allow", "fullscreen");
    document.body.appendChild(embed);
  }
  function goIndex() {
    if (inFrame()) {
      try { window.top.postMessage({ xrdHome: true }, location.origin); } catch (e) {}
      return;
    }
    closeEmbed();
    if (isIndex()) return;
    location.assign(talkHref("index.html", themeName()));
  }
  function onNavClick(e) {
    if (e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
    var a = e.target.closest && e.target.closest("a.talk, a.talk-link, a.home-index");
    if (!a) return;
    var src = a.getAttribute("data-src");
    if (!src && a.classList.contains("home-index")) src = "index.html";
    if (!src) return;
    e.preventDefault();
    if (src === "index.html" || a.classList.contains("home-index")) {
      goIndex();
      return;
    }
    var theme = themeName();
    if (fsOn() && !inFrame()) {
      openEmbed(talkHref(src, theme, true));
      return;
    }
    location.assign(talkHref(src, theme, inFrame()));
  }
  function stripFsParam() {
    try {
      var url = new URL(location.href);
      if (url.searchParams.get("fs") !== "1") return;
      url.searchParams.delete("fs");
      history.replaceState(null, "", url.pathname + url.search + url.hash);
    } catch (e) {}
  }
  function onFs() {
    if (fsOn()) setWanted(true);
    else if (!leaving) {
      setWanted(false);
      stripFsParam();
    }
  }
  function isInteractive(el) {
    return !!(el && el.closest && el.closest("a, button, input, select, textarea"));
  }
  function retry(e) {
    if (!wanted() || fsOn()) return;
    if (e && isInteractive(e.target)) return;
    enter();
  }
  function pad2(n) { return n < 10 ? "0" + n : String(n); }
  function fillSlideVer() {
    var d = new Date(document.lastModified);
    if (isNaN(d.getTime())) d = new Date();
    var text = d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
      + " " + pad2(d.getHours()) + ":" + pad2(d.getMinutes());
    document.querySelectorAll(".slide-ver").forEach(function (el) {
      el.textContent = text;
      try { el.dateTime = d.toISOString(); } catch (e) {}
    });
  }
  function bind() {
    fillSlideVer();
    rewriteLinks();
    document.addEventListener("click", onNavClick, true);
    window.addEventListener("message", function (e) {
      if (e.origin !== location.origin) return;
      if (e.data && e.data.xrdHome) goIndex();
    });
    window.addEventListener("pagehide", function () { leaving = true; });
    document.addEventListener("fullscreenchange", onFs);
    document.addEventListener("webkitfullscreenchange", onFs);
    var urlWants = false;
    try { urlWants = new URLSearchParams(location.search).get("fs") === "1"; } catch (e) {}
    if (urlWants && !inFrame()) {
      setWanted(true);
      enter();
      document.addEventListener("pointerdown", retry, true);
      document.addEventListener("keydown", retry, true);
    } else if (!fsOn()) {
      setWanted(false);
    }
  }

  global.XrdDeckFs = {
    bind: bind,
    carry: carry,
    talkHref: talkHref,
    wanted: wanted,
    workshopRoot: workshopRoot
  };
})(window);
