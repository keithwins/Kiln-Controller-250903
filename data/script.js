// VERSION: 2025-01-03 16:25 UTC - Kiln Controller JavaScript

class KilnController {
    constructor() {
        this.logElement = document.getElementById('log');
        this.updateInterval = null;
        this.schedules = [];
        this.lastStatus = {};
        
        this.init();
    }
    
    init() {
        this.log('Kiln Controller Web Interface loaded');
        this.log('VERSION: 2025-01-03 16:25 UTC');
        
        // Start status updates
        this.startStatusUpdates();
        
        // Load schedules
        this.loadSchedules();
        
        // Set up event listeners
        this.setupEventListeners();
    }
    
    setupEventListeners() {
        // Handle Enter key in temperature input
        const tempInput = document.getElementById('targetTemp');
        if (tempInput) {
            tempInput.addEventListener('keypress', (e) => {
                if (e.key === 'Enter') {
                    this.setTarget();
                }
            });
        }
        
        // Handle visibility change to pause/resume updates
        document.addEventListener('visibilitychange', () => {
            if (document.hidden) {
                this.stopStatusUpdates();
            } else {
                this.startStatusUpdates();
            }
        });
    }
    
    log(message, type = 'info') {
        const timestamp = new Date().toLocaleTimeString();
        const logClass = type !== 'info' ? ` class="${type}"` : '';
        this.logElement.innerHTML += `<span${logClass}>[${timestamp}] ${message}</span><br>`;
        this.logElement.scrollTop = this.logElement.scrollHeight;
    }
    
    clearLog() {
        this.logElement.innerHTML = '';
        this.log('Log cleared');
    }
    
