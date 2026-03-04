/**
 * Monitor/log viewer Alpine.js component.
 */
function monitorComponent() {
  return {
    logs: [],
    filter: '',
    paused: false,
    maxLines: 1000,

    init() {
      window.omoteSerial.onLog((text) => {
        this.addLog('log', text);
      });

      window.omoteSerial.onResponse((data) => {
        const text = typeof data === 'string' ? data : JSON.stringify(data);
        this.addLog('response', text);
      });
    },

    addLog(type, text) {
      if (this.paused) return;
      const now = new Date();
      const timestamp = now.toLocaleTimeString('en-US', { hour12: false }) +
        '.' + String(now.getMilliseconds()).padStart(3, '0');
      this.logs.push({ type, text, timestamp });
      if (this.logs.length > this.maxLines) {
        this.logs.splice(0, this.logs.length - this.maxLines);
      }
      // Auto-scroll
      this.$nextTick(() => {
        const container = this.$refs.logContainer;
        if (container) {
          container.scrollTop = container.scrollHeight;
        }
      });
    },

    filteredLogs() {
      if (!this.filter) return this.logs;
      const f = this.filter.toLowerCase();
      return this.logs.filter(entry => entry.text.toLowerCase().includes(f));
    },

    clear() {
      this.logs = [];
    },

    togglePause() {
      this.paused = !this.paused;
    }
  };
}
