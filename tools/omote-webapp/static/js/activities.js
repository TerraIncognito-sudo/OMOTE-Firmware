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
    saveStatus: '',

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
        // Auto-select last used activity from localStorage
        if (!this.selectedActivity) {
          const lastId = localStorage.getItem('omote_current_activity_id');
          if (lastId) {
            const id = parseInt(lastId, 10);
            if (this.activities.find(a => a.id === id)) {
              this.selectActivity(id);
            }
          }
        }
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
      // Persist selection and notify parent app bar
      localStorage.setItem('omote_current_activity_id', act.id);
      localStorage.setItem('omote_current_activity_name', act.name);
      window.dispatchEvent(new CustomEvent('activity-changed', { detail: { id: act.id, name: act.name } }));
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
      this.saveStatus = '';
      try {
        const a = this.editActivity;
        if (!a.name || !a.name.trim()) {
          this.saveStatus = 'Error: Activity name is required.';
          return;
        }
        // Firmware expects activity fields at top level
        // HTML selects return strings — ensure device_id and key are proper types
        // Filter out incomplete key bindings and startup actions before sending
        const allBindings = a.key_bindings || [];
        // Force any value to integer — handles strings, Alpine proxies, etc.
        const toInt = (v) => { const n = parseInt(String(v), 10); return isNaN(n) ? 0 : n; };
        const validBindings = allBindings.filter(kb => kb.key_char && kb.device_id && kb.command_name);
        const allStartups = a.startup_actions || [];
        const validStartups = allStartups.filter(sa => sa.device_id && sa.slot);
        const skippedBindings = allBindings.length - validBindings.length;
        const skippedStartups = allStartups.length - validStartups.length;
        console.log('[activities] bindings before save:', JSON.stringify(allBindings.map(kb => ({kc: kb.key_char, did: kb.device_id, cmd: kb.command_name, didType: typeof kb.device_id}))));
        const payload = {
          name: a.name.trim(),
          device_ids: (a.device_ids || []).map(id => toInt(id)).filter(id => id > 0),
          key_bindings: validBindings.map(kb => ({
            key: typeof kb.key_char === 'string' ? kb.key_char.charCodeAt(0) : toInt(kb.key_char),
            device_id: toInt(kb.device_id),
            command_name: kb.command_name
          })),
          startup_actions: validStartups.map(sa => ({
            device_id: toInt(sa.device_id),
            slot: sa.slot
          }))
        };
        const isNew = !a.id;
        const cmdName = isNew ? 'act_add' : 'act_update';
        if (!isNew) payload.act_id = a.id;
        this.saveStatus = 'Saving...';
        console.log('[activities] saveActivity sending:', cmdName, JSON.stringify(payload));
        let resp;
        try {
          resp = await window.omoteSerial.send(cmdName, payload);
        } catch (sendErr) {
          this.saveStatus = 'Error: ' + sendErr.message;
          console.error('[activities] send failed:', sendErr);
          return;
        }
        console.log('[activities] saveActivity response:', JSON.stringify(resp));
        // Check firmware response for errors
        if (!resp || resp.ok === false) {
          this.saveStatus = 'Firmware error: ' + (resp ? (resp.error || 'unknown') : 'no response');
          return;
        }
        await this.reloadActivities();
        // Re-select the activity so the edit panel stays open with fresh data
        let savedId;
        if (isNew) {
          // For new activities, use the id from the firmware response if available
          savedId = (resp.data && resp.data.id) ? resp.data.id
            : (this.activities.length > 0 ? this.activities[this.activities.length - 1].id : null);
        } else {
          savedId = a.id;
        }
        console.log('[activities] savedId:', savedId, 'activities:', this.activities.map(x => x.id));
        if (savedId) {
          this.selectActivity(savedId);
        } else {
          this.editMode = false;
        }
        let msg = 'Saved successfully.';
        if (skippedBindings > 0) msg += ` (${skippedBindings} incomplete binding(s) skipped)`;
        if (skippedStartups > 0) msg += ` (${skippedStartups} incomplete action(s) skipped)`;
        this.saveStatus = msg;
        setTimeout(() => { if (this.saveStatus === msg) this.saveStatus = ''; }, 4000);
      } catch (e) {
        this.saveStatus = 'Error: ' + e.message;
        console.error('[activities] saveActivity error:', e);
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
    },

    // -- Visual key map state and methods --
    remoteButtons: REMOTE_BUTTONS,
    imgWidth: REMOTE_IMAGE_WIDTH,
    imgHeight: REMOTE_IMAGE_HEIGHT,
    selectedKey: null,
    assignDeviceId: '',
    assignCommandName: '',

    getBinding(keyChar) {
      if (!this.editActivity || !this.editActivity.key_bindings) return null;
      return this.editActivity.key_bindings.find(kb => kb.key_char === keyChar) || null;
    },

    bindingSummary(keyChar) {
      const kb = this.getBinding(keyChar);
      if (!kb || !kb.device_id || !kb.command_name) return '';
      const dev = (this.availableDevices || []).find(d => d.id == kb.device_id);
      const devName = dev ? dev.name : '?';
      return devName + ': ' + kb.command_name;
    },

    isBound(keyChar) {
      const kb = this.getBinding(keyChar);
      return !!(kb && kb.device_id && kb.command_name);
    },

    renderKeymapSvg() {
      let html = '';
      for (const btn of this.remoteButtons) {
        const pts = btn.coords.map(c => `${c.x},${c.y}`).join(' ');
        const cx = btn.coords.reduce((s, c) => s + c.x, 0) / btn.coords.length;
        const cy = btn.coords.reduce((s, c) => s + c.y, 0) / btn.coords.length;
        const bound = this.isBound(btn.char);
        const selected = this.selectedKey && this.selectedKey.char === btn.char;
        let cls = 'keymap-btn';
        if (bound) cls += ' keymap-btn-bound';
        if (selected) cls += ' keymap-btn-selected';
        const summary = bound ? this.bindingSummary(btn.char) : '';
        // Escape for safe SVG text content
        const esc = (s) => s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
        html += `<g data-key="${btn.char}" style="cursor:pointer">`;
        html += `<polygon points="${pts}" class="${cls}"/>`;
        html += `<text x="${cx}" y="${cy - 4}" class="keymap-label">${esc(btn.label)}</text>`;
        if (bound) {
          html += `<text x="${cx}" y="${cy + 10}" class="keymap-binding-text">${esc(summary)}</text>`;
        }
        html += `</g>`;
      }
      return html;
    },

    handleKeymapClick(event) {
      const g = event.target.closest('g[data-key]');
      if (!g) return;
      const keyChar = g.getAttribute('data-key');
      const btn = this.remoteButtons.find(b => b.char === keyChar);
      if (!btn) return;
      this.selectedKey = btn;
      const kb = this.getBinding(btn.char);
      this.assignDeviceId = kb && kb.device_id ? kb.device_id : '';
      this.assignCommandName = kb && kb.command_name ? kb.command_name : '';
    },

    assignKey() {
      if (!this.selectedKey) return;
      const keyChar = this.selectedKey.char;
      if (!this.editActivity.key_bindings) this.editActivity.key_bindings = [];
      const idx = this.editActivity.key_bindings.findIndex(kb => kb.key_char === keyChar);
      if (this.assignDeviceId && this.assignCommandName) {
        // Ensure device_id is always a number
        const devId = typeof this.assignDeviceId === 'string' ? parseInt(this.assignDeviceId, 10) : Number(this.assignDeviceId);
        const newBinding = {
          key_char: keyChar,
          device_id: devId,
          command_name: this.assignCommandName
        };
        console.log('[activities] assignKey:', keyChar, 'device_id:', devId, 'command:', this.assignCommandName);
        if (idx >= 0) {
          // Replace entire object instead of mutating in place (Alpine proxy safety)
          this.editActivity.key_bindings.splice(idx, 1, newBinding);
        } else {
          this.editActivity.key_bindings.push(newBinding);
        }
      } else if (idx >= 0) {
        this.editActivity.key_bindings.splice(idx, 1);
      }
      this.selectedKey = null;
    },

    clearSelectedKey() {
      if (!this.selectedKey) return;
      this.assignDeviceId = '';
      this.assignCommandName = '';
      this.assignKey();
    },

    cancelAssign() {
      this.selectedKey = null;
    }
  };
}
