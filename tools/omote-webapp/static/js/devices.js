/**
 * Devices management Alpine.js component.
 */
function devicesComponent() {
  return {
    devices: [],
    selectedDevice: null,
    editMode: false,
    editDevice: {},
    meta: { deviceTypes: [], transports: [], protocols: [] },
    _loaded: false,

    async loadDevices() {
      if (this._loaded) return;
      this._loaded = true;
      try {
        const [devResp, metaResp] = await Promise.all([
          window.omoteSerial.send('dev_list'),
          window.omoteSerial.send('meta')
        ]);
        this.devices = (devResp.data && devResp.data.devices) || [];
        const m = metaResp.data || {};
        if (m.device_types) this.meta.deviceTypes = m.device_types;
        if (m.transport_types) this.meta.transports = m.transport_types;
        if (m.ir_protocols) this.meta.protocols = m.ir_protocols.map(p => p.name || p);
      } catch (e) {
        console.error('Failed to load devices', e);
        this._loaded = false;
      }
    },

    async reloadDevices() {
      this._loaded = false;
      await this.loadDevices();
    },

    selectDevice(id) {
      // Use data already loaded from dev_list — no round-trip needed.
      const dev = this.devices.find(d => d.id === id);
      if (!dev) return;
      this.selectedDevice = dev;
      this.editDevice = JSON.parse(JSON.stringify(dev));
      this.editDevice.commands = this.editDevice.commands || [];
      this.editMode = true;
    },

    addDevice() {
      this.selectedDevice = null;
      this.editDevice = {
        name: '',
        type: '',
        transport: '',
        protocol: '',
        address: '',
        enabled: true,
        commands: []
      };
      this.editMode = true;
    },

    editExisting(device) {
      this.editDevice = JSON.parse(JSON.stringify(device));
      this.editDevice.commands = this.editDevice.commands || [];
      this.editMode = true;
    },

    async saveDevice() {
      try {
        const d = this.editDevice;
        // Firmware expects device fields at top level of the JSON command
        const payload = {
          name: d.name,
          type: d.type,
          transport: d.transport,
          ir_protocol_name: d.ir_protocol_name || d.ir_protocol || d.protocol || 'NEC',
          address: d.address || '',
          enabled: d.enabled !== false,
          commands: d.commands || []
        };
        if (d.id) {
          payload.dev_id = d.id;
          await window.omoteSerial.send('dev_update', payload);
        } else {
          await window.omoteSerial.send('dev_add', payload);
        }
        this.editMode = false;
        await this.reloadDevices();
      } catch (e) {
        alert('Save failed: ' + e.message);
      }
    },

    async deleteDevice(id) {
      if (!confirm('Delete this device?')) return;
      try {
        await window.omoteSerial.send('dev_delete', { dev_id: id });
        if (this.selectedDevice && this.selectedDevice.id === id) {
          this.selectedDevice = null;
          this.editMode = false;
        }
        await this.reloadDevices();
      } catch (e) {
        alert('Delete failed: ' + e.message);
      }
    },

    addCommand() {
      this.editDevice.commands.push({ name: '', payload: '' });
    },

    removeCommand(index) {
      this.editDevice.commands.splice(index, 1);
    },

    testStatus: '',

    async testCommand(deviceId, commandName) {
      if (!deviceId || !commandName) {
        this.testStatus = 'Save the device first before testing.';
        return;
      }
      this.testStatus = 'Sending ' + commandName + '...';
      try {
        const resp = await window.omoteSerial.send('dispatch', {
          device_id: deviceId,
          command: commandName
        });
        const d = resp.data || {};
        this.testStatus = d.sent ? ('Sent: ' + commandName) : ('Failed: ' + (d.detail || 'unknown error'));
        setTimeout(() => { this.testStatus = ''; }, 3000);
      } catch (e) {
        this.testStatus = 'Error: ' + e.message;
        setTimeout(() => { this.testStatus = ''; }, 3000);
      }
    }
  };
}
