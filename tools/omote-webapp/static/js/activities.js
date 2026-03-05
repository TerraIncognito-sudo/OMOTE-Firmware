/**
 * Activities management Alpine.js component.
 */
function activitiesComponent() {
  return {
    activities: [],
    selectedActivity: null,
    editMode: false,
    editActivity: {},
    availableDevices: [],
    _deviceCommandCache: {},
    meta: { commandSlots: [] },
    _loaded: false,

    // Physical keypad keys with labels (Rev5 layout)
    physicalKeys: [
      { char: 'o', label: 'Off' },
      { char: '=', label: 'Stop' },
      { char: '<', label: 'Rewind' },
      { char: 'p', label: 'Play' },
      { char: '>', label: 'Forward' },
      { char: 'c', label: 'Config' },
      { char: 'i', label: 'Info' },
      { char: 'u', label: 'Up' },
      { char: 'l', label: 'Left' },
      { char: 'k', label: 'OK' },
      { char: 'r', label: 'Right' },
      { char: 'd', label: 'Down' },
      { char: 'b', label: 'Back' },
      { char: 's', label: 'Source' },
      { char: '+', label: 'Volume+' },
      { char: '-', label: 'Volume-' },
      { char: 'm', label: 'Mute' },
      { char: 'e', label: 'Record' },
      { char: '^', label: 'Channel+' },
      { char: 'v', label: 'Channel-' },
      { char: '1', label: 'Red' },
      { char: '2', label: 'Green' },
      { char: '3', label: 'Yellow' },
      { char: '4', label: 'Blue' },
      { char: '?', label: '?' },
    ],

    keyLabel(keyChar) {
      const numChar = typeof keyChar === 'number' ? keyChar : (typeof keyChar === 'string' ? keyChar.charCodeAt(0) : 0);
      const ch = typeof keyChar === 'string' && keyChar.length === 1 ? keyChar : String.fromCharCode(numChar);
      const found = this.physicalKeys.find(k => k.char === ch);
      return found ? found.label + ' (' + ch + ')' : ch;
    },

    async loadActivities() {
      if (this._loaded) return;
      this._loaded = true;
      try {
        const [actResp, devResp, metaResp] = await Promise.all([
          window.omoteSerial.send('act_list'),
          window.omoteSerial.send('dev_list'),
          window.omoteSerial.send('meta')
        ]);
        this.activities = (actResp.data && actResp.data.activities) || [];
        this.availableDevices = (devResp.data && devResp.data.devices) || [];
        // Cache commands per device
        this._deviceCommandCache = {};
        for (const d of this.availableDevices) {
          if (d.commands) {
            this._deviceCommandCache[d.id] = d.commands.map(c => c.name || c);
          }
        }
        const m = metaResp.data || {};
        if (m.command_slots) this.meta.commandSlots = m.command_slots;
      } catch (e) {
        console.error('Failed to load activities', e);
        this._loaded = false;
      }
    },

    async reloadActivities() {
      this._loaded = false;
      await this.loadActivities();
    },

    selectActivity(id) {
      // Use data already loaded from act_list — no round-trip needed.
      const act = this.activities.find(a => a.id === id);
      if (!act) return;
      this.selectedActivity = act;
      this.editActivity = JSON.parse(JSON.stringify(act));
      this.editActivity.device_ids = this.editActivity.device_ids || [];
      this.editActivity.key_bindings = (this.editActivity.key_bindings || []).map(kb => ({
        ...kb,
        // Firmware sends "key" as ASCII int; normalize to single char string for dropdown
        key_char: typeof kb.key === 'number' ? String.fromCharCode(kb.key) : (kb.key_char || '')
      }));
      this.editActivity.startup_actions = this.editActivity.startup_actions || [];
      this.editMode = true;
    },

    addActivity() {
      this.selectedActivity = null;
      this.editActivity = {
        name: '',
        device_ids: [],
        key_bindings: [],
        startup_actions: []
      };
      this.editMode = true;
    },

    toggleActivityDevice(deviceId, checked) {
      if (!this.editActivity.device_ids) this.editActivity.device_ids = [];
      if (checked) {
        if (!this.editActivity.device_ids.includes(deviceId)) {
          this.editActivity.device_ids.push(deviceId);
        }
      } else {
        this.editActivity.device_ids = this.editActivity.device_ids.filter(id => id !== deviceId);
      }
    },

    activityDevices() {
      if (!this.editActivity.device_ids) return [];
      return this.availableDevices.filter(d => this.editActivity.device_ids.includes(d.id));
    },

    deviceCommands(deviceId) {
      if (!deviceId) return [];
      return this._deviceCommandCache[deviceId] || [];
    },

    addBinding() {
      this.editActivity.key_bindings.push({ key_char: '', device_id: '', command_name: '' });
    },

    removeBinding(index) {
      this.editActivity.key_bindings.splice(index, 1);
    },

    addStartup() {
      this.editActivity.startup_actions.push({ device_id: '', slot: '' });
    },

    removeStartup(index) {
      this.editActivity.startup_actions.splice(index, 1);
    },

    async saveActivity() {
      try {
        const a = this.editActivity;
        // Firmware expects activity fields at top level
        const payload = {
          name: a.name,
          device_ids: a.device_ids || [],
          key_bindings: (a.key_bindings || []).map(kb => ({
            key: typeof kb.key_char === 'string' ? kb.key_char.charCodeAt(0) : kb.key_char,
            device_id: kb.device_id,
            command_name: kb.command_name
          })),
          startup_actions: a.startup_actions || []
        };
        if (a.id) {
          payload.act_id = a.id;
          await window.omoteSerial.send('act_update', payload);
        } else {
          await window.omoteSerial.send('act_add', payload);
        }
        this.editMode = false;
        await this.reloadActivities();
      } catch (e) {
        alert('Save failed: ' + e.message);
      }
    },

    async deleteActivity(id) {
      if (!confirm('Delete this activity?')) return;
      try {
        await window.omoteSerial.send('act_delete', { act_id: id });
        if (this.selectedActivity && this.selectedActivity.id === id) {
          this.selectedActivity = null;
          this.editMode = false;
        }
        await this.reloadActivities();
      } catch (e) {
        alert('Delete failed: ' + e.message);
      }
    }
  };
}
