(() => {
  const loginView = document.getElementById("login-view");
  const appView = document.getElementById("app-view");
  const loginForm = document.getElementById("login-form");
  const loginError = document.getElementById("login-error");
  const nodeLabel = document.getElementById("node-label");
  const actionError = document.getElementById("action-error");

  let refreshTimer = null;
  let activeTab = "overview";

  async function api(path, opts = {}) {
    const res = await fetch(path, {
      credentials: "same-origin",
      headers: { "Content-Type": "application/json", ...(opts.headers || {}) },
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
  }

  function setTab(name) {
    activeTab = name;
    document.querySelectorAll(".tab").forEach((t) => {
      t.classList.toggle("active", t.dataset.tab === name);
    });
    document.querySelectorAll(".panel").forEach((p) => {
      p.classList.toggle("hidden", p.id !== `tab-${name}`);
    });
  }

  function fmt(n) {
    if (n === undefined || n === null) return "—";
    if (typeof n === "number") return n.toLocaleString();
    return String(n);
  }

  function renderCards(status) {
    const ops = status.ops || {};
    const cards = [
      ["Node", status.node_id || "—"],
      ["Map epoch", status.map_epoch],
      ["Members alive", status.members_alive],
      ["HTTP requests", ops.http_requests],
      ["Puts", ops.put],
      ["Gets", ops.get],
      ["Deletes", ops.del],
      ["Errors", ops.errors],
    ];
    document.getElementById("overview-cards").innerHTML = cards
      .map(
        ([label, value]) =>
          `<div class="card"><span class="label">${label}</span><div class="value">${fmt(value)}</div></div>`
      )
      .join("");
    nodeLabel.textContent = status.node_id ? `· ${status.node_id}` : "";
  }

  function renderOps(opsPayload) {
    const ops = (opsPayload && opsPayload.ops) || {};
    const rows = Object.keys(ops)
      .sort()
      .map((k) => `<tr><td>${k}</td><td>${fmt(ops[k])}</td></tr>`)
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
        return `<tr><td>${k}</td><td>${fmt(c.read_ops)}</td><td>${fmt(c.write_ops)}</td><td>${fmt(
          c.read_bytes
        )}</td><td>${fmt(c.write_bytes)}</td><td>${c.source || "—"}</td></tr>`;
      })
      .join("");
    const vbd = (opsPayload && opsPayload.io_frontends && opsPayload.io_frontends.vbd_devices) || [];
    const vrows = vbd
      .map(
        (d) =>
          `<tr><td>aiosvd${d.dev_id}</td><td>${d.pool}/${d.name}</td><td>${fmt(
            d.ops_read
          )}</td><td>${fmt(d.ops_write)}</td><td>${fmt(d.bytes_read)}</td><td>${fmt(
            d.bytes_written
          )}</td></tr>`
      )
      .join("");
    document.getElementById("io-frontends").innerHTML =
      `<table><thead><tr><th>Frontend</th><th>Read ops</th><th>Write ops</th><th>Read bytes</th><th>Write bytes</th><th>Source</th></tr></thead><tbody>${
        frows || "<tr><td colspan=6>No frontend IO yet</td></tr>"
      }</tbody></table>` +
      (vbd.length
        ? `<table style="margin-top:1rem"><thead><tr><th>Device</th><th>Volume</th><th>Read ops</th><th>Write ops</th><th>Read bytes</th><th>Write bytes</th></tr></thead><tbody>${vrows}</tbody></table>`
        : `<p class="muted" style="margin-top:0.75rem">No aiosvd devices on this node (module not loaded or none mapped).</p>`);
  }

  function renderCluster(cluster) {
    const peers = (cluster && cluster.admin_peers) || [];
    const rows = peers
      .map(
        (p) =>
          `<tr><td>${p.node_id || ""}${p.self ? " (self)" : ""}</td><td>${p.addr || ""}</td><td>${p.http_addr || ""}</td></tr>`
      )
      .join("");
    document.getElementById("cluster-table").innerHTML =
      `<table><thead><tr><th>Node</th><th>Gossip</th><th>HTTP</th></tr></thead><tbody>${
        rows || "<tr><td colspan=3>No peers with http_addr</td></tr>"
      }</tbody></table>`;
  }

  function renderS3(payload) {
    const creds = (payload && payload.credentials) || [];
    const rows = creds
      .map((c) => {
        const buckets = Array.isArray(c.buckets) ? c.buckets.join(", ") : "";
        const id = c.access_key_id || "";
        return `<tr>
          <td>${id}</td><td>${fmt(c.uid)}</td><td>${fmt(c.gid)}</td><td>${buckets}</td>
          <td><button type="button" class="btn ghost s3-del" data-id="${id}">Delete</button></td>
        </tr>`;
      })
      .join("");
    document.getElementById("s3-table").innerHTML =
      `<table><thead><tr><th>Access key</th><th>UID</th><th>GID</th><th>Buckets</th><th></th></tr></thead><tbody>${
        rows || "<tr><td colspan=5>No credentials (or S3 disabled on this node)</td></tr>"
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
    renderCards(st.json || {});
    if (ops.res.ok) renderOps(ops.json);
    if (cl.res.ok) renderCluster(cl.json);
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
          `<tr><td>${r.uid}</td><td>${fmt(r.used_bytes)}</td><td>${
            r.limit_bytes == null ? "—" : fmt(r.limit_bytes)
          }</td></tr>`
      )
      .join("");
    document.getElementById("quota-vol-table").innerHTML =
      `<table><thead><tr><th>UID</th><th>Used</th><th>Limit</th></tr></thead><tbody>${
        urows || "<tr><td colspan=3>No uid quotas</td></tr>"
      }</tbody></table>` +
      (() => {
        const grows = ((q && q.volume_gids) || [])
          .map(
            (r) =>
              `<tr><td>${r.gid}</td><td>${fmt(r.used_bytes)}</td><td>${
                r.limit_bytes == null ? "—" : fmt(r.limit_bytes)
              }</td></tr>`
          )
          .join("");
        return `<table style="margin-top:1rem"><thead><tr><th>GID</th><th>Used</th><th>Limit</th></tr></thead><tbody>${
          grows || "<tr><td colspan=3>No gid quotas</td></tr>"
        }</tbody></table>`;
      })();
    const prows = ((q && q.projects) || [])
      .map(
        (p) =>
          `<tr><td>${p.id}</td><td>${p.name || ""}</td><td>${p.root_ino}</td><td>${fmt(
            p.used_bytes
          )}</td><td>${p.limit_bytes == null ? "—" : fmt(p.limit_bytes)}</td>
          <td><button type="button" class="btn ghost quota-del" data-id="${p.id}">Delete</button></td></tr>`
      )
      .join("");
    document.getElementById("quota-proj-table").innerHTML =
      `<table><thead><tr><th>ID</th><th>Name</th><th>Root ino</th><th>Used</th><th>Limit</th><th></th></tr></thead><tbody>${
        prows || "<tr><td colspan=6>No projects</td></tr>"
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
          `<tr><td>${r.uid}</td><td>${r.limit_iops == null ? "—" : r.limit_iops}</td><td>${
            r.limit_bps == null ? "—" : fmt(r.limit_bps)
          }</td></tr>`
      )
      .join("");
    const grows = ((q && q.volume_gids) || [])
      .map(
        (r) =>
          `<tr><td>${r.gid}</td><td>${r.limit_iops == null ? "—" : r.limit_iops}</td><td>${
            r.limit_bps == null ? "—" : fmt(r.limit_bps)
          }</td></tr>`
      )
      .join("");
    document.getElementById("qos-vol-table").innerHTML =
      `<table><thead><tr><th>UID</th><th>IOPS</th><th>BPS</th></tr></thead><tbody>${
        urows || "<tr><td colspan=3>No uid QoS</td></tr>"
      }</tbody></table>` +
      `<table style="margin-top:1rem"><thead><tr><th>GID</th><th>IOPS</th><th>BPS</th></tr></thead><tbody>${
        grows || "<tr><td colspan=3>No gid QoS</td></tr>"
      }</tbody></table>`;
    const prows = ((q && q.projects) || [])
      .map((p) => {
        const u = (p.uids || [])
          .map((x) => `uid ${x.uid}: ${x.limit_iops ?? "—"} iops / ${x.limit_bps == null ? "—" : fmt(x.limit_bps)}`)
          .join("; ");
        return `<tr><td>${p.id}</td><td>${p.limit_iops == null ? "—" : p.limit_iops}</td><td>${
          p.limit_bps == null ? "—" : fmt(p.limit_bps)
        }</td><td>${u || "—"}</td></tr>`;
      })
      .join("");
    document.getElementById("qos-proj-table").innerHTML =
      `<table><thead><tr><th>ID</th><th>IOPS</th><th>BPS</th><th>Per-uid</th></tr></thead><tbody>${
        prows || "<tr><td colspan=4>No project QoS</td></tr>"
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

  document.getElementById("tabs").addEventListener("click", (e) => {
    const btn = e.target.closest(".tab");
    if (!btn) return;
    setTab(btn.dataset.tab);
    if (btn.dataset.tab === "s3") refreshS3().catch(() => {});
    if (btn.dataset.tab === "quota") refreshQuota().catch(() => {});
    if (btn.dataset.tab === "qos") refreshQos().catch(() => {});
    if (btn.dataset.tab === "actions") refreshArchiveBackup().catch(() => {});
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
      wrap.innerHTML = "<p class=\"muted\">No live policies.</p>";
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
        <td><code>${id}</code>${en}</td>
        <td>${p.volume || ""}</td>
        <td>${p.path || "/"}</td>
        <td>${at} UTC</td>
        <td>${kd}d / ${km}mo</td>
        <td><button type="button" class="btn bp-del" data-id="${id}">Delete</button></td>
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

  setTab("overview");
  probeSession().catch(() => showLogin());
})();
