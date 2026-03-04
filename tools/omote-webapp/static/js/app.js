/**
 * Root Alpine.js component for the OMOTE Companion app.
 */
function app() {
  return {
    activeTab: 'connection',
    connected: false,
    connectedPort: '',
    ports: [],
    selectedPort: '',
    batteryPct: 0,
    batteryCharging: false,
    meta: {
      deviceTypes: [],
      transports: [],
      protocols: [],
      commandSlots: [],
      commandNames: []
    },
    _pollTimer: null,

    init() {
      this.fetchPorts();
      window.omoteSerial.connect();

      window.omoteSerial.onStatus((data) => {
        this.connected = data.connected;
        this.connectedPort = data.port || '';
        if (data.connected) {
          this.fetchMeta();
          this._startPolling();
        } else {
          this._stopPolling();
        }
      });

      window.omoteSerial.onResponse((resp) => {
        if (!resp || !resp.ok) return;
        const d = resp.data || {};
        if (resp.res === 'status') {
          if (d.battery_pct !== undefined) this.batteryPct = d.battery_pct;
          if (d.battery_charging !== undefined) this.batteryCharging = d.battery_charging;
        }
        if (resp.res === 'meta') {
          if (d.device_types) this.meta.deviceTypes = d.device_types;
          if (d.transports) this.meta.transports = d.transports;
          if (d.protocols) this.meta.protocols = d.protocols;
          if (d.command_slots) this.meta.commandSlots = d.command_slots;
          if (d.command_names) this.meta.commandNames = d.command_names;
        }
      });
    },

    async fetchPorts() {
      try {
        const resp = await fetch('/api/ports');
        this.ports = await resp.json();
        // Auto-select ESP32 if found
        const esp = this.ports.find(p => p.is_esp32);
        if (esp && !this.selectedPort) this.selectedPort = esp.port;
      } catch (e) {
        console.error('Failed to fetch ports', e);
      }
    },

    async connectSerial() {
      try {
        const resp = await fetch('/api/connect', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ port: this.selectedPort })
        });
        const result = await resp.json();
        if (!result.ok) alert('Connect failed: ' + result.error);
      } catch (e) {
        alert('Connect error: ' + e.message);
      }
    },

    async disconnectSerial() {
      try {
        await fetch('/api/disconnect', { method: 'POST' });
      } catch (e) {
        console.error('Disconnect error', e);
      }
    },

    async fetchMeta() {
      try {
        const resp = await window.omoteSerial.send('meta');
        const d = resp.data || {};
        if (d.device_types) this.meta.deviceTypes = d.device_types;
        if (d.transports) this.meta.transports = d.transports;
        if (d.protocols) this.meta.protocols = d.protocols;
        if (d.command_slots) this.meta.commandSlots = d.command_slots;
        if (d.command_names) this.meta.commandNames = d.command_names;
      } catch (e) {
        console.warn('Meta fetch failed:', e.message);
      }
    },

    _startPolling() {
      this._stopPolling();
      this._pollTimer = setInterval(() => {
        if (this.connected) {
          window.omoteSerial.send('status').catch(() => {});
        }
      }, 5000);
    },

    _stopPolling() {
      if (this._pollTimer) {
        clearInterval(this._pollTimer);
        this._pollTimer = null;
      }
    }
  };
}
