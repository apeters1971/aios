(() => {
  const loginView = document.getElementById("login-view");
  const appView = document.getElementById("app-view");
  const loginForm = document.getElementById("login-form");
  const loginError = document.getElementById("login-error");
  const nodeLabel = document.getElementById("node-label");
  const actionError = document.getElementById("action-error");

  let refreshTimer = null;
  let chartTimer = null;
  let activeTab = "overview";

  const CHART_WINDOW = 60; // samples (~60s at 1 Hz)
  const chartState = {
    last: null, // { t, readOps, writeOps, readBytes, writeBytes }
    iops: [], // { t, read, write }
    bytes: [],
  };

  async function api(path, opts = {}) {
    // x-aios-admin is the CSRF token: cookie-authenticated PUT/POST/DELETE are
    // rejected without it, and a cross-site page cannot add a custom header.
    const res = await fetch(path, {
      credentials: "same-origin",
      headers: { "Content-Type": "application/json", "x-aios-admin": "1", ...(opts.headers || {}) },
      ...opts,
    });
    const text = await res.text();
    let json = null;
    try { json = text ? JSON.parse(text) : null; } catch (_) {}
    return { res, text, json };
  }

  function showLogin(err) {
    clearInterval(refreshTimer);
    refreshTimer = null;
    stopChartSampler();
    chartState.last = null;
    chartState.iops = [];
    chartState.bytes = [];
    appView.classList.add("hidden");
    loginView.classList.remove("hidden");
    if (err) {
      loginError.hidden = false;
      loginError.textContent = err;
    } else {
      loginError.hidden = true;
    }
  }

  function showApp() {
    loginView.classList.add("hidden");
    appView.classList.remove("hidden");
    if (!refreshTimer) refreshTimer = setInterval(() => refresh().catch(() => {}), 5000);
    startChartSampler();
  }

  function setTab(name) {
    activeTab = name;
    document.querySelectorAll(".tab").forEach((t) => {
      t.classList.toggle("active", t.dataset.tab === name);
    });
    document.querySelectorAll(".panel").forEach((p) => {
      p.classList.toggle("hidden", p.id !== `tab-${name}`);
    });
    if (name === "overview" && !appView.classList.contains("hidden")) {
      startChartSampler();
      drawIoCharts();
    } else if (name !== "overview") {
      stopChartSampler();
    }
  }

  function fmt(n) {
    if (n === undefined || n === null) return "—";
    if (typeof n === "number") return n.toLocaleString();
    return String(n);
  }

  function fmtBytes(n) {
    if (n == null || Number.isNaN(Number(n))) return "—";
    const v = Number(n);
    const abs = Math.abs(v);
    const units = ["B", "KiB", "MiB", "GiB", "TiB", "PiB"];
    let i = 0;
    let x = abs;
    while (x >= 1024 && i < units.length - 1) {
      x /= 1024;
      i += 1;
    }
    const sign = v < 0 ? "-" : "";
    const digits = i === 0 ? 0 : x >= 10 ? 1 : 2;
    return sign + x.toFixed(digits) + " " + units[i];
  }

  function fmtRateUnit(n, unit) {
    if (n == null || Number.isNaN(n)) return "—";
    const abs = Math.abs(n);
    if (unit === "B/s") {
      if (abs >= 1e9) return (n / 1e9).toFixed(2) + " GB/s";
      if (abs >= 1e6) return (n / 1e6).toFixed(2) + " MB/s";
      if (abs >= 1e3) return (n / 1e3).toFixed(1) + " KB/s";
      return n.toFixed(0) + " B/s";
    }
    if (abs >= 1e6) return (n / 1e6).toFixed(2) + "M " + unit;
    if (abs >= 1e3) return (n / 1e3).toFixed(1) + "K " + unit;
    return n.toFixed(1) + " " + unit;
  }

  function sumFrontendCounters(logical) {
    const fe = logical || {};
    let readOps = 0, writeOps = 0, readBytes = 0, writeBytes = 0;
    for (const k of ["s3", "fs", "vbd"]) {
      const c = fe[k] || {};
      readOps += Number(c.read_ops) || 0;
      writeOps += Number(c.write_ops) || 0;
      readBytes += Number(c.read_bytes) || 0;
      writeBytes += Number(c.write_bytes) || 0;
    }
    return { readOps, writeOps, readBytes, writeBytes };
  }

  function pushSample(series, point) {
    series.push(point);
    while (series.length > CHART_WINDOW) series.shift();
  }

  function sampleIoCharts(opsPayload) {
    const logical =
      (opsPayload && opsPayload.io_frontends && opsPayload.io_frontends.logical) || {};
    const cur = sumFrontendCounters(logical);
    const t = Date.now();
    if (chartState.last) {
      const dt = Math.max(0.001, (t - chartState.last.t) / 1000);
      const dReadOps = Math.max(0, cur.readOps - chartState.last.readOps);
      const dWriteOps = Math.max(0, cur.writeOps - chartState.last.writeOps);
      const dReadBytes = Math.max(0, cur.readBytes - chartState.last.readBytes);
      const dWriteBytes = Math.max(0, cur.writeBytes - chartState.last.writeBytes);
      pushSample(chartState.iops, { t, read: dReadOps / dt, write: dWriteOps / dt });
      pushSample(chartState.bytes, { t, read: dReadBytes / dt, write: dWriteBytes / dt });
    }
    chartState.last = { t, ...cur };
    if (activeTab === "overview") drawIoCharts();
  }

  function drawLineChart(canvas, series, colors) {
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth || 640;
    const cssH = canvas.clientHeight || 180;
    if (canvas.width !== Math.floor(cssW * dpr) || canvas.height !== Math.floor(cssH * dpr)) {
      canvas.width = Math.floor(cssW * dpr);
      canvas.height = Math.floor(cssH * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);

    const pad = { l: 44, r: 8, t: 8, b: 18 };
    const w = cssW - pad.l - pad.r;
    const h = cssH - pad.t - pad.b;

    ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue("--surface-2").trim() || "#f3eee7";
    ctx.fillRect(pad.l, pad.t, w, h);

    let maxY = 0;
    for (const p of series) {
      maxY = Math.max(maxY, p.read || 0, p.write || 0);
    }
    if (maxY <= 0) maxY = 1;

    const ink = getComputedStyle(document.documentElement).getPropertyValue("--muted").trim() || "#6b635a";
    ctx.strokeStyle = "rgba(28,25,22,0.08)";
    ctx.lineWidth = 1;
    ctx.font = "11px Sora, sans-serif";
    ctx.fillStyle = ink;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let i = 0; i <= 4; ++i) {
      const y = pad.t + (h * i) / 4;
      ctx.beginPath();
      ctx.moveTo(pad.l, y);
      ctx.lineTo(pad.l + w, y);
      ctx.stroke();
      const val = maxY * (1 - i / 4);
      ctx.fillText(val >= 1000 ? (val / 1000).toFixed(1) + "k" : val.toFixed(val >= 10 ? 0 : 1), pad.l - 6, y);
    }

    function pathFor(key, color) {
      if (!series.length) return;
      ctx.beginPath();
      series.forEach((p, i) => {
        const x = pad.l + (series.length === 1 ? w / 2 : (w * i) / (series.length - 1));
        const y = pad.t + h - (Math.min(p[key] || 0, maxY) / maxY) * h;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      });
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.lineJoin = "round";
      ctx.stroke();
    }

    pathFor("read", colors.read);
    pathFor("write", colors.write);

    if (!series.length) {
      ctx.fillStyle = ink;
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.fillText("Collecting samples…", pad.l + w / 2, pad.t + h / 2);
    }
  }

  function drawIoCharts() {
    const read = getComputedStyle(document.documentElement).getPropertyValue("--ok").trim() || "#2f6b4f";
    const write = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#b87333";
    const colors = { read, write };
    drawLineChart(document.getElementById("chart-iops"), chartState.iops, colors);
    drawLineChart(document.getElementById("chart-bytes"), chartState.bytes, colors);
    const iLast = chartState.iops[chartState.iops.length - 1];
    const bLast = chartState.bytes[chartState.bytes.length - 1];
    const iEl = document.getElementById("chart-iops-now");
    const bEl = document.getElementById("chart-bytes-now");
    if (iEl) {
      iEl.textContent = iLast
        ? `now  read ${fmtRateUnit(iLast.read, "ops/s")} · write ${fmtRateUnit(iLast.write, "ops/s")}`
        : "now  —";
    }
    if (bEl) {
      bEl.textContent = bLast
        ? `now  read ${fmtRateUnit(bLast.read, "B/s")} · write ${fmtRateUnit(bLast.write, "B/s")}`
        : "now  —";
    }
  }

  async function pollIoCharts() {
    if (activeTab !== "overview") return;
    try {
      const { res, json } = await api("/admin/api/ops");
      if (res.status === 401) {
        showLogin("Session expired — sign in again.");
        return;
      }
      if (res.ok) sampleIoCharts(json);
    } catch (_) {
      /* keep last series */
    }
  }

  function startChartSampler() {
    if (chartTimer) return;
    chartTimer = setInterval(() => pollIoCharts().catch(() => {}), 1000);
    pollIoCharts().catch(() => {});
  }

  function stopChartSampler() {
    if (chartTimer) {
      clearInterval(chartTimer);
      chartTimer = null;
    }
  }

  function emptyRow(cols, msg) {
    return `<tr class="empty-row"><td colspan="${cols}" class="empty">${msg}</td></tr>`;
  }

  function esc(s) {
    return String(s ?? "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function badge(text, kind) {
    const k = kind ? ` ${esc(kind)}` : "";
    return `<span class="badge${k}">${esc(text)}</span>`;
  }

  function stateBadge(state) {
    if (!state || state === "—") return "—";
    const s = String(state).toLowerCase();
    let kind = "";
    if (s === "up" || s === "online" || s === "ok") kind = "up";
    else if (s === "drain" || s === "suspect" || s === "warn") kind = "drain";
    else if (s === "off" || s === "offline" || s === "error") kind = "off";
    return badge(s, kind);
  }

  function membersFromStatus(status) {
    const m = status && status.membership;
    if (Array.isArray(m)) return m;
    if (m && Array.isArray(m.members)) return m.members;
    return [];
  }

  function mapTargetsFromStatus(status) {
    const cm = status && status.cluster_map;
    return cm && Array.isArray(cm.targets) ? cm.targets : [];
  }

  function replicaNeed(status, config) {
    if (typeof status?.replica_count === "number" && status.replica_count > 0) {
      return status.replica_count;
    }
    if (typeof config?.replica_count === "number" && config.replica_count > 0) {
      return config.replica_count;
    }
    const cm = status && status.cluster_map && status.cluster_map.replica_count;
    return typeof cm === "number" && cm > 0 ? cm : 3;
  }

  function upTargetCount(targets) {
    return targets.filter((t) => String(t.state || "up").toLowerCase() === "up").length;
  }

  function targetsByNode(targets) {
    const out = new Map();
    for (const t of targets) {
      const id = t.node_id || "";
      if (!out.has(id)) out.set(id, []);
      out.get(id).push(t);
    }
    return out;
  }

  function diskKind(nodeTargets) {
    if (!nodeTargets || !nodeTargets.length) return { label: "no disk", kind: "off" };
    const states = nodeTargets.map((t) => String(t.state || "up").toLowerCase());
    if (states.includes("up")) return { label: "disk", kind: "up" };
    if (states.includes("drain")) return { label: "drain", kind: "drain" };
    return { label: "off", kind: "off" };
  }

  function renderPlacement(status, config) {
    const members = membersFromStatus(status);
    const targets = mapTargetsFromStatus(status);
    const byNode = targetsByNode(targets);
    const need = replicaNeed(status, config);
    const up = upTargetCount(targets);
    const banner = document.getElementById("placement-banner");
    const chips = document.getElementById("node-targets");
    if (banner) {
      if (up < need) {
        banner.classList.remove("hidden");
        banner.className = "callout warn";
        banner.textContent =
          `${up} of ${need} storage targets — PUTs return 503 (no_targets). ` +
          `A node with no disk usually means .aios was not scanned or aios/ is not writable.`;
      } else {
        banner.className = "callout warn hidden";
        banner.textContent = "";
      }
    }
    if (chips) {
      const ids = members.map((m) => m.node_id).filter(Boolean);
      for (const t of targets) {
        if (t.node_id && !ids.includes(t.node_id)) ids.push(t.node_id);
      }
      ids.sort();
      chips.innerHTML =
        ids
          .map((id) => {
            const d = diskKind(byNode.get(id));
            const mem = members.find((m) => m.node_id === id);
            const memState = String((mem && mem.state) || "").toLowerCase();
            const cls = memState === "offline" ? "off" : d.kind;
            return `<span class="node-chip ${cls}"><span class="dot"></span>${esc(id)} · ${d.label}</span>`;
          })
          .join("") || `<span class="muted">No members yet</span>`;
    }
  }

  let lastMapEpoch = null;

  function renderCards(status, config) {
    const ops = status.ops || {};
    lastMapEpoch = status.map_epoch;
    const epochEl = document.getElementById("map-epoch-value");
    if (epochEl) epochEl.textContent = fmt(lastMapEpoch);

    const need = replicaNeed(status, config);
    const up = upTargetCount(mapTargetsFromStatus(status));
    const storageClass = up < need ? " warn" : " ok";

    const cards = [
      ["Node", status.node_id || "—"],
      ["Members online", status.members_alive],
      ["HTTP requests", ops.http_requests],
      ["Puts", ops.put],
      ["Gets", ops.get],
      ["Deletes", ops.del],
      ["Errors", ops.errors],
    ];
    const cardsHtml =
      `<div class="card"><span class="label">Map epoch</span>` +
      `<button type="button" class="btn card-action" id="show-map-epoch">Show</button></div>` +
      `<div class="card${storageClass}"><span class="label">Storage targets</span>` +
      `<div class="value">${up} / ${need}</div></div>` +
      cards
        .map(
          ([label, value]) =>
            `<div class="card"><span class="label">${label}</span><div class="value">${esc(fmt(value))}</div></div>`
        )
        .join("");
    document.getElementById("overview-cards").innerHTML = cardsHtml;
    nodeLabel.textContent = status.node_id || "";
  }

  function renderOps(opsPayload) {
    const ops = (opsPayload && opsPayload.ops) || {};
    const rows = Object.keys(ops)
      .sort()
      .map((k) => `<tr><td>${esc(k)}</td><td>${esc(fmt(ops[k]))}</td></tr>`)
      .join("");
    document.getElementById("ops-table").innerHTML =
      `<table><thead><tr><th>Counter</th><th>Value</th></tr></thead><tbody>${rows}</tbody></table>`;
    const comp = (opsPayload && opsPayload.compression) || {};
    const ratio = typeof comp.ratio === "number" ? comp.ratio : 0;
    const ratioTxt =
      comp.stored_bytes > 0 ? `${ratio.toFixed(2)}×` : "—";
    document.getElementById("compression-stats").innerHTML =
      `<table><thead><tr><th>Metric</th><th>Value</th></tr></thead><tbody>
        <tr><td>Overall ratio (logical÷stored)</td><td>${ratioTxt}</td></tr>
        <tr><td>Compressed puts</td><td>${fmt(comp.puts)}</td></tr>
        <tr><td>Skipped</td><td>${fmt(comp.skipped)}</td></tr>
        <tr><td>Logical bytes</td><td>${fmt(comp.logical_bytes)}</td></tr>
        <tr><td>Stored bytes</td><td>${fmt(comp.stored_bytes)}</td></tr>
      </tbody></table>`;
    const cardsEl = document.getElementById("overview-cards");
    if (cardsEl && comp.stored_bytes > 0) {
      const extra = `<div class="card"><span class="label">Compression ratio</span><div class="value">${ratioTxt}</div></div>`;
      if (!cardsEl.innerHTML.includes("Compression ratio")) {
        cardsEl.insertAdjacentHTML("beforeend", extra);
      } else {
        cardsEl.querySelectorAll(".card").forEach((c) => {
          if (c.querySelector(".label")?.textContent === "Compression ratio") {
            c.querySelector(".value").textContent = ratioTxt;
          }
        });
      }
    }
    const fe = (opsPayload && opsPayload.io_frontends && opsPayload.io_frontends.logical) || {};
    const order = ["s3", "fs", "vbd"];
    const keys = order.concat(Object.keys(fe).filter((k) => !order.includes(k)));
    const frows = keys
      .map((k) => {
        const c = fe[k] || {};
        return `<tr><td>${esc(k)}</td><td>${fmt(c.read_ops)}</td><td>${fmt(c.write_ops)}</td><td>${fmt(
          c.read_bytes
        )}</td><td>${fmt(c.write_bytes)}</td><td>${esc(c.source || "—")}</td></tr>`;
      })
      .join("");
    const vbd = (opsPayload && opsPayload.io_frontends && opsPayload.io_frontends.vbd_devices) || [];
    const vrows = vbd
      .map(
        (d) =>
          `<tr><td>aiosvd${esc(d.dev_id)}</td><td>${esc(d.pool)}/${esc(d.name)}</td><td>${fmt(
            d.ops_read
          )}</td><td>${fmt(d.ops_write)}</td><td>${fmt(d.bytes_read)}</td><td>${fmt(
            d.bytes_written
          )}</td></tr>`
      )
      .join("");
    document.getElementById("io-frontends").innerHTML =
      `<table><thead><tr><th>Frontend</th><th>Read ops</th><th>Write ops</th><th>Read bytes</th><th>Write bytes</th><th>Source</th></tr></thead><tbody>${
        frows || emptyRow(6, "No frontend IO yet")
      }</tbody></table>` +
      (vbd.length
        ? `<table><thead><tr><th>Device</th><th>Volume</th><th>Read ops</th><th>Write ops</th><th>Read bytes</th><th>Write bytes</th></tr></thead><tbody>${vrows}</tbody></table>`
        : `<p class="empty">No aiosvd devices on this node (module not loaded or none mapped).</p>`);
    sampleIoCharts(opsPayload);
  }

  function renderCluster(cluster, status) {
    const st = (cluster && cluster.status) || status || {};
    const members = membersFromStatus(st);
    const byNode = targetsByNode(mapTargetsFromStatus(st));
    const peers = (cluster && cluster.admin_peers) || [];
    const peerById = Object.fromEntries(peers.map((p) => [p.node_id, p]));
    const rows = members
      .map((m) => {
        const p = peerById[m.node_id] || {};
        const d = diskKind(byNode.get(m.node_id));
        const self = p.self || m.node_id === st.node_id;
        return `<tr>
          <td>${esc(m.node_id || "")}${self ? badge("self", "self") : ""}</td>
          <td>${stateBadge(m.state)}</td>
          <td>${badge(d.label, d.kind)}</td>
          <td>${esc(m.addr || p.addr || "")}</td>
          <td>${esc(m.http_addr || p.http_addr || "")}</td>
        </tr>`;
      })
      .join("");
    document.getElementById("cluster-table").innerHTML =
      `<table><thead><tr><th>Node</th><th>Member</th><th>Disk</th><th>Gossip</th><th>HTTP</th></tr></thead><tbody>${
        rows || emptyRow(5, "No members")
      }</tbody></table>`;
  }

  function renderS3(payload) {
    const creds = (payload && payload.credentials) || [];
    const rows = creds
      .map((c) => {
        const buckets = Array.isArray(c.buckets) ? c.buckets.join(", ") : "";
        const id = c.access_key_id || "";
        return `<tr>
          <td>${esc(id)}</td><td>${esc(fmt(c.uid))}</td><td>${esc(fmt(c.gid))}</td><td>${esc(buckets)}</td>
          <td><button type="button" class="btn ghost s3-del" data-id="${esc(id)}">Delete</button></td>
        </tr>`;
      })
      .join("");
    document.getElementById("s3-table").innerHTML =
      `<table><thead><tr><th>Access key</th><th>UID</th><th>GID</th><th>Buckets</th><th></th></tr></thead><tbody>${
        rows || emptyRow(5, "No credentials (or S3 disabled on this node)")
      }</tbody></table>`;
  }

  async function refreshS3() {
    const { res, json } = await api("/admin/api/s3/credentials");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderS3(json);
    else if (res.status === 404) renderS3({ credentials: [] });
  }

  async function refresh() {
    const [st, ops, cl, cfg] = await Promise.all([
      api("/admin/api/status"),
      api("/admin/api/ops"),
      api("/admin/api/cluster"),
      api("/admin/api/config"),
    ]);
    if (st.res.status === 401 || ops.res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (!st.res.ok) throw new Error((st.json && st.json.error) || "status failed");
    renderPlacement(st.json || {}, (cfg.res.ok && cfg.json) || {});
    renderCards(st.json || {}, (cfg.res.ok && cfg.json) || {});
    if (ops.res.ok) renderOps(ops.json);
    if (cl.res.ok) renderCluster(cl.json, st.json);
    if (cfg.res.ok) {
      document.getElementById("config-json").textContent = JSON.stringify(cfg.json, null, 2);
      const mp = document.getElementById("metrics-public");
      if (mp && cfg.json && typeof cfg.json.admin_metrics_public === "boolean") {
        mp.checked = cfg.json.admin_metrics_public;
      }
    }
    if (activeTab === "s3") await refreshS3();
    if (activeTab === "quota") await refreshQuota();
    if (activeTab === "qos") await refreshQos();
    if (activeTab === "posix-layout") await refreshPosixLayout();
    if (activeTab === "lifecycle") {
      const ae = document.activeElement;
      const editing =
        ae &&
        document.getElementById("lifecycle-table")?.contains(ae) &&
        (ae.matches("input, select, button") || ae.closest("tr"));
      if (!editing) await refreshLifecycle();
    }
    if (activeTab === "bench") await refreshBench();
    if (activeTab === "space") await refreshSpace();
    if (activeTab === "actions") await refreshArchiveBackup();
  }

  function parseBytes(s) {
    if (!s || !String(s).trim()) return null;
    const m = String(s).trim().match(/^([0-9.]+)\s*([KMGT]?)$/i);
    if (!m) return null;
    const mul = { "": 1, K: 1024, M: 1024 ** 2, G: 1024 ** 3, T: 1024 ** 4 };
    return Math.floor(Number(m[1]) * (mul[m[2].toUpperCase()] || 1));
  }

  function renderQuota(q) {
    const urows = ((q && q.volume_uids) || [])
      .map(
        (r) =>
          `<tr><td>${esc(r.uid)}</td><td>${fmt(r.used_bytes)}</td><td>${
            r.limit_bytes == null ? "—" : fmt(r.limit_bytes)
          }</td></tr>`
      )
      .join("");
    document.getElementById("quota-vol-table").innerHTML =
      `<table><thead><tr><th>UID</th><th>Used</th><th>Limit</th></tr></thead><tbody>${
        urows || emptyRow(3, "No uid quotas")
      }</tbody></table>` +
      (() => {
        const grows = ((q && q.volume_gids) || [])
          .map(
            (r) =>
              `<tr><td>${esc(r.gid)}</td><td>${fmt(r.used_bytes)}</td><td>${
                r.limit_bytes == null ? "—" : fmt(r.limit_bytes)
              }</td></tr>`
          )
          .join("");
        return `<table><thead><tr><th>GID</th><th>Used</th><th>Limit</th></tr></thead><tbody>${
          grows || emptyRow(3, "No gid quotas")
        }</tbody></table>`;
      })();
    const prows = ((q && q.projects) || [])
      .map(
        (p) =>
          `<tr><td>${esc(p.id)}</td><td>${esc(p.name || "")}</td><td>${esc(p.root_ino)}</td><td>${fmt(
            p.used_bytes
          )}</td><td>${p.limit_bytes == null ? "—" : fmt(p.limit_bytes)}</td>
          <td><button type="button" class="btn ghost quota-del" data-id="${esc(p.id)}">Delete</button></td></tr>`
      )
      .join("");
    document.getElementById("quota-proj-table").innerHTML =
      `<table><thead><tr><th>ID</th><th>Name</th><th>Root ino</th><th>Used</th><th>Limit</th><th></th></tr></thead><tbody>${
        prows || emptyRow(6, "No projects")
      }</tbody></table>`;
  }

  async function refreshQuota() {
    const { res, json } = await api("/admin/api/quota");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderQuota(json);
    else renderQuota({ volume_uids: [], volume_gids: [], projects: [] });
  }

  async function probeSession() {
    const { res } = await api("/admin/api/status");
    if (res.ok) {
      showApp();
      await refresh();
      return;
    }
    showLogin();
  }

  loginForm.addEventListener("submit", async (e) => {
    e.preventDefault();
    loginError.hidden = true;
    const cluster_key = document.getElementById("cluster-key").value;
    const { res, json } = await api("/admin/login", {
      method: "POST",
      body: JSON.stringify({ cluster_key }),
    });
    if (!res.ok) {
      showLogin((json && json.error) || "Login failed");
      return;
    }
    document.getElementById("cluster-key").value = "";
    showApp();
    await refresh();
  });

  document.getElementById("logout-btn").addEventListener("click", async () => {
    await api("/admin/logout", { method: "POST", body: "{}" });
    showLogin();
  });

  function fmtRate(n) {
    if (n == null || Number.isNaN(n)) return "—";
    if (n >= 1e9) return (n / 1e9).toFixed(2) + "G";
    if (n >= 1e6) return (n / 1e6).toFixed(2) + "M";
    if (n >= 1e3) return (n / 1e3).toFixed(1) + "K";
    return Number(n).toFixed(1);
  }

  function renderQos(q) {
    const mon = (q && q.monitoring && q.monitoring.node) || {};
    document.getElementById("qos-mon").innerHTML = `<table><thead><tr>
      <th>Put IOPS</th><th>Get IOPS</th><th>Put B/s</th><th>Get B/s</th></tr></thead><tbody>
      <tr><td>${fmtRate(mon.put_iops)}</td><td>${fmtRate(mon.get_iops)}</td>
      <td>${fmtRate(mon.put_bps)}</td><td>${fmtRate(mon.get_bps)}</td></tr></tbody></table>`;
    const urows = ((q && q.volume_uids) || [])
      .map(
        (r) =>
          `<tr><td>${esc(r.uid)}</td><td>${r.limit_iops == null ? "—" : esc(r.limit_iops)}</td><td>${
            r.limit_bps == null ? "—" : fmt(r.limit_bps)
          }</td></tr>`
      )
      .join("");
    const grows = ((q && q.volume_gids) || [])
      .map(
        (r) =>
          `<tr><td>${esc(r.gid)}</td><td>${r.limit_iops == null ? "—" : esc(r.limit_iops)}</td><td>${
            r.limit_bps == null ? "—" : fmt(r.limit_bps)
          }</td></tr>`
      )
      .join("");
    document.getElementById("qos-vol-table").innerHTML =
      `<table><thead><tr><th>UID</th><th>IOPS</th><th>BPS</th></tr></thead><tbody>${
        urows || emptyRow(3, "No uid QoS")
      }</tbody></table>` +
      `<table><thead><tr><th>GID</th><th>IOPS</th><th>BPS</th></tr></thead><tbody>${
        grows || emptyRow(3, "No gid QoS")
      }</tbody></table>`;
    const prows = ((q && q.projects) || [])
      .map((p) => {
        const u = (p.uids || [])
          .map((x) => `uid ${esc(x.uid)}: ${esc(x.limit_iops ?? "—")} iops / ${x.limit_bps == null ? "—" : fmt(x.limit_bps)}`)
          .join("; ");
        return `<tr><td>${esc(p.id)}</td><td>${p.limit_iops == null ? "—" : esc(p.limit_iops)}</td><td>${
          p.limit_bps == null ? "—" : fmt(p.limit_bps)
        }</td><td>${u || "—"}</td></tr>`;
      })
      .join("");
    document.getElementById("qos-proj-table").innerHTML =
      `<table><thead><tr><th>ID</th><th>IOPS</th><th>BPS</th><th>Per-uid</th></tr></thead><tbody>${
        prows || emptyRow(4, "No project QoS")
      }</tbody></table>`;
  }

  async function refreshQos() {
    const { res, json } = await api("/admin/api/qos");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderQos(json);
    else renderQos({ volume_uids: [], volume_gids: [], projects: [], monitoring: {} });
  }

  let spaceDoc = null;
  let spaceRange = "1y";

  function usedFillClass(pct) {
    if (pct >= 90) return "danger";
    if (pct >= 75) return "warn";
    return "";
  }

  function drawSpaceRing(used, total) {
    const canvas = document.getElementById("space-ring");
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const css = 200;
    if (canvas.width !== Math.floor(css * dpr) || canvas.height !== Math.floor(css * dpr)) {
      canvas.width = Math.floor(css * dpr);
      canvas.height = Math.floor(css * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, css, css);
    const cx = css / 2;
    const cy = css / 2;
    const r = 74;
    const lw = 18;
    const pct = total > 0 ? Math.min(1, used / total) : 0;
    ctx.lineWidth = lw;
    ctx.lineCap = "round";
    ctx.strokeStyle = "rgba(28,25,22,0.08)";
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.stroke();
    const accent = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#b87333";
    const warn = getComputedStyle(document.documentElement).getPropertyValue("--warn").trim() || "#9a6b1f";
    const danger = getComputedStyle(document.documentElement).getPropertyValue("--danger").trim() || "#a33b2b";
    ctx.strokeStyle = pct >= 0.9 ? danger : pct >= 0.75 ? warn : accent;
    ctx.beginPath();
    ctx.arc(cx, cy, r, -Math.PI / 2, -Math.PI / 2 + pct * Math.PI * 2);
    ctx.stroke();
    const ink = getComputedStyle(document.documentElement).getPropertyValue("--ink").trim() || "#1c1916";
    const muted = getComputedStyle(document.documentElement).getPropertyValue("--muted").trim() || "#6b635a";
    ctx.fillStyle = ink;
    ctx.font = "650 28px Sora, sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(total > 0 ? Math.round(pct * 100) + "%" : "—", cx, cy - 8);
    ctx.fillStyle = muted;
    ctx.font = "500 12px Sora, sans-serif";
    ctx.fillText("used", cx, cy + 16);
    const label = document.getElementById("space-ring-label");
    if (label) {
      label.textContent = total > 0 ? `${fmtBytes(used)} of ${fmtBytes(total)}` : "No disks in the map";
    }
  }

  function renderSpaceBars(el, rows, nameKey) {
    if (!el) return;
    if (!rows || !rows.length) {
      el.innerHTML = `<p class="empty">No usable disks</p>`;
      return;
    }
    el.innerHTML = rows
      .map((r) => {
        const tot = Number(r.total_bytes) || 0;
        const used = Number(r.used_bytes) || 0;
        const pct = tot > 0 ? (100 * used) / tot : 0;
        const disks = Number(r.disks) || 0;
        const name = esc(r[nameKey] || "—") + (disks ? ` <span class="muted">· ${disks} disk${disks === 1 ? "" : "s"}</span>` : "");
        return `<div class="space-bar">
          <div class="bar-head"><strong>${name}</strong><span class="muted">${fmtBytes(used)} / ${fmtBytes(tot)}</span></div>
          <div class="bar-track"><div class="bar-fill ${usedFillClass(pct)}" style="width:${Math.min(100, pct).toFixed(1)}%"></div></div>
        </div>`;
      })
      .join("");
  }

  function spaceHistorySeries(doc, range) {
    const h = (doc && doc.history) || {};
    const now = Date.now();
    let raw = h.daily || [];
    let windowMs = 366 * 24 * 3600 * 1000;
    if (range === "24h") {
      raw = h.recent || [];
      windowMs = 24 * 3600 * 1000;
    } else if (range === "7d") {
      raw = h.hourly || [];
      windowMs = 7 * 24 * 3600 * 1000;
    } else if (range === "30d") {
      raw = h.hourly || [];
      windowMs = 30 * 24 * 3600 * 1000;
    }
    const cutoff = now - windowMs;
    return raw
      .filter((p) => Number(p.t) >= cutoff)
      .map((p) => ({
        t: Number(p.t),
        used: Number(p.used_bytes) || 0,
        total: Number(p.total_bytes) || 0,
        avail: Number(p.avail_bytes) || 0,
      }));
  }

  function drawSpaceHistory(series) {
    const canvas = document.getElementById("chart-space");
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const cssW = canvas.clientWidth || 640;
    const cssH = canvas.clientHeight || 200;
    if (canvas.width !== Math.floor(cssW * dpr) || canvas.height !== Math.floor(cssH * dpr)) {
      canvas.width = Math.floor(cssW * dpr);
      canvas.height = Math.floor(cssH * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, cssW, cssH);
    const pad = { l: 52, r: 10, t: 10, b: 28 };
    const w = cssW - pad.l - pad.r;
    const h = cssH - pad.t - pad.b;
    const muted = getComputedStyle(document.documentElement).getPropertyValue("--muted").trim() || "#6b635a";
    const accent = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || "#b87333";
    ctx.fillStyle = getComputedStyle(document.documentElement).getPropertyValue("--surface-2").trim() || "#f3eee7";
    ctx.fillRect(pad.l, pad.t, w, h);
    let maxY = 0;
    for (const p of series) maxY = Math.max(maxY, p.total || 0, p.used || 0);
    if (maxY <= 0) maxY = 1;
    ctx.strokeStyle = "rgba(28,25,22,0.08)";
    ctx.lineWidth = 1;
    ctx.font = "11px Sora, sans-serif";
    ctx.fillStyle = muted;
    ctx.textAlign = "right";
    ctx.textBaseline = "middle";
    for (let i = 0; i <= 4; ++i) {
      const y = pad.t + (h * i) / 4;
      ctx.beginPath();
      ctx.moveTo(pad.l, y);
      ctx.lineTo(pad.l + w, y);
      ctx.stroke();
      ctx.fillText(fmtBytes(maxY * (1 - i / 4)), pad.l - 6, y);
    }
    if (!series.length) {
      ctx.textAlign = "center";
      ctx.fillText("No samples yet — the first point is recorded at startup", pad.l + w / 2, pad.t + h / 2);
      return;
    }
    const xAt = (i) => pad.l + (series.length === 1 ? w / 2 : (w * i) / (series.length - 1));
    const yAt = (v) => pad.t + h - (Math.min(v, maxY) / maxY) * h;
    ctx.beginPath();
    series.forEach((p, i) => {
      const x = xAt(i);
      const y = yAt(p.used);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.lineTo(xAt(series.length - 1), pad.t + h);
    ctx.lineTo(xAt(0), pad.t + h);
    ctx.closePath();
    ctx.fillStyle = "rgba(184, 115, 51, 0.22)";
    ctx.fill();
    ctx.beginPath();
    series.forEach((p, i) => {
      const x = xAt(i);
      if (i === 0) ctx.moveTo(x, yAt(p.used));
      else ctx.lineTo(x, yAt(p.used));
    });
    ctx.strokeStyle = accent;
    ctx.lineWidth = 2;
    ctx.stroke();
    ctx.beginPath();
    series.forEach((p, i) => {
      const x = xAt(i);
      if (i === 0) ctx.moveTo(x, yAt(p.total));
      else ctx.lineTo(x, yAt(p.total));
    });
    ctx.strokeStyle = "rgba(28,25,22,0.28)";
    ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 3]);
    ctx.stroke();
    ctx.setLineDash([]);
    const fmtWhen = (t) => {
      const d = new Date(t);
      if (Number.isNaN(d.getTime())) return "";
      const span = series[series.length - 1].t - series[0].t;
      if (span <= 36 * 3600 * 1000) {
        return d.toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
      }
      return d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" });
    };
    ctx.fillStyle = muted;
    ctx.font = "11px Sora, sans-serif";
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    ctx.fillText(fmtWhen(series[0].t), pad.l, pad.t + h + 6);
    ctx.textAlign = "right";
    ctx.fillText(fmtWhen(series[series.length - 1].t), pad.l + w, pad.t + h + 6);
  }

  function renderSpace(doc) {
    spaceDoc = doc;
    const tot = (doc && doc.totals) || {};
    const used = Number(tot.used_bytes) || 0;
    const total = Number(tot.total_bytes) || 0;
    const avail = Number(tot.avail_bytes) || 0;
    const pct = total > 0 ? (100 * used) / total : 0;
    drawSpaceRing(used, total);
    document.getElementById("space-summary").innerHTML = [
      ["Total", fmtBytes(total)],
      ["Used", fmtBytes(used)],
      ["Available", fmtBytes(avail)],
      ["Full", total > 0 ? pct.toFixed(1) + "%" : "—"],
    ]
      .map(
        ([label, value]) =>
          `<div class="card${label === "Full" ? (pct >= 75 ? " warn" : " ok") : ""}"><span class="label">${label}</span><div class="value">${value}</div></div>`
      )
      .join("");
    renderSpaceBars(document.getElementById("space-hosts"), doc.by_host || [], "node_id");
    renderSpaceBars(document.getElementById("space-classes"), doc.by_class || [], "storage_class");
    const disks = (doc && doc.disks) || [];
    const rows = disks
      .map((d) => {
        const t = Number(d.total_bytes) || 0;
        const u = Number(d.used_bytes) || 0;
        const p = t > 0 ? ((100 * u) / t).toFixed(1) + "%" : "—";
        return `<tr>
          <td>${esc(d.node_id || "—")}${d.self ? badge("self", "self") : ""}</td>
          <td>${esc(d.mount || d.aios_path || "—")}</td>
          <td>${esc(d.storage_class || "—")}</td>
          <td>${stateBadge(d.state)}${d.usable ? "" : badge("unusable", "off")}</td>
          <td class="num">${fmtBytes(t)}</td>
          <td class="num">${fmtBytes(u)}</td>
          <td class="num">${fmtBytes(d.avail_bytes)}</td>
          <td class="num">${p}</td>
        </tr>`;
      })
      .join("");
    document.getElementById("space-table").innerHTML =
      `<table><thead><tr><th>Host</th><th>Disk</th><th>Class</th><th>State</th><th class="num">Total</th><th class="num">Used</th><th class="num">Avail</th><th class="num">Full</th></tr></thead><tbody>${
        rows || emptyRow(8, "No disks in the cluster map")
      }</tbody></table>`;
    document.querySelectorAll("#space-range .btn").forEach((b) => {
      b.classList.toggle("active", b.dataset.range === spaceRange);
    });
    const series = spaceHistorySeries(doc, spaceRange);
    drawSpaceHistory(series);
    const nowEl = document.getElementById("chart-space-now");
    const last = series[series.length - 1];
    if (nowEl) {
      nowEl.textContent = last
        ? `${spaceRange}  used ${fmtBytes(last.used)} · total ${fmtBytes(last.total)} · ${series.length} samples`
        : `${spaceRange}  no samples yet`;
    }
  }

  async function refreshSpace() {
    const { res, json } = await api("/admin/api/space");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderSpace(json);
    else renderSpace({ totals: {}, disks: [], by_host: [], by_class: [], history: {} });
  }

  document.getElementById("space-range").addEventListener("click", (e) => {
    const btn = e.target.closest("[data-range]");
    if (!btn) return;
    spaceRange = btn.dataset.range;
    if (spaceDoc) renderSpace(spaceDoc);
  });

  document.getElementById("tabs").addEventListener("click", (e) => {
    const btn = e.target.closest(".tab");
    if (!btn) return;
    setTab(btn.dataset.tab);
    if (btn.dataset.tab === "s3") refreshS3().catch(() => {});
    if (btn.dataset.tab === "quota") refreshQuota().catch(() => {});
    if (btn.dataset.tab === "qos") refreshQos().catch(() => {});
    if (btn.dataset.tab === "posix-layout") refreshPosixLayout().catch(() => {});
    if (btn.dataset.tab === "lifecycle") refreshLifecycle().catch(() => {});
    if (btn.dataset.tab === "bench") refreshBench().catch(() => {});
    if (btn.dataset.tab === "space") refreshSpace().catch(() => {});
    if (btn.dataset.tab === "actions") refreshArchiveBackup().catch(() => {});
  });

  function renderPosixLayout(doc) {
    const rules = (doc && doc.posix_layout_rules) || [];
    const rows = rules
      .map((r) => {
        const meta = r.meta || {};
        const data = r.data || {};
        const fmt = (s) =>
          esc([s.layout || "—", s.storage_class || "—"].join(" / "));
        return `<tr><td>${esc(r.path || "/")}</td><td>${esc(r.volume || "*")}</td><td>${fmt(
          meta
        )}</td><td>${fmt(data)}</td></tr>`;
      })
      .join("");
    document.getElementById("posix-layout-table").innerHTML =
      `<table><thead><tr><th>Path</th><th>Volume</th><th>Meta</th><th>Data</th></tr></thead><tbody>${
        rows || emptyRow(4, "No rules (cluster defaults)")
      }</tbody></table>`;
    document.getElementById("posix-layout-json").value = JSON.stringify(
      { posix_layout_rules: rules },
      null,
      2
    );
  }

  async function refreshPosixLayout() {
    const { res, json } = await api("/admin/api/posix-layout");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderPosixLayout(json);
    else renderPosixLayout({ posix_layout_rules: [] });
  }

  function lifecycleStateSelect(id, value, disabled) {
    const v = String(value || "up").toLowerCase();
    const known = value != null && value !== "";
    const opts = ["up", "drain", "off"]
      .map((s) => `<option value="${s}"${known && s === v ? " selected" : ""}>${s}</option>`)
      .join("");
    const extra = known ? "" : `<option value="" selected disabled>—</option>`;
    return `<select class="cell-select" data-id="${esc(id)}" data-field="state"${
      disabled ? " disabled" : ""
    }>${extra}${opts}</select>`;
  }

  function renderLifecycle(doc) {
    const nodes = [...((doc && doc.nodes) || [])];
    const targets = [...((doc && doc.targets) || [])];
    const byHost = new Map();
    for (const n of nodes) {
      if (!n || !n.node_id) continue;
      byHost.set(n.node_id, { node: n, disks: [] });
    }
    for (const t of targets) {
      const id = t.node_id || "unknown";
      if (!byHost.has(id)) byHost.set(id, { node: { node_id: id, self: !!t.self }, disks: [] });
      byHost.get(id).disks.push(t);
    }
    const hosts = [...byHost.values()].sort((a, b) => {
      const as = a.node && a.node.self ? 0 : 1;
      const bs = b.node && b.node.self ? 0 : 1;
      if (as !== bs) return as - bs;
      return String(a.node.node_id || "").localeCompare(String(b.node.node_id || ""));
    });
    for (const h of hosts) {
      h.disks.sort((a, b) =>
        String(a.mount || a.aios_path || "").localeCompare(String(b.mount || b.aios_path || ""))
      );
    }

    const rows = [];
    for (const h of hosts) {
      const n = h.node || {};
      const nid = n.node_id || "—";
      const self = !!n.self;
      const settingsOk = self || !!n.settings_ok;
      const nodeState = settingsOk
        ? n.node_state || (self ? doc.node_state : null)
        : null;
      const autoOn = settingsOk
        ? n.weight_autotune != null
          ? !!n.weight_autotune
          : !!(self && doc.weight_autotune)
        : false;
      const autoTh = settingsOk
        ? n.weight_autotune_threshold_pct != null
          ? n.weight_autotune_threshold_pct
          : self
            ? doc.weight_autotune_threshold_pct ?? 20
            : 20
        : "";
      const autoMd = settingsOk
        ? n.weight_autotune_min_delta != null
          ? n.weight_autotune_min_delta
          : self
            ? doc.weight_autotune_min_delta ?? 1
            : 1
        : "";
      const gossip = n.member_state ? stateBadge(n.member_state) : "";
      const diskLabel = `${h.disks.length} disk${h.disks.length === 1 ? "" : "s"}`;
      const hostMeta = [n.http_addr || n.addr, diskLabel].filter(Boolean).join(" · ");
      rows.push(`<tr class="host-row" data-node="${esc(nid)}">
        <td><div class="host-cell">
          <div class="host-name">${esc(nid)}${self ? badge("self", "self") : ""}${gossip}${
            settingsOk ? "" : badge("unreachable", "warn")
          }</div>
          <div class="host-meta">${esc(hostMeta)}</div>
        </div></td>
        <td>${esc(n.rack || "—")}</td>
        <td class="muted">host</td>
        <td>${lifecycleStateSelect(`host-${nid}`, nodeState, !settingsOk)}</td>
        <td class="muted">—</td>
        <td>${
          settingsOk
            ? `<div class="autotune">
            <label><input type="checkbox" data-id="${esc("auto-" + nid)}" data-field="autotune"${autoOn ? " checked" : ""} /> auto</label>
            <label>% <input type="number" min="0" max="100" value="${esc(autoTh)}" data-id="${esc("th-" + nid)}" data-field="threshold" /></label>
            <label>Δ <input type="number" min="1" value="${esc(autoMd)}" data-id="${esc("md-" + nid)}" data-field="mindelta" /></label>
          </div>`
            : `<span class="muted">settings unavailable</span>`
        }</td>
        <td class="apply"><button type="button" class="btn primary apply-btn" data-apply="host" data-node="${esc(nid)}"${
          settingsOk ? "" : " disabled"
        }>Apply</button></td>
      </tr>`);
      if (!h.disks.length) {
        rows.push(
          `<tr class="disk-row host-last"><td class="disk-name muted" colspan="7">No disks in the cluster map for this host</td></tr>`
        );
        continue;
      }
      h.disks.forEach((t, i) => {
        const key = t.aios_path || t.mount || "";
        const diskId = `${nid}:${key}`;
        const last = i === h.disks.length - 1;
        rows.push(`<tr class="disk-row${last ? " host-last" : ""}" data-node="${esc(nid)}" data-mount="${esc(t.mount || "")}" data-path="${esc(t.aios_path || "")}">
          <td class="disk-name"><span class="disk-mark" aria-hidden="true"></span><span class="disk-label">${esc(t.mount || t.aios_path || "—")}${
            t.aios_path && t.aios_path !== t.mount
              ? `<span class="disk-path">${esc(t.aios_path)}</span>`
              : ""
          }</span></td>
          <td>${esc(t.rack || n.rack || "—")}</td>
          <td>${esc(t.storage_class || "—")}</td>
          <td>${lifecycleStateSelect(diskId, t.state)}</td>
          <td><input class="cell-input weight" type="number" min="1" value="${t.weight != null ? esc(t.weight) : ""}" placeholder="—" data-id="${esc(diskId)}" data-field="weight" /></td>
          <td class="muted">—</td>
          <td class="apply"><button type="button" class="btn primary apply-btn" data-apply="disk" data-node="${esc(nid)}" data-mount="${esc(t.mount || "")}" data-path="${esc(t.aios_path || "")}">Apply</button></td>
        </tr>`);
      });
    }

    document.getElementById("lifecycle-table").innerHTML =
      `<table class="lifecycle-table"><thead><tr>
        <th>Host / disk</th><th>Rack</th><th>Class</th><th>State</th><th>Weight</th><th>Autotune</th><th class="apply">Apply</th>
      </tr></thead><tbody>${
        rows.length ? rows.join("") : emptyRow(7, "No hosts or disks in the cluster map")
      }</tbody></table>`;
  }

  async function refreshLifecycle() {
    const { res, json } = await api("/admin/api/lifecycle");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    const doc = res.ok && json ? json : { targets: [], nodes: [] };
    const remotes = (doc.nodes || []).filter((n) => n && n.node_id && !n.self);
    if (remotes.length) {
      const extras = await Promise.all(
        remotes.map(async (n) => {
          const r = await api("/admin/api/lifecycle?node_id=" + encodeURIComponent(n.node_id));
          return [n.node_id, r.res.ok ? r.json : null];
        })
      );
      const byId = new Map(extras);
      doc.nodes = (doc.nodes || []).map((n) => {
        if (n.self) return { ...n, settings_ok: true };
        const extra = byId.get(n.node_id);
        if (!extra) return { ...n, settings_ok: false };
        return {
          ...n,
          settings_ok: true,
          node_state: extra.node_state || n.node_state,
          weight_autotune: extra.weight_autotune,
          weight_autotune_threshold_pct: extra.weight_autotune_threshold_pct,
          weight_autotune_min_delta: extra.weight_autotune_min_delta,
        };
      });
    } else {
      doc.nodes = (doc.nodes || []).map((n) => ({ ...n, settings_ok: !!n.self || n.settings_ok }));
    }
    renderLifecycle(doc);
  }

  function lifecycleMsg(ok, json, fallback) {
    const errEl = document.getElementById("lifecycle-error");
    const out = document.getElementById("lifecycle-result");
    errEl.hidden = true;
    out.classList.add("hidden");
    if (!ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || fallback;
      return;
    }
    out.classList.remove("hidden");
    out.textContent = typeof json === "string" ? json : JSON.stringify(json, null, 2);
  }

  document.getElementById("lifecycle-table").addEventListener("click", async (e) => {
    const btn = e.target.closest("[data-apply]");
    if (!btn) return;
    const errEl = document.getElementById("lifecycle-error");
    const out = document.getElementById("lifecycle-result");
    errEl.hidden = true;
    out.classList.add("hidden");
    const node = btn.dataset.node || "";
    const row = btn.closest("tr");
    if (!row) return;
    btn.disabled = true;
    try {
      if (btn.dataset.apply === "host") {
        const state = row.querySelector('[data-field="state"]')?.value;
        const enabled = !!row.querySelector('[data-field="autotune"]')?.checked;
        const threshold_pct = parseInt(row.querySelector('[data-field="threshold"]')?.value, 10);
        const min_delta = parseInt(row.querySelector('[data-field="mindelta"]')?.value, 10);
        const nodeBody = { state, node_id: node };
        const autoBody = { enabled, threshold_pct, min_delta, node_id: node };
        const nres = await api("/admin/api/lifecycle/node", {
          method: "PUT",
          body: JSON.stringify(nodeBody),
        });
        if (!nres.res.ok) {
          lifecycleMsg(false, nres.json, "Set node state failed");
          return;
        }
        const ares = await api("/admin/api/lifecycle/autotune", {
          method: "PUT",
          body: JSON.stringify(autoBody),
        });
        if (!ares.res.ok) {
          lifecycleMsg(false, ares.json, "Save autotune failed");
          return;
        }
        lifecycleMsg(true, `Applied host settings on ${node}`);
        await refreshLifecycle();
        return;
      }
      if (btn.dataset.apply === "disk") {
        const state = row.querySelector('[data-field="state"]')?.value;
        const wraw = row.querySelector('[data-field="weight"]')?.value?.trim();
        const body = { node_id: node };
        if (btn.dataset.path) body.aios_path = btn.dataset.path;
        else if (btn.dataset.mount) body.mount = btn.dataset.mount;
        if (state) body.state = state;
        if (wraw) body.weight = parseInt(wraw, 10);
        if (!body.state && body.weight == null) {
          lifecycleMsg(false, { error: "Set state and/or weight" });
          return;
        }
        const { res, json } = await api("/admin/api/lifecycle/target", {
          method: "PUT",
          body: JSON.stringify(body),
        });
        const label = btn.dataset.mount || btn.dataset.path || "disk";
        lifecycleMsg(res.ok, res.ok ? `Applied ${label} on ${node}` : json, "Set target failed");
        if (res.ok) await refreshLifecycle();
      }
    } finally {
      btn.disabled = false;
    }
  });

  let benchPoll = null;

  function benchPresets() {
    return {
      quick: { mode: "object", threads: 4, ops: 20, warmup: 2, sizes: "1k,4k", mix: ["create", "read"], layout: "", stl_sync: "async", types: ["string"] },
      standard: { mode: "object", threads: 4, ops: 50, warmup: 5, sizes: "1k,4k,64k", mix: ["create", "update", "read"], layout: "", stl_sync: "async", types: ["string", "map"] },
      wide: { mode: "object", threads: 8, ops: 80, warmup: 5, sizes: "1k,64k,1M", mix: ["create", "update", "read"], layout: "", stl_sync: "async", types: ["string", "map"] },
      stl: { mode: "stl", threads: 4, ops: 40, warmup: 4, sizes: "16,64,256", mix: ["create", "update", "read"], layout: "", stl_sync: "async", types: ["string", "map"] },
    };
  }

  function applyBenchPreset(name) {
    const p = benchPresets()[name];
    if (!p) return;
    document.getElementById("bench-mode").value = p.mode;
    document.getElementById("bench-threads").value = p.threads;
    document.getElementById("bench-ops").value = p.ops;
    document.getElementById("bench-warmup").value = p.warmup;
    document.getElementById("bench-sizes").value = p.sizes;
    document.getElementById("bench-layout").value = p.layout;
    document.getElementById("bench-stl-sync").value = p.stl_sync;
    document.getElementById("bench-op-create").checked = p.mix.includes("create");
    document.getElementById("bench-op-update").checked = p.mix.includes("update");
    document.getElementById("bench-op-read").checked = p.mix.includes("read");
    document.querySelectorAll(".bench-stl-type").forEach((el) => {
      el.checked = p.types.includes(el.value);
    });
    syncBenchModeFields();
  }

  function syncBenchModeFields() {
    const stl = document.getElementById("bench-mode").value === "stl";
    const ec = document.getElementById("bench-layout").value === "ec";
    document.querySelectorAll(".bench-stl").forEach((el) => el.classList.toggle("hidden", !stl));
    document.querySelectorAll(".bench-ec").forEach((el) => el.classList.toggle("hidden", stl || !ec));
  }

  function benchFormBody() {
    const mix = [];
    if (document.getElementById("bench-op-create").checked) mix.push("create");
    if (document.getElementById("bench-op-update").checked) mix.push("update");
    if (document.getElementById("bench-op-read").checked) mix.push("read");
    const types = [...document.querySelectorAll(".bench-stl-type:checked")].map((el) => el.value);
    const body = {
      mode: document.getElementById("bench-mode").value,
      threads: Number(document.getElementById("bench-threads").value) || 4,
      ops: Number(document.getElementById("bench-ops").value) || 50,
      warmup: Number(document.getElementById("bench-warmup").value) || 0,
      sizes: document.getElementById("bench-sizes").value.trim(),
      prefix: document.getElementById("bench-prefix").value.trim() || "bench",
      ops_mix: mix,
      cleanup: document.getElementById("bench-cleanup").checked,
      layout: document.getElementById("bench-layout").value,
      stl_sync: document.getElementById("bench-stl-sync").value,
      stl_types: types,
    };
    if (body.layout === "ec") {
      body.ec_k = Number(document.getElementById("bench-ec-k").value) || 0;
      body.ec_m = Number(document.getElementById("bench-ec-m").value) || 0;
      body.ec_codec = document.getElementById("bench-ec-codec").value;
    }
    return body;
  }

  function renderBench(doc) {
    const state = (doc && doc.state) || "idle";
    const badge = document.getElementById("bench-state");
    badge.textContent = state;
    badge.className = "badge " + (state === "done" ? "up" : state === "error" || state === "cancelled" ? "off" : state === "running" ? "warn" : "");
    document.getElementById("bench-run").disabled = state === "running";
    document.getElementById("bench-stop").disabled = state !== "running";
    const errEl = document.getElementById("bench-error");
    if (doc && doc.error) {
      errEl.hidden = false;
      errEl.textContent = doc.error;
    } else {
      errEl.hidden = true;
    }
    const results = (doc && doc.results) || [];
    const stl = results.some((r) => r.stl_type);
    const head = stl
      ? `<th>Type</th><th>Sync</th><th>Size</th><th>Op</th><th class="num">OK</th><th class="num">Err</th><th class="num">IOPS</th><th class="num">p50</th><th class="num">p95</th><th class="num">p99</th>`
      : `<th>Size</th><th>Op</th><th class="num">OK</th><th class="num">Err</th><th class="num">IOPS</th><th class="num">MiB/s</th><th class="num">p50</th><th class="num">p95</th><th class="num">p99</th>`;
    const rows = results
      .map((r) => {
        const n = (x, d) => (x == null ? "—" : Number(x).toFixed(d));
        if (stl) {
          return `<tr><td>${esc(r.stl_type || "—")}</td><td>${esc(r.stl_sync || "—")}</td><td>${esc(r.size_label || r.size)}</td><td>${esc(r.op)}</td><td class="num">${esc(r.ok ?? 0)}</td><td class="num">${esc(r.err ?? 0)}</td><td class="num">${n(r.iops, 1)}</td><td class="num">${n(r.p50_ms, 2)}</td><td class="num">${n(r.p95_ms, 2)}</td><td class="num">${n(r.p99_ms, 2)}</td></tr>`;
        }
        return `<tr><td>${esc(r.size_label || r.size)}</td><td>${esc(r.op)}</td><td class="num">${esc(r.ok ?? 0)}</td><td class="num">${esc(r.err ?? 0)}</td><td class="num">${n(r.iops, 1)}</td><td class="num">${n(r.mib_s, 2)}</td><td class="num">${n(r.p50_ms, 2)}</td><td class="num">${n(r.p95_ms, 2)}</td><td class="num">${n(r.p99_ms, 2)}</td></tr>`;
      })
      .join("");
    document.getElementById("bench-table").innerHTML =
      `<table><thead><tr>${head}</tr></thead><tbody>${
        rows || emptyRow(stl ? 10 : 9, state === "running" ? "Running…" : "No results yet")
      }</tbody></table>`;
    const running = state === "running";
    if (running && !benchPoll) {
      benchPoll = setInterval(() => refreshBench().catch(() => {}), 1000);
    }
    if (!running && benchPoll) {
      clearInterval(benchPoll);
      benchPoll = null;
    }
  }

  async function refreshBench() {
    const { res, json } = await api("/admin/api/bench");
    if (res.status === 401) {
      showLogin("Session expired — sign in again.");
      return;
    }
    if (res.ok) renderBench(json);
    else renderBench({ state: "idle", results: [], error: (json && json.error) || "Bench API unavailable" });
  }

  document.getElementById("bench-form").addEventListener("input", syncBenchModeFields);
  document.getElementById("bench-mode").addEventListener("change", syncBenchModeFields);
  document.getElementById("bench-layout").addEventListener("change", syncBenchModeFields);
  document.querySelector(".bench-presets").addEventListener("click", (e) => {
    const btn = e.target.closest("[data-bench-preset]");
    if (!btn) return;
    applyBenchPreset(btn.dataset.benchPreset);
  });
  document.getElementById("bench-run").addEventListener("click", async () => {
    const errEl = document.getElementById("bench-error");
    errEl.hidden = true;
    const body = benchFormBody();
    if (!body.ops_mix.length) {
      errEl.hidden = false;
      errEl.textContent = "Select at least one operation";
      return;
    }
    if (body.mode === "stl" && !body.stl_types.length) {
      errEl.hidden = false;
      errEl.textContent = "Select at least one STL type";
      return;
    }
    const { res, json } = await api("/admin/api/bench/run", {
      method: "POST",
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Start failed";
      return;
    }
    await refreshBench();
  });
  document.getElementById("bench-stop").addEventListener("click", async () => {
    await api("/admin/api/bench/stop", { method: "POST", body: "{}" });
    await refreshBench();
  });
  syncBenchModeFields();

  document.getElementById("posix-layout-reload").addEventListener("click", () => {
    refreshPosixLayout().catch(() => {});
  });

  document.getElementById("posix-layout-save").addEventListener("click", async () => {
    const errEl = document.getElementById("posix-layout-error");
    const out = document.getElementById("posix-layout-result");
    errEl.hidden = true;
    out.classList.add("hidden");
    let body;
    try {
      body = JSON.parse(document.getElementById("posix-layout-json").value);
    } catch (e) {
      errEl.hidden = false;
      errEl.textContent = "Invalid JSON";
      return;
    }
    const { res, json } = await api("/admin/api/posix-layout", {
      method: "PUT",
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Save failed";
      return;
    }
    renderPosixLayout(json);
    out.classList.remove("hidden");
    out.textContent = "Saved.";
  });

  async function qosPutLimits(body) {
    const errEl = document.getElementById("qos-error");
    errEl.hidden = true;
    const { res, json } = await api("/admin/api/qos/limits", {
      method: "PUT",
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Set failed";
      return;
    }
    await refreshQos();
  }

  document.getElementById("qos-set-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const uid = document.getElementById("qos-uid").value;
    const gid = document.getElementById("qos-gid").value;
    const body = {};
    if (uid !== "") body.uid = Number(uid);
    else if (gid !== "") body.gid = Number(gid);
    else {
      const errEl = document.getElementById("qos-error");
      errEl.hidden = false;
      errEl.textContent = "UID or GID required";
      return;
    }
    const iops = document.getElementById("qos-iops").value;
    const bps = document.getElementById("qos-bps").value;
    if (iops !== "") body.iops = Number(iops);
    if (bps.trim() !== "") body.bps = parseBytes(bps);
    await qosPutLimits(body);
  });

  document.getElementById("qos-clear").addEventListener("click", async () => {
    const uid = document.getElementById("qos-uid").value;
    const gid = document.getElementById("qos-gid").value;
    const body = { clear: true };
    if (uid !== "") body.uid = Number(uid);
    else if (gid !== "") body.gid = Number(gid);
    else {
      const errEl = document.getElementById("qos-error");
      errEl.hidden = false;
      errEl.textContent = "UID or GID required to clear";
      return;
    }
    await qosPutLimits(body);
  });

  document.getElementById("qos-proj-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const body = { project_id: Number(document.getElementById("qos-proj-id").value) };
    const uid = document.getElementById("qos-proj-uid").value;
    if (uid !== "") body.uid = Number(uid);
    const iops = document.getElementById("qos-proj-iops").value;
    const bps = document.getElementById("qos-proj-bps").value;
    if (iops !== "") body.iops = Number(iops);
    if (bps.trim() !== "") body.bps = parseBytes(bps);
    await qosPutLimits(body);
  });

  document.getElementById("quota-set-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const errEl = document.getElementById("quota-error");
    errEl.hidden = true;
    const uid = document.getElementById("quota-uid").value;
    const gid = document.getElementById("quota-gid").value;
    const raw = document.getElementById("quota-bytes").value;
    const body = {};
    if (uid !== "") body.uid = Number(uid);
    else if (gid !== "") body.gid = Number(gid);
    else {
      errEl.hidden = false;
      errEl.textContent = "UID or GID required";
      return;
    }
    body.bytes = raw.trim() === "" ? null : parseBytes(raw);
    const { res, json } = await api("/admin/api/quota/limits", {
      method: "PUT",
      body: JSON.stringify(body),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Set failed";
      return;
    }
    await refreshQuota();
  });

  document.getElementById("quota-proj-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const errEl = document.getElementById("quota-error");
    errEl.hidden = true;
    const name = document.getElementById("quota-proj-name").value.trim();
    const root_ino = Number(document.getElementById("quota-proj-ino").value);
    const bytes = parseBytes(document.getElementById("quota-proj-bytes").value);
    const { res, json } = await api("/admin/api/quota/projects", {
      method: "POST",
      body: JSON.stringify({ name, root_ino, bytes }),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Create project failed";
      return;
    }
    await refreshQuota();
  });

  document.getElementById("quota-proj-table").addEventListener("click", async (e) => {
    const btn = e.target.closest(".quota-del");
    if (!btn) return;
    const id = btn.dataset.id;
    if (!confirm(`Delete project ${id}?`)) return;
    const { res, json } = await api(`/admin/api/quota/projects/${id}`, {
      method: "DELETE",
      body: "{}",
    });
    if (!res.ok) {
      const errEl = document.getElementById("quota-error");
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Delete failed";
      return;
    }
    await refreshQuota();
  });

  document.getElementById("quota-reconcile").addEventListener("click", async () => {
    const errEl = document.getElementById("quota-error");
    const out = document.getElementById("quota-result");
    errEl.hidden = true;
    const { res, json } = await api("/admin/api/quota/reconcile", {
      method: "POST",
      body: "{}",
    });
    out.classList.remove("hidden");
    out.textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Reconcile failed";
      return;
    }
    await refreshQuota();
  });

  document.getElementById("s3-table").addEventListener("click", async (e) => {
    const btn = e.target.closest(".s3-del");
    if (!btn) return;
    const id = btn.dataset.id;
    if (!id || !confirm(`Delete S3 credential ${id}?`)) return;
    const errEl = document.getElementById("s3-error");
    errEl.hidden = true;
    const { res, json } = await api(`/admin/api/s3/credentials/${encodeURIComponent(id)}`, {
      method: "DELETE",
      body: "{}",
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Delete failed";
      return;
    }
    await refreshS3();
  });

  document.getElementById("s3-create-form").addEventListener("submit", async (e) => {
    e.preventDefault();
    const errEl = document.getElementById("s3-error");
    const secretEl = document.getElementById("s3-secret");
    errEl.hidden = true;
    secretEl.classList.add("hidden");
    const access_key_id = document.getElementById("s3-id").value.trim();
    const uid = Number(document.getElementById("s3-uid").value);
    const gid = Number(document.getElementById("s3-gid").value);
    const buckets = document.getElementById("s3-buckets").value
      .split(",")
      .map((s) => s.trim())
      .filter(Boolean);
    const { res, json } = await api("/admin/api/s3/credentials", {
      method: "POST",
      body: JSON.stringify({ access_key_id, uid, gid, buckets }),
    });
    if (!res.ok) {
      errEl.hidden = false;
      errEl.textContent = (json && json.error) || "Create failed";
      return;
    }
    secretEl.classList.remove("hidden");
    secretEl.textContent =
      `Created ${json.access_key_id}\nsecret: ${json.secret}\n(store the secret now; it is not shown again)`;
    document.getElementById("s3-id").value = "";
    await refreshS3();
  });

  document.getElementById("save-settings").addEventListener("click", async () => {
    actionError.hidden = true;
    const admin_metrics_public = document.getElementById("metrics-public").checked;
    const { res, json } = await api("/admin/api/settings", {
      method: "POST",
      body: JSON.stringify({ admin_metrics_public }),
    });
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Settings update failed";
      return;
    }
    await refresh();
  });

  document.getElementById("run-transitions").addEventListener("click", async () => {
    actionError.hidden = true;
    const { res, json } = await api("/admin/api/transitions/run", {
      method: "POST",
      body: "{}",
    });
    document.getElementById("transitions-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Transitions failed";
    }
  });

  document.getElementById("run-repair").addEventListener("click", async () => {
    actionError.hidden = true;
    const { res, json } = await api("/admin/api/repair/run", {
      method: "POST",
      body: "{}",
    });
    document.getElementById("repair-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Repair failed";
    }
  });

  function renderBackupPolicies(policies) {
    const wrap = document.getElementById("backup-policy-table");
    if (!wrap) return;
    const list = policies || [];
    if (!list.length) {
      wrap.innerHTML = "<p class=\"empty\">No live policies.</p>";
      return;
    }
    let html =
      "<table><thead><tr><th>ID</th><th>Volume</th><th>Path</th><th>At</th><th>Keep</th><th></th></tr></thead><tbody>";
    for (const p of list) {
      const at = (p.schedule && p.schedule.at) || p.at || "";
      const kd = (p.retain && p.retain.keep_days) ?? p.keep_days ?? "";
      const km = (p.retain && p.retain.keep_monthly) ?? p.keep_monthly ?? "";
      const en = p.enabled === false ? " (off)" : "";
      const id = p.id || "";
      html += `<tr>
        <td><code>${esc(id)}</code>${en}</td>
        <td>${esc(p.volume || "")}</td>
        <td>${esc(p.path || "/")}</td>
        <td>${esc(at)} UTC</td>
        <td>${esc(kd)}d / ${esc(km)}mo</td>
        <td><button type="button" class="btn bp-del" data-id="${esc(id)}">Delete</button></td>
      </tr>`;
    }
    html += "</tbody></table>";
    wrap.innerHTML = html;
    wrap.querySelectorAll(".bp-del").forEach((btn) => {
      btn.addEventListener("click", async () => {
        const id = btn.getAttribute("data-id");
        actionError.hidden = true;
        const { res, json } = await api("/admin/api/backup/policies/" + encodeURIComponent(id), {
          method: "DELETE",
        });
        document.getElementById("backup-policy-result").textContent = JSON.stringify(json, null, 2);
        if (!res.ok) {
          actionError.hidden = false;
          actionError.textContent = (json && json.error) || "Delete failed";
        } else {
          await refreshArchiveBackup();
        }
      });
    });
  }

  async function refreshArchiveBackup() {
    const [arch, bak] = await Promise.all([
      api("/admin/api/archive"),
      api("/admin/api/backup"),
    ]);
    const archEl = document.getElementById("archive-rules");
    const bakEl = document.getElementById("backup-rules");
    if (arch.res.ok && arch.json) {
      archEl.textContent = JSON.stringify(
        {
          archive_interval_ms: arch.json.archive_interval_ms,
          archive_batch_oids: arch.json.archive_batch_oids,
          archive_rules: arch.json.archive_rules || [],
        },
        null,
        2
      );
    } else {
      archEl.textContent = (arch.json && arch.json.error) || "Failed to load archive rules";
    }
    if (bak.res.ok && bak.json) {
      bakEl.textContent = JSON.stringify(
        {
          backup_interval_ms: bak.json.backup_interval_ms,
          backup_batch_oids: bak.json.backup_batch_oids,
          backup_rules: bak.json.backup_rules || [],
        },
        null,
        2
      );
      renderBackupPolicies(bak.json.policies || []);
    } else {
      bakEl.textContent = (bak.json && bak.json.error) || "Failed to load backup rules";
      renderBackupPolicies([]);
    }
  }

  function syncBackupSnapFields() {
    const kind = document.getElementById("backup-snap-kind").value;
    document.querySelectorAll(".backup-posix-fields").forEach((el) => {
      el.classList.toggle("hidden", kind !== "posix");
    });
    document.querySelectorAll(".backup-vbd-fields").forEach((el) => {
      el.classList.toggle("hidden", kind !== "vbd");
    });
  }

  document.getElementById("backup-snap-kind").addEventListener("change", syncBackupSnapFields);
  syncBackupSnapFields();

  document.getElementById("run-archive").addEventListener("click", async () => {
    actionError.hidden = true;
    const { res, json } = await api("/admin/api/archive/run", { method: "POST", body: "{}" });
    document.getElementById("archive-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Archive pack failed";
    }
  });

  document.getElementById("run-archive-drain").addEventListener("click", async () => {
    actionError.hidden = true;
    const { res, json } = await api("/admin/api/archive/drain", { method: "POST", body: "{}" });
    document.getElementById("archive-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Archive drain failed";
    }
  });

  document.getElementById("run-archive-recall").addEventListener("click", async () => {
    actionError.hidden = true;
    const oid = document.getElementById("archive-recall-oid").value.trim();
    if (!oid) {
      actionError.hidden = false;
      actionError.textContent = "Recall oid required";
      return;
    }
    const { res, json } = await api("/admin/api/archive/recall", {
      method: "POST",
      body: JSON.stringify({ oid }),
    });
    document.getElementById("archive-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Archive recall failed";
    }
  });

  document.getElementById("run-backup").addEventListener("click", async () => {
    actionError.hidden = true;
    const { res, json } = await api("/admin/api/backup/run", { method: "POST", body: "{}" });
    document.getElementById("backup-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Backup run failed";
    } else {
      await refreshArchiveBackup();
    }
  });

  document.getElementById("backup-policy-form").addEventListener("submit", async (ev) => {
    ev.preventDefault();
    actionError.hidden = true;
    const body = {
      kind: "posix",
      volume: document.getElementById("bp-volume").value.trim(),
      path: document.getElementById("bp-path").value.trim() || "/",
      enabled: document.getElementById("bp-enabled").value === "true",
      schedule: { at: document.getElementById("bp-at").value.trim() || "00:00", tz: "UTC" },
      retain: {
        keep_days: Number(document.getElementById("bp-keep-days").value) || 0,
        keep_monthly: Number(document.getElementById("bp-keep-monthly").value) || 0,
      },
      staging_class: "archive",
      tape_sink: document.getElementById("bp-tape-sink").value.trim(),
      tape_uri_prefix: document.getElementById("bp-tape-uri").value.trim(),
      bag_compression: document.getElementById("bp-bag-compression").value,
      bag_encryption: document.getElementById("bp-bag-encryption").value,
    };
    const id = document.getElementById("bp-id").value.trim();
    if (id) body.id = id;
    if (!body.volume) {
      actionError.hidden = false;
      actionError.textContent = "Volume required";
      return;
    }
    const { res, json } = await api("/admin/api/backup/policies", {
      method: "POST",
      body: JSON.stringify(body),
    });
    document.getElementById("backup-policy-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Policy save failed";
    } else {
      document.getElementById("bp-id").value = "";
      await refreshArchiveBackup();
    }
  });

  document.getElementById("run-backup-snapshot").addEventListener("click", async () => {
    actionError.hidden = true;
    const kind = document.getElementById("backup-snap-kind").value;
    const body = { kind };
    if (kind === "posix") {
      body.volume = document.getElementById("backup-snap-volume").value.trim();
      body.path = document.getElementById("backup-snap-path").value.trim() || "/";
      if (!body.volume) {
        actionError.hidden = false;
        actionError.textContent = "Volume required";
        return;
      }
    } else {
      body.pool = document.getElementById("backup-snap-pool").value.trim();
      body.name = document.getElementById("backup-snap-name").value.trim();
      const dest = document.getElementById("backup-snap-dest").value.trim();
      if (dest) body.dest = dest;
      if (!body.pool || !body.name) {
        actionError.hidden = false;
        actionError.textContent = "Pool and name required";
        return;
      }
    }
    const { res, json } = await api("/admin/api/backup/snapshot", {
      method: "POST",
      body: JSON.stringify(body),
    });
    document.getElementById("backup-snapshot-result").textContent = JSON.stringify(json, null, 2);
    if (!res.ok) {
      actionError.hidden = false;
      actionError.textContent = (json && json.error) || "Snapshot failed";
    }
  });

  window.addEventListener("resize", () => {
    if (activeTab === "overview") drawIoCharts();
    if (activeTab === "space" && spaceDoc) renderSpace(spaceDoc);
  });

  document.getElementById("overview-cards").addEventListener("click", (e) => {
    const btn = e.target.closest("#show-map-epoch");
    if (!btn) return;
    const dialog = document.getElementById("map-epoch-dialog");
    const valueEl = document.getElementById("map-epoch-value");
    if (valueEl) valueEl.textContent = fmt(lastMapEpoch);
    if (dialog && typeof dialog.showModal === "function") dialog.showModal();
  });

  setTab("overview");
  probeSession().catch(() => showLogin());
})();
