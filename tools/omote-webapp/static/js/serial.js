/**
 * OmoteSerial - WebSocket client for communicating with the OMOTE device
 * via the Python serial bridge.
 */
class OmoteSerial {
  constructor() {
    this.ws = null;
    this._pending = new Map();
    this._responseCbs = [];
    this._logCbs = [];
    this._statusCbs = [];
    this._idCounter = 0;
    this._reconnectTimer = null;
    this._intentionalClose = false;
  }

  connect() {
    this._intentionalClose = false;
    const proto = location.protocol === 'https:' ? 'wss' : 'ws';
    this.ws = new WebSocket(`${proto}://${location.host}/ws`);

    this.ws.onopen = () => {
      console.log('[serial] WebSocket connected');
      if (this._reconnectTimer) {
        clearTimeout(this._reconnectTimer);
        this._reconnectTimer = null;
      }
    };

    this.ws.onmessage = (event) => {
      let msg;
      try { msg = JSON.parse(event.data); } catch { return; }

      if (msg.type === 'response') {
        this._responseCbs.forEach(cb => cb(msg.data));
        // Resolve pending promises — firmware responses use "res" and "id"
        if (msg.data && typeof msg.data === 'object') {
          const res = msg.data.res || '';
          const resId = msg.data.id || '';
          // Try matching by id first, then by command name
          let pending = resId ? this._pending.get(resId) : null;
          if (!pending && res) pending = this._pending.get(res);
          if (pending) {
            clearTimeout(pending.timer);
            // Clean up both keys
            if (resId) this._pending.delete(resId);
            if (res) this._pending.delete(res);
            pending.resolve(msg.data);
          }
        }
      } else if (msg.type === 'log') {
        this._logCbs.forEach(cb => cb(msg.data));
      } else if (msg.type === 'status') {
        this._statusCbs.forEach(cb => cb(msg.data));
      }
    };

    this.ws.onclose = () => {
      console.log('[serial] WebSocket closed');
      if (!this._intentionalClose) {
        this._reconnectTimer = setTimeout(() => this.connect(), 2000);
      }
    };

    this.ws.onerror = (err) => {
      console.error('[serial] WebSocket error', err);
    };
  }

  send(cmd, params = {}) {
    return new Promise((resolve, reject) => {
      if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
        reject(new Error('WebSocket not connected'));
        return;
      }

      const id = `${cmd}_${++this._idCounter}`;
      const payload = { cmd, id, ...params };
      const content = JSON.stringify(payload);

      const timer = setTimeout(() => {
        this._pending.delete(id);
        this._pending.delete(cmd);
        reject(new Error(`Timeout waiting for response to ${cmd}`));
      }, 5000);

      // Store under both id and cmd for flexible matching
      const entry = { resolve, reject, timer };
      this._pending.set(id, entry);
      this._pending.set(cmd, entry);

      this.ws.send(JSON.stringify({ type: 'command', content }));
    });
  }

  onResponse(cb) { this._responseCbs.push(cb); }
  onLog(cb) { this._logCbs.push(cb); }
  onStatus(cb) { this._statusCbs.push(cb); }

  disconnect() {
    this._intentionalClose = true;
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }
  }
}

window.omoteSerial = new OmoteSerial();
