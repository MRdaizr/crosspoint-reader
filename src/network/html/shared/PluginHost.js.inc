static constexpr char PLUGIN_HOST_JS[] PROGMEM = R"CPJS(
// Trusted, same-origin BrowserJS host. No code is evaluated on the ESP32.
(function () {
  const host = window.CrossPoint = window.CrossPoint || {};
  Object.assign(host, {
    _pending: null, _session: '', _actions: Object.create(null), _busy: false, _completion: null,
    registerPlugin(fn) { this._pending = fn; },
    async _read(r) {
      if (!r.ok) {
        let error = ''; try { error = (await r.json()).error || ''; } catch (_) {}
        const e = new Error(error || ('HTTP ' + r.status)); e.status = r.status; throw e;
      }
      return r.json();
    },
    async _post(path, body) {
      return this._read(await fetch(path, { method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Plugin-Session': this._session },
        body: JSON.stringify(body) }));
    },
    pluginFile(plugin, file) {
      return '/plugin?name=' + encodeURIComponent(plugin) + '&file=' + encodeURIComponent(file) +
             '&session=' + encodeURIComponent(this._session);
    },
    async relay(plugin, method, url, headers, body) {
      const r = await fetch('/api/relay', { method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-Plugin-Session': this._session },
        body: JSON.stringify({ plugin, method, url, headers: headers || {}, body: body || '' }) });
      if (!r.ok) return this._read(r);
      return { status: Number(r.headers.get('X-Relay-Status')),
               headers: JSON.parse(r.headers.get('X-Relay-Headers') || '[]'), body: await r.text() };
    },
    crypto(plugin, op, fields) { return this._post('/api/crypto', { ...(fields || {}), plugin, op }); },
    async writeFile(plugin, path, dataB64) {
      const bytes = Uint8Array.from(atob(dataB64 || ''), c => c.charCodeAt(0));
      const form = new FormData(); form.append('file', new Blob([bytes]), 'data');
      return this._read(await fetch('/api/plugin-fs?plugin=' + encodeURIComponent(plugin) +
        '&path=' + encodeURIComponent(path) + '&session=' + encodeURIComponent(this._session),
        { method: 'POST', body: form }));
    },
    fetchToSd(plugin, url, dest, headers) {
      return this._post('/api/fetch', { plugin, url, dest, headers: headers || {} });
    },
    registerAction(plugin, action, fn) {
      (this._actions[plugin] ||= Object.create(null))[action] = fn;
      if (!this._timer) this._timer = setInterval(() => this._pollJobs(), 2500);
    },
    async _sendCompletion() {
      if (!this._completion) return true;
      try { await this._post('/api/plugin-jobs/complete', this._completion); }
      catch (e) { if (!(e.status >= 400 && e.status < 500)) return false; }
      this._completion = null; return true;
    },
    async _pollJobs() {
      if (this._busy) return; this._busy = true;
      try {
        if (!(await this._sendCompletion())) return;
        for (const plugin of Object.keys(this._actions)) {
          let job;
          try { job = await this._read(await fetch('/api/plugin-jobs/claim?plugin=' + encodeURIComponent(plugin),
            { headers: { 'X-Plugin-Session': this._session } })); } catch (_) { continue; }
          if (!job.id) continue;
          let result, ok = false;
          try {
            const action = this._actions[plugin][job.action];
            if (typeof action !== 'function') throw new Error('Unknown action');
            result = (await action(job.args || {})) ?? {}; ok = true;
          } catch (e) { result = { error: String(e.message || e).slice(0, 96) }; }
          try {
            if (new TextEncoder().encode(JSON.stringify(result)).length >= 192) {
              result = { error: 'Result exceeds 191 bytes' }; ok = false;
            }
          } catch (_) { result = { error: 'Result is not JSON' }; ok = false; }
          this._completion = { plugin, id: job.id, claim: job.claim, ok, result };
          if (!(await this._sendCompletion())) return;
        }
      } finally { this._busy = false; }
    }
  });
  window.loadPlugins = async function loadPlugins(mount) {
    let container = document.getElementById('plugin-container');
    if (!container) {
      container = document.createElement('section'); container.id = 'plugin-container';
      (document.querySelector('.container') || document.body).appendChild(container);
    }
    let list;
    try {
      const response = await fetch('/api/plugins', { cache: 'no-store' });
      host._session = response.headers.get('X-Plugin-Session') || '';
      list = await host._read(response);
    } catch (_) { return; }
    for (const plugin of list) {
      if (mount && (plugin.mount || 'settings') !== mount) continue;
      const card = document.createElement('div'); card.className = 'card';
      const title = document.createElement('h2'); title.textContent = plugin.title || plugin.name;
      const description = document.createElement('p'); description.textContent = plugin.description || '';
      const status = document.createElement('p');
      status.textContent = plugin.approved ? 'Enabled' : (plugin.enabled ? 'Changed: approval required' : 'Disabled');
      const control = document.createElement('button');
      control.textContent = plugin.approved ? 'Disable' : 'Trust and enable';
      control.disabled = !plugin.approved && (!plugin.available || !plugin.systemEnabled);
      control.onclick = async () => {
        if (!plugin.approved && !confirm('This plugin runs in this trusted, same-origin page. It can access page data, SD files and the network. Enable only code you trust. Code or manifest changes require approval again.')) return;
        try {
          await host._post('/api/plugins/permission', { plugin: plugin.name,
            enabled: !plugin.approved, digest: plugin.digest });
          // Reload removes already-running JS after disable. Disable blocks
          // new server jobs/drains immediately but cannot revoke JS already loaded.
          location.reload();
        } catch (e) { status.textContent = e.message; }
      };
      card.append(title, description, status, control); container.appendChild(card);
      if (plugin.enabled && !plugin.approved) {
        const disable = document.createElement('button'); disable.textContent = 'Disable';
        disable.onclick = async () => {
          try { await host._post('/api/plugins/permission', {plugin:plugin.name, enabled:false}); location.reload(); }
          catch (e) { status.textContent = e.message; }
        };
        card.appendChild(disable);
      }
      if (!plugin.approved || !plugin.script || !plugin.systemEnabled) continue;
      const content = document.createElement('div'); card.appendChild(content);
      host._pending = null;
      try {
        await new Promise((resolve, reject) => {
          const script = document.createElement('script');
          script.src = host.pluginFile(plugin.name, plugin.script);
          script.onload = resolve; script.onerror = reject; document.body.appendChild(script);
        });
        if (typeof host._pending === 'function') await host._pending(content, {
          name: plugin.name, dir: plugin.dir,
          relay: (m, u, h, b) => host.relay(plugin.name, m, u, h, b),
          crypto: (op, f) => host.crypto(plugin.name, op, f),
          writeFile: (p, b) => host.writeFile(plugin.name, p, b),
          fetchToSd: (u, d, h) => host.fetchToSd(plugin.name, u, d, h),
          registerAction: (a, fn) => host.registerAction(plugin.name, a, fn),
          pluginFile: f => host.pluginFile(plugin.name, f)
        });
      } catch (e) { content.textContent = 'Plugin failed: ' + String(e.message || e); }
    }
  };
})();
)CPJS";
