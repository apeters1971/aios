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

  document.getElementById("tabs").addEventListener("click", (e) => {
    const btn = e.target.closest(".tab");
    if (!btn) return;
    setTab(btn.dataset.tab);
    if (btn.dataset.tab === "s3") refreshS3().catch(() => {});
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

  setTab("overview");
  probeSession().catch(() => showLogin());
})();