    async apiCall(endpoint, options = {}) {
        try {
            const response = await fetch(endpoint, {
                ...options,
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    ...options.headers
                }
            });
            
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            
            const contentType = response.headers.get('content-type');
            if (contentType && contentType.includes('application/json')) {
                return await response.json();
            } else {
                return await response.text();
            }
        } catch (error) {
            this.log(`API Error: ${error.message}`, 'error');
            throw error;
        }
    }
    
    startStatusUpdates() {
        if (this.updateInterval) {
            clearInterval(this.updateInterval);
        }
        
        this.updateStatus(); // Initial update
        this.updateInterval = setInterval(() => {
            this.updateStatus();
        }, 2000);
    }
    
    stopStatusUpdates() {
        if (this.updateInterval) {
            clearInterval(this.updateInterval);
            this.updateInterval = null;
        }
    }
    
    async updateStatus() {
        try {
            const data = await this.apiCall('/api/status');
            
            // Update temperature displays
            this.updateElement('temp1', data.temp1.toFixed(1));
            this.updateElement('temp2', data.temp2.toFixed(1));
            this.updateElement('avgTemp', data.avgTemp.toFixed(1));
            this.updateElement('setpoint', data.setpoint.toFixed(1));
            this.updateElement('power', data.power.toFixed(0));
            
            // Update status
            let status = 'Ready';
            let statusClass = 'info';
            if (data.emergency) {
                status = 'Emergency';
                statusClass = 'error';
            } else if (data.enabled) {
                status = 'Heating';
                statusClass = 'success';
            }
            
            const statusElement = document.getElementById('status');
            if (statusElement) {
                statusElement.textContent = status;
                statusElement.className = 'value ' + statusClass;
            }
            
            // Update button states
            const startBtn = document.getElementById('startBtn');
            const stopBtn = document.getElementById('stopBtn');
            
            if (startBtn) startBtn.disabled = data.enabled || data.emergency;
            if (stopBtn) stopBtn.disabled = !data.enabled;
            
            // Update system info
            this.updateElement('uptime', this.formatUptime(data.uptime));
            this.updateElement('wifiStatus', data.wifi ? 'Connected' : 'Disconnected');
            this.updateElement('version', data.version || 'Unknown');
            
            // Update schedule status
            if (data.schedule) {
                this.updateScheduleStatus(data.schedule);
            } else {
                this.hideScheduleStatus();
            }
            
            // Check for significant changes
            this.checkForAlerts(data);
            
            this.lastStatus = data;
            
        } catch (error) {
            this.log(`Failed to update status: ${error.message}`, 'error');
            
            // Show offline status
            const elements = ['temp1', 'temp2', 'avgTemp', 'setpoint', 'power', 'status'];
            elements.forEach(id => {
                const el = document.getElementById(id);
                if (el) el.textContent = '--';
            });
        }
    }
    
    updateElement(id, value) {
        const element = document.getElementById(id);
        if (element && element.textContent !== value) {
            element.textContent = value;
            element.classList.add('updating');
            setTimeout(() => element.classList.remove('updating'), 500);
        }
    }
    
    formatUptime(seconds) {
        const hours = Math.floor(seconds / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        const secs = seconds % 60;
        
        if (hours > 0) {
            return `${hours}h ${minutes}m`;
        } else if (minutes > 0) {
            return `${minutes}m ${secs}s`;
        } else {
            return `${secs}s`;
        }
    }
    
    checkForAlerts(data) {
        // Temperature alerts
        if (data.temp1 > 1000 || data.temp2 > 1000) {
            if (!this.lastStatus.highTempAlerted) {
                this.log('HIGH TEMPERATURE WARNING: >1000°C detected', 'warning');
                this.lastStatus.highTempAlerted = true;
            }
        } else {
            this.lastStatus.highTempAlerted = false;
        }
        
        // Emergency stop alert
        if (data.emergency && !this.lastStatus.emergency) {
            this.log('EMERGENCY STOP ACTIVATED', 'error');
        }
        
        // System started/stopped
        if (data.enabled !== this.lastStatus.enabled) {
            if (data.enabled) {
                this.log('System started', 'success');
            } else {
                this.log('System stopped', 'info');
            }
        }
    }
    
    async loadSchedules() {
        try {
            const response = await this.apiCall('/api/control', {
                method: 'POST',
                body: 'action=schedules'
            });
            
            if (response.success && response.schedules) {
                this.schedules = response.schedules;
                this.renderSchedules();
            }
        } catch (error) {
            // If schedules endpoint doesn't exist, create default ones
            this.schedules = [
                { index: 0, name: 'Bisque Fire', segments: 3, maxTemp: 950 },
                { index: 1, name: 'Glaze Fire', segments: 4, maxTemp: 1240 },
                { index: 2, name: 'Test Fire', segments: 2, maxTemp: 200 }
            ];
            this.renderSchedules();
        }
    }
    
    renderSchedules() {
        const grid = document.getElementById('scheduleGrid');
        if (!grid) return;
        
        grid.innerHTML = '';
        
        this.schedules.forEach(schedule => {
            const card = document.createElement('div');
            card.className = 'schedule-card';
            card.onclick = () => this.startSchedule(schedule.index);
            
            card.innerHTML = `
                <h4>${schedule.name}</h4>
                <div class="segments">${schedule.segments} segments • Max: ${schedule.maxTemp}°C</div>
            `;
            
            grid.appendChild(card);
        });
    }
    
    updateScheduleStatus(schedule) {
        const statusDiv = document.getElementById('scheduleStatus');
        const nameEl = document.getElementById('activScheduleName');
        const segmentEl = document.getElementById('activeScheduleSegment');
        const progressEl = document.getElementById('scheduleProgress');
        
        if (statusDiv) statusDiv.style.display = 'block';
        if (nameEl) nameEl.textContent = schedule.name;
        if (segmentEl) segmentEl.textContent = `Segment ${schedule.segment} of ${schedule.total} - Target: ${schedule.target}°C`;
        if (progressEl) {
            const progress = (schedule.segment / schedule.total) * 100;
            progressEl.style.width = progress + '%';
        }
    }
    
    hideScheduleStatus() {
        const statusDiv = document.getElementById('scheduleStatus');
        if (statusDiv) statusDiv.style.display = 'none';
    }
    
    // Control methods
    async startSystem() {
        try {
            const response = await this.apiCall('/api/control', {
                method: 'POST',
                body: 'action=start'
            });
            
            if (response.success) {
                this.log(response.message, 'success');
            } else {
                this.log(response.message, 'error');
            }
        } catch (error) {
            this.log(`Failed to start system: ${error.message}`, 'error');
        }
    }
    
    async stopSystem() {
        try {
            const response = await this.apiCall('/api/control', {
                method: 'POST',
                body: 'action=stop'
            });
            
            if (response.success) {
                this.log(response.message, 'info');
            } else {
                this.log(response.message, 'error');
            }
        } catch (error) {
            this.log(`Failed to stop system: ${error.message}`, 'error');
        }
    }
    
    async emergencyStop() {
        if (confirm('Are you sure you want to activate EMERGENCY STOP? This will immediately shut down all heating.')) {
            try {
                const response = await this.apiCall('/api/control', {
                    method: 'POST',
                    body: 'action=emergency'
                });
                
                if (response.success) {
                    this.log(response.message, 'error');
                } else {
                    this.log(response.message, 'error');
                }
            } catch (error) {
                this.log(`Failed to emergency stop: ${error.message}`, 'error');
            }
        }
    }
    
    async resetSystem() {
        try {
            const response = await this.apiCall('/api/control', {
                method: 'POST',
                body: 'action=reset'
            });
            
            if (response.success) {
                this.log(response.message, 'success');
            } else {
                this.log(response.message, 'error');
            }
        } catch (error) {
            this.log(`Failed to reset system: ${error.message}`, 'error');
        }
    }
    
    async setTarget() {
        const tempInput = document.getElementById('targetTemp');
        if (!tempInput) return;
        
        const temp = parseFloat(tempInput.value);
        
        if (isNaN(temp)) {
            this.log('Please enter a valid temperature', 'error');
            return;
        }
        
        if (temp < 0 || temp > 1200) {
            this.log('Temperature must be between 0-1200°C', 'error');
            return;
        }
        
        try {
            const response = await this.apiCall('/api/control', {
                method: 'POST',
                body: `action=settemp&value=${temp}`
            });
            
            if (response.success) {
                this.log(response.message, 'success');
                tempInput.value = ''; // Clear input
            } else {
                this.log(response.message, 'error');
            }
        } catch (error) {
            this.log(`Failed to set temperature: ${error.message}`, 'error');
        }
    }
    
    async startSchedule(index) {
        const schedule = this.schedules.find(s => s.index === index);
        if (!schedule) {
            this.log('Schedule not found', 'error');
            return;
        }
        
        const confirmMsg = `Start "${schedule.name}" firing schedule?\n\n` +
                          `${schedule.segments} segments, max temperature: ${schedule.maxTemp}°C\n\n` +
                          `This will automatically control the kiln heating.`;
        
        if (confirm(confirmMsg)) {
            try {
                const response = await this.apiCall('/api/control', {
                    method: 'POST',
                    body: `action=schedule&index=${index}`
                });
                
                if (response.success) {
                    this.log(response.message, 'success');
                } else {
                    this.log(response.message, 'error');
                }
            } catch (error) {
                this.log(`Failed to start schedule: ${error.message}`, 'error');
            }
        }
    }
    
    // Utility methods
    showNotification(message, type = 'info') {
        // Create a temporary notification element
        const notification = document.createElement('div');
        notification.className = `notification ${type}`;
        notification.textContent = message;
        notification.style.cssText = `
            position: fixed;
            top: 20px;
            right: 20px;
            padding: 15px 25px;
            background: ${type === 'error' ? '#ff6b6b' : type === 'success' ? '#00ff88' : '#00d4ff'};
            color: ${type === 'success' || type === 'info' ? '#1a1a1a' : 'white'};
            border-radius: 8px;
            z-index: 1000;
            box-shadow: 0 5px 15px rgba(0,0,0,0.3);
            transform: translateX(100%);
            transition: transform 0.3s ease;
        `;
        
        document.body.appendChild(notification);
        
        // Animate in
        setTimeout(() => {
            notification.style.transform = 'translateX(0)';
        }, 100);
        
        // Remove after 5 seconds
        setTimeout(() => {
            notification.style.transform = 'translateX(100%)';
            setTimeout(() => {
                if (notification.parentNode) {
                    notification.parentNode.removeChild(notification);
                }
            }, 300);
        }, 5000);
    }
    
    exportLog() {
        const logContent = this.logElement.innerHTML
            .replace(/<br>/g, '\n')
            .replace(/<[^>]*>/g, ''); // Remove HTML tags
        
        const blob = new Blob([logContent], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        
        const a = document.createElement('a');
        a.href = url;
        a.download = `kiln-log-${new Date().toISOString().split('T')[0]}.txt`;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        this.log('Log exported successfully', 'success');
    }
    
    // Keyboard shortcuts
    handleKeyboard(event) {
        if (event.ctrlKey || event.metaKey) {
            switch (event.key.toLowerCase()) {
                case 's':
                    event.preventDefault();
                    this.startSystem();
                    break;
                case 'x':
                    event.preventDefault();
                    this.stopSystem();
                    break;
                case 'e':
                    event.preventDefault();
                    this.emergencyStop();
                    break;
                case 'r':
                    event.preventDefault();
                    this.resetSystem();
                    break;
                case 'l':
                    event.preventDefault();
                    this.clearLog();
                    break;
            }
        }
    }
}

// Global functions for HTML onclick handlers
let kilnController;

function startSystem() {
    kilnController.startSystem();
}

function stopSystem() {
    kilnController.stopSystem();
}

function emergencyStop() {
    kilnController.emergencyStop();
}

function resetSystem() {
    kilnController.resetSystem();
}

function setTarget() {
    kilnController.setTarget();
}

function startSchedule(index) {
    kilnController.startSchedule(index);
}

function clearLog() {
    kilnController.clearLog();
}

function exportLog() {
    kilnController.exportLog();
}

// Initialize when DOM is loaded
document.addEventListener('DOMContentLoaded', () => {
    kilnController = new KilnController();
    
    // Add keyboard shortcuts
    document.addEventListener('keydown', (e) => {
        kilnController.handleKeyboard(e);
    });
    
    // Add export log button if it doesn't exist
    const logSection = document.querySelector('.log-section');
    if (logSection && !document.getElementById('exportLogBtn')) {
        const exportBtn = document.createElement('button');
        exportBtn.id = 'exportLogBtn';
        exportBtn.className = 'btn-secondary';
        exportBtn.textContent = 'Export Log';
        exportBtn.onclick = exportLog;
        exportBtn.style.marginLeft = '10px';
        
        const clearBtn = logSection.querySelector('button');
        if (clearBtn) {
            clearBtn.parentNode.insertBefore(exportBtn, clearBtn.nextSibling);
        }
    }
});

// Handle page unload
window.addEventListener('beforeunload', () => {
    if (kilnController) {
        kilnController.stopStatusUpdates();
    }
});

// VERSION: 2025-01-03 16:25 UTC - End of file