/**
 * Backup & Restore Alpine.js component.
 */
function backupComponent() {
  return {
    backups: [],
    selectedBackups: [],
    status: '',
    uploading: false,
    uploadProgress: 0,

    async createBackup() {
      this.status = 'Creating backup...';
      try {
        const resp = await window.omoteSerial.send('backup_sd');
        this.status = (resp.data && resp.data.status) || 'Backup created.';
        await this.listBackups();
      } catch (e) {
        this.status = 'Backup failed: ' + e.message;
      }
    },

    async listBackups() {
      try {
        const resp = await window.omoteSerial.send('backup_list');
        this.backups = (resp.data && resp.data.backups) || [];
        this.selectedBackups = [];
      } catch (e) {
        this.status = 'Failed to list backups: ' + e.message;
      }
    },

    toggleBackupSelection(path) {
      const idx = this.selectedBackups.indexOf(path);
      if (idx >= 0) {
        this.selectedBackups.splice(idx, 1);
      } else {
        this.selectedBackups.push(path);
      }
    },

    isSelected(path) {
      return this.selectedBackups.includes(path);
    },

    async restoreBackup(path) {
      if (!confirm('Restore from backup "' + path + '"? This will overwrite current data.')) return;
      this.status = 'Restoring...';
      try {
        const resp = await window.omoteSerial.send('restore_sd', { path });
        this.status = (resp.data && resp.data.status) || 'Restore complete.';
      } catch (e) {
        this.status = 'Restore failed: ' + e.message;
      }
    },

    async exportSelected() {
      if (this.selectedBackups.length === 0) {
        this.status = 'Select one or more backups from the list first.';
        return;
      }
      for (const path of this.selectedBackups) {
        await this.downloadBackupFile(path);
      }
    },

    async downloadBackupFile(path) {
      this.status = 'Downloading ' + path + '...';
      try {
        // Open file for reading
        const startResp = await window.omoteSerial.send('sd_read_start', { path });
        const fileSize = (startResp.data && startResp.data.size) || 0;
        if (fileSize === 0) {
          await window.omoteSerial.send('sd_read_end');
          this.status = 'File is empty: ' + path;
          return;
        }

        // Read in chunks
        const chunkSize = 512;
        let collected = '';
        let offset = 0;
        while (offset < fileSize) {
          const chunkResp = await window.omoteSerial.send('sd_read_chunk', { offset, size: chunkSize });
          const d = chunkResp.data || {};
          if (!d.data || d.bytes === 0) break;
          // Decode base64
          collected += atob(d.data);
          offset += d.bytes;
        }
        await window.omoteSerial.send('sd_read_end');

        // Trigger browser download
        const filename = path.split('/').pop() || 'omote_backup.txt';
        const blob = new Blob([collected], { type: 'application/octet-stream' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = filename;
        a.click();
        URL.revokeObjectURL(url);
        this.status = 'Downloaded: ' + filename;
      } catch (e) {
        this.status = 'Download failed: ' + e.message;
        try { await window.omoteSerial.send('sd_read_end'); } catch (_) {}
      }
    },

    async exportCurrentState() {
      this.status = 'Exporting current state...';
      try {
        const resp = await window.omoteSerial.send('backup_export');
        const content = (resp.data && resp.data.payload) || JSON.stringify(resp);
        const blob = new Blob([content], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = 'omote_backup_' + new Date().toISOString().slice(0, 10) + '.txt';
        a.click();
        URL.revokeObjectURL(url);
        this.status = 'Export downloaded.';
      } catch (e) {
        this.status = 'Export failed: ' + e.message;
      }
    },

    async importBackup(file) {
      if (!file) return;
      this.status = 'Importing...';
      try {
        const text = await file.text();
        const resp = await window.omoteSerial.send('backup_import', { payload: text });
        this.status = (resp.data && resp.data.status) || 'Import complete.';
      } catch (e) {
        this.status = 'Import failed: ' + e.message;
      }
    },

    async uploadIconPack(file) {
      if (!file) return;
      this.uploading = true;
      this.uploadProgress = 0;
      this.status = 'Uploading icon pack...';

      try {
        const buffer = await file.arrayBuffer();
        const bytes = new Uint8Array(buffer);
        const chunkSize = 512;
        const totalChunks = Math.ceil(bytes.length / chunkSize);

        await window.omoteSerial.send('sd_write_start', { path: '/omote_v2_icons.csv' });

        for (let i = 0; i < totalChunks; i++) {
          const chunk = bytes.slice(i * chunkSize, (i + 1) * chunkSize);
          let binary = '';
          for (let j = 0; j < chunk.length; j++) {
            binary += String.fromCharCode(chunk[j]);
          }
          const b64 = btoa(binary);
          await window.omoteSerial.send('sd_write_chunk', { data: b64 });
          this.uploadProgress = Math.round(((i + 1) / totalChunks) * 100);
        }

        await window.omoteSerial.send('sd_write_end');
        this.status = 'Icon pack uploaded successfully.';
      } catch (e) {
        this.status = 'Upload failed: ' + e.message;
      } finally {
        this.uploading = false;
      }
    }
  };
}
