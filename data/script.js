// VERSION: 2025-01-03 17:45 UTC - Restored Working JavaScript

class KilnController {
    constructor() {
        this.logElement = document.getElementById('log');
        this.updateInterval = null;
        this.lastStatus = {};
        this.graphData = [];
        this.maxGraphPoints = 75; // 75 points = ~2.5 minutes at 2-second updates

        this.init();
    }

    init() {
        this.log('Kiln Controller Web Interface loaded');
        this.log('VERSION: 2025-01-03 17:45 UTC - Restored');

        this.startStatusUpdates();
        this.setupEventListeners();
    }

    setupEventListeners() {
        const tempInput = document.getElementById('targetTemp');
        if (tempInput) {
            tempInput.addEventListener('keypress', (e) => {
                if (e.key === 'Enter') {
                    this.setTarget();
                }
            });
        }

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

        this.updateStatus();
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

            this.updateElement('temp1', data.temp1.toFixed(1));
            this.updateElement('temp2', data.temp2.toFixed(1));
            this.updateElement('avgTemp', data.avgTemp.toFixed(1));
            this.updateElement('setpoint', data.setpoint.toFixed(1));
            this.updateElement('power', data.power.toFixed(0));

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

            const startBtn = document.getElementById('startBtn');
            const stopBtn = document.getElementById('stopBtn');

            if (startBtn) startBtn.disabled = data.enabled || data.emergency;
            if (stopBtn) stopBtn.disabled = !data.enabled;

            this.updateElement('uptime', this.formatUptime(data.uptime));
            this.updateElement('wifiStatus', data.wifi ? 'Connected' : 'Disconnected');
            this.updateElement('version', data.version || 'Unknown');

            this.checkForAlerts(data);
            this.updateGraph(data);
            this.lastStatus = data;

        } catch (error) {
            this.log(`Failed to update status: ${error.message}`, 'error');

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

    updateGraph(data) {
        // Add new data point
        this.graphData.push({
            temp1: data.temp1,
            temp2: data.temp2,
            target: data.setpoint,
            power: data.power
        });

        // Keep only recent points
        if (this.graphData.length > this.maxGraphPoints) {
            this.graphData.shift();
        }

        // Update current values display
        document.getElementById('graphTemp1').textContent = `T1: ${data.temp1.toFixed(1)}°C`;
        document.getElementById('graphTemp2').textContent = `T2: ${data.temp2.toFixed(1)}°C`;
        document.getElementById('graphTarget').textContent = `Target: ${data.setpoint.toFixed(1)}°C`;
        document.getElementById('graphPower').textContent = `Power: ${data.power.toFixed(0)}%`;

        // Draw the lines
        this.drawGraphLines();
    }

    drawGraphLines() {
        if (this.graphData.length < 2) return;

        const graphWidth = 700; // 750 - 50 for margins
        const graphHeight = 200; // 250 - 50 for margins
        const graphTop = 50;

        // Temperature range for scaling
        const minTemp = 0;
        const maxTemp = 1000;

        const temp1Points = [];
        const temp2Points = [];
        const targetPoints = [];

        this.graphData.forEach((point, index) => {
            // X position: newest data on the left, oldest on the right
            const x = 50 + (index / (this.maxGraphPoints - 1)) * graphWidth;

            // Y positions (inverted because SVG Y increases downward)
            const y1 = graphTop + graphHeight - ((point.temp1 - minTemp) / (maxTemp - minTemp)) * graphHeight;
            const y2 = graphTop + graphHeight - ((point.temp2 - minTemp) / (maxTemp - minTemp)) * graphHeight;
            const yTarget = graphTop + graphHeight - ((point.target - minTemp) / (maxTemp - minTemp)) * graphHeight;

            temp1Points.push(`${x},${y1}`);
            temp2Points.push(`${x},${y2}`);
            targetPoints.push(`${x},${yTarget}`);
        });

        // Update the polyline elements
        document.getElementById('temp1Line').setAttribute('points', temp1Points.join(' '));
        document.getElementById('temp2Line').setAttribute('points', temp2Points.join(' '));
        document.getElementById('targetLine').setAttribute('points', targetPoints.join(' '));
    }

    clearGraph() {
        this.graphData = [];
        document.getElementById('temp1Line').setAttribute('points', '');
        document.getElementById('temp2Line').setAttribute('points', '');
        document.getElementById('targetLine').setAttribute('points', '');
        this.log('Graph cleared', 'info');
    }

    checkForAlerts(data) {
        if (data.temp1 > 1000 || data.temp2 > 1000) {
            if (!this.lastStatus.highTempAlerted) {
                this.log('HIGH TEMPERATURE WARNING: >1000°C detected', 'warning');
                this.lastStatus.highTempAlerted = true;
            }
        } else {
            this.lastStatus.highTempAlerted = false;
        }

        if (data.emergency && !this.lastStatus.emergency) {
            this.log('EMERGENCY STOP ACTIVATED', 'error');
        }

        if (data.enabled !== this.lastStatus.enabled) {
            if (data.enabled) {
                this.log('System started', 'success');
            } else {
                this.log('System stopped', 'info');
            }
        }
    }

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
        if (confirm('Are you sure you want to activate EMERGENCY STOP?')) {
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
                tempInput.value = '';
            } else {
                this.log(response.message, 'error');
            }
        } catch (error) {
            this.log(`Failed to set temperature: ${error.message}`, 'error');
        }
    }

    async startSchedule(index) {
        const scheduleNames = ['Bisque Fire', 'Glaze Fire', 'Test Fire'];
        const scheduleName = scheduleNames[index] || 'Unknown';

        const confirmMsg = `Start "${scheduleName}" firing schedule?`;

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
}

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

function clearGraph() {
    kilnController.clearGraph();
}

document.addEventListener('DOMContentLoaded', () => {
    kilnController = new KilnController();
});

window.addEventListener('beforeunload', () => {
    if (kilnController) {
        kilnController.stopStatusUpdates();
    }
});

// VERSION: 2025-01-03 17:45 UTC - End of file