(function () {
    const names = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun'];

    function el(tag, props = {}, ...kids) {
        const e = document.createElement(tag);
        Object.assign(e, props);
        kids.forEach(k => {
            if (typeof k === 'string') e.appendChild(document.createTextNode(k));
            else if (k) e.appendChild(k);
        });
        return e;
    }

    function api(path, opts) {
        return fetch(path, opts).then(r => r.ok ? r.json() : Promise.reject(r));
    }

    function formatDays(mask) {
        if (mask === 0b1111111) return 'Every day';
        let out = [];
        for (let i = 0; i < 7; i++) if (mask & (1 << i)) out.push(names[i]);
        return out.join(', ') || 'None';
    }

    function batteryState(soc) {
        if (soc < 5) return { label: 'EMPTY', symbol: '[____]' };
        if (soc <= 30) return { label: 'ALMOST EMPTY', symbol: '[|___]' };
        if (soc <= 90) return { label: 'OK', symbol: '[|||_]' };
        return { label: '', symbol: '[||||]' };
    }

    function setFlowValue(id, value) {
        document.getElementById(id).textContent = value;
    }

    function displayNumber(value, unit) {
        return `${value.toFixed(2)}${unit}`;
    }

    function formatBytes(bytes) {
        const value = Number(bytes || 0);
        if (value < 1024 * 1024) return `${(value / 1024).toFixed(0)} KB`;
        return `${(value / (1024 * 1024 * 1024)).toFixed(2)} GB`;
    }

    function tankBox(title, state, lines, extraClass = '') {
        const box = el('div', { className: `tank-box ${extraClass}`.trim() });
        box.appendChild(el('div', { className: 'tank-title' }, title));
        box.appendChild(el('div', { className: 'tank-state' }, state));
        lines.forEach(line => box.appendChild(el('div', { className: 'tank-detail' }, line)));
        return box;
    }

    function nextScheduleLabel(fillEnabled, rawNext) {
        if (!fillEnabled) return 'DISABLED';
        if (!rawNext || rawNext === '--') return 'NO SCHEDULE';
        return rawNext;
    }

    function renderWaterStates(data) {
        const wrap = document.getElementById('waterStates');
        wrap.innerHTML = '';

        const topState = data.topTankFull ? 'FULL' : 'OK';
        const topNext1 = data.nextFillPump1 || '--';
        const topNext2 = data.nextFillPump2 || '--';
        const leftFillEnabled = !!data.fillPump1Enabled;
        const rightFillEnabled = !!data.fillPump2Enabled;
        const leftWateringEnabled = !!data.wateringPump1Enabled;
        const rightWateringEnabled = !!data.wateringPump2Enabled;

        wrap.appendChild(tankBox('TOP TANK', topState, [
            `W.-level: ${topState}`
        ], 'top'));

        wrap.appendChild(tankBox('LEFT TANK', data.leftTankEmpty ? 'EMPTY' : 'OK', [
            `Fill pump (left): ${leftFillEnabled ? (data.fillPump1Active ? 'ON' : 'OFF') : 'DISABLED'} | W.-level: ${data.leftTankEmpty ? 'EMPTY' : 'OK'}`,
            `Watering pump (left): ${leftWateringEnabled ? (data.wateringPump1Active ? 'ON' : 'OFF') : 'DISABLED'}`,
            `Next schedule: ${nextScheduleLabel(leftFillEnabled, topNext1)}`
        ]));

        wrap.appendChild(tankBox('RIGHT TANK', data.rightTankEmpty ? 'EMPTY' : 'OK', [
            `Fill pump (right): ${rightFillEnabled ? (data.fillPump2Active ? 'ON' : 'OFF') : 'DISABLED'} | W.-level: ${data.rightTankEmpty ? 'EMPTY' : 'OK'}`,
            `Watering pump (right): ${rightWateringEnabled ? (data.wateringPump2Active ? 'ON' : 'OFF') : 'DISABLED'}`,
            `Next schedule: ${nextScheduleLabel(rightFillEnabled, topNext2)}`
        ]));
    }

    function renderStatus(data) {
        const isValid = !!data.tracerValid;
        renderWaterStates(data);
        if (isValid) {
            const batteryVNum = Number(data.batteryVoltage || data.batteryV || 0);
            const batteryANum = Number(data.batteryCurrent || 0);
            const batteryV = displayNumber(batteryVNum, 'V');
            const batteryA = displayNumber(batteryANum, 'A');
            const batteryW = displayNumber(batteryVNum * batteryANum, 'W');
            const batteryT = displayNumber(Number(data.batteryTempC || 0), 'C');
            setFlowValue('batteryState', 'Battery');
            setFlowValue('pvVoltage', displayNumber(Number(data.pvVoltage || 0), 'V'));
            setFlowValue('pvCurrent', displayNumber(Number(data.pvCurrent || 0), 'A'));
            setFlowValue('pvPower', displayNumber(Number(data.pvVoltage || 0) * Number(data.pvCurrent || 0), 'W'));
            setFlowValue('batteryVoltage', batteryV);
            setFlowValue('batteryCurrent', batteryA);
            setFlowValue('batteryPower', batteryW);
            setFlowValue('batteryTemp', batteryT);
            setFlowValue('loadVoltage', displayNumber(Number(data.loadVoltage || 0), 'V'));
            setFlowValue('loadCurrent', displayNumber(Number(data.loadCurrent || 0), 'A'));
            setFlowValue('loadPower', displayNumber(Number(data.loadVoltage || 0) * Number(data.loadCurrent || 0), 'W'));

            const pvDay = `${(Number(data.pvDailyWh || 0) / 1000).toFixed(2)} kWh`;
            const pvMonth = `${(Number(data.pvMonthlyWh || 0) / 1000).toFixed(2)} kWh`;
            const pvTotal = `${(Number(data.pvTotalWh || 0) / 1000).toFixed(2)} kWh`;
            document.getElementById('production').textContent = `Day:   ${pvDay}\nMonth: ${pvMonth}\nTotal:  ${pvTotal}`;
        } else {
            setFlowValue('batteryState', 'Battery');
            setFlowValue('pvVoltage', 'xx.xxV');
            setFlowValue('pvCurrent', 'xx.xxA');
            setFlowValue('pvPower', 'xx.xxW');
            setFlowValue('batteryVoltage', 'xx.xxV');
            setFlowValue('batteryCurrent', 'xx.xxA');
            setFlowValue('batteryPower', 'xx.xxW');
            setFlowValue('batteryTemp', 'xx.xxC');
            setFlowValue('loadVoltage', 'xx.xxV');
            setFlowValue('loadCurrent', 'xx.xxA');
            setFlowValue('loadPower', 'xx.xxW');
            document.getElementById('production').textContent = 'Day:   xx.xx kWh\nMonth: xx.xx kWh\nTotal:  xx.xx kWh';
        }

        const clock = document.getElementById('clock');
        if (data.rtcPresent && data.rtcDisplay) {
            const parts = data.rtcDisplay.split(' ');
            const datePart = parts[0] || '';
            const timePart = parts[1] || '';
            clock.textContent = `${datePart} ${timePart}`.trim();
            clock.classList.remove('muted');
        } else {
            clock.textContent = 'RTC unavailable';
            clock.classList.add('muted');
        }

        const sdStatus = document.getElementById('sdStatus');
        const sdState = data.sdWritable ? 'OK' : (data.sdMounted ? 'ERROR' : 'NO CARD');
        const intervalSeconds = Math.round(Number(data.sdIntervalMs || 0) / 1000);
        sdStatus.textContent = `Status: ${sdState}\nInterval: ${intervalSeconds}s\nUsed: ${formatBytes(data.sdUsedBytes)} / ${formatBytes(data.sdTotalBytes)}\nQueue: ${data.sdQueuedRecords || 0}  Dropped: ${data.sdDroppedRecords || 0}`;
        sdStatus.classList.toggle('muted', !data.sdWritable);
    }

    function splitRtcString(value) {
        const parts = (value || '').trim().split(' ');
        const datePart = parts[0] || '';
        const timePart = parts[1] || '';
        const dateMatch = /^([0-9]{2})\.([0-9]{2})\.([0-9]{4})$/.exec(datePart);
        if (!dateMatch) return null;
        return {
            day: parseInt(dateMatch[1], 10),
            month: parseInt(dateMatch[2], 10),
            year: parseInt(dateMatch[3], 10),
            time: timePart
        };
    }

    function parseLogTimestamp(value) {
        const text = String(value || '');
        if (text.length !== 14) return null;
        const year = Number(text.slice(0, 4));
        const month = Number(text.slice(4, 6)) - 1;
        const day = Number(text.slice(6, 8));
        const hour = Number(text.slice(8, 10));
        const minute = Number(text.slice(10, 12));
        const second = Number(text.slice(12, 14));
        const date = new Date(year, month, day, hour, minute, second);
        return Number.isNaN(date.getTime()) ? null : date;
    }

    function dateFromLogFilename(filename) {
        const match = /^(\d{4})(\d{2})(\d{2})\.ndjson$/i.exec(filename || '');
        if (!match) return null;
        return new Date(Number(match[1]), Number(match[2]) - 1, Number(match[3]), 0, 0, 0);
    }

    function parseLog(text, filename) {
        const records = [];
        let invalidLines = 0;
        const fallbackStart = dateFromLogFilename(filename);
        text.split('\n').forEach((line, index) => {
            if (!line.trim()) return;
            try {
                const row = JSON.parse(line);
                const time = row.tv ? parseLogTimestamp(row.t) : null;
                const fallbackTime = fallbackStart ? new Date(fallbackStart.getTime() + index * 10 * 60 * 1000) : null;
                if (time && Array.isArray(row.p) && Array.isArray(row.b)) {
                    records.push({ time, solarW: Number(row.p[2]) / 100, batteryV: Number(row.b[0]) / 100, loadA: Array.isArray(row.l) ? Number(row.l[1]) / 100 : 0 });
                } else if (fallbackTime && Array.isArray(row.p) && Array.isArray(row.b)) {
                    records.push({ time: fallbackTime, solarW: Number(row.p[2]) / 100, batteryV: Number(row.b[0]) / 100, loadA: Array.isArray(row.l) ? Number(row.l[1]) / 100 : 0 });
                } else {
                    invalidLines++;
                }
            } catch (_) {
                // A trailing partial record after power loss is intentionally ignored.
                invalidLines++;
            }
        });
        return { records, invalidLines };
    }

    function downsample(records, maxPoints = 720) {
        if (records.length <= maxPoints) return records;
        const step = records.length / maxPoints;
        const output = [];
        for (let index = 0; index < maxPoints; index++) output.push(records[Math.floor(index * step)]);
        return output;
    }

    function drawHistory(canvas, sourceRecords) {
        const records = downsample(sourceRecords);
        canvas.style.width = `${Math.max(1440, records.length * 12 + 140)}px`;
        const rect = canvas.getBoundingClientRect();
        const pixelRatio = window.devicePixelRatio || 1;
        canvas.width = Math.max(1, Math.floor(rect.width * pixelRatio));
        canvas.height = Math.max(1, Math.floor(rect.height * pixelRatio));
        const context = canvas.getContext('2d');
        context.scale(pixelRatio, pixelRatio);
        const width = rect.width;
        const height = rect.height;
        context.clearRect(0, 0, width, height);
        if (!records.length) {
            context.fillStyle = '#8fa59b';
            context.fillText('No RTC-stamped records in this log.', 20, 30);
            return;
        }

        const margin = { left: 50, right: 100, top: 18, bottom: 38 };
        const chartWidth = width - margin.left - margin.right;
        const chartHeight = height - margin.top - margin.bottom;
        const start = records[0].time.getTime();
        const end = records[records.length - 1].time.getTime();
        const solarMax = Math.max(10, ...records.map(row => row.solarW));
        const batteryMin = Math.floor(Math.min(...records.map(row => row.batteryV)) - 0.5);
        const batteryMax = Math.ceil(Math.max(...records.map(row => row.batteryV)) + 0.5);
        const loadMax = Math.max(0.5, ...records.map(row => row.loadA));
        const range = Math.max(1, end - start);
        const x = row => margin.left + ((row.time.getTime() - start) / range) * chartWidth;
        const solarY = row => margin.top + chartHeight - (row.solarW / solarMax) * chartHeight;
        const batteryY = row => margin.top + chartHeight - ((row.batteryV - batteryMin) / Math.max(0.1, batteryMax - batteryMin)) * chartHeight;
        const loadY = row => margin.top + chartHeight - (row.loadA / loadMax) * chartHeight;

        context.strokeStyle = '#2a3a31';
        context.lineWidth = 1;
        for (let row = 0; row <= 4; row++) {
            const y = margin.top + chartHeight * row / 4;
            context.beginPath(); context.moveTo(margin.left, y); context.lineTo(width - margin.right, y); context.stroke();
        }
        context.font = '12px Arial';
        context.fillStyle = '#e4b44e';
        context.fillText(`0 W`, 4, margin.top + chartHeight + 4);
        context.fillText(`${solarMax.toFixed(0)} W`, 4, margin.top + 4);
        context.fillStyle = '#64c7be';
        context.fillText(`${batteryMin.toFixed(1)} V`, width - 94, margin.top + chartHeight + 4);
        context.fillText(`${batteryMax.toFixed(1)} V`, width - 94, margin.top + 4);
        context.fillStyle = '#d777aa';
        context.fillText(`0 A`, width - 42, margin.top + chartHeight + 4);
        context.fillText(`${loadMax.toFixed(1)} A`, width - 42, margin.top + 4);

        const firstHour = new Date(records[0].time);
        firstHour.setMinutes(0, 0, 0);
        if (firstHour.getTime() < start) firstHour.setHours(firstHour.getHours() + 1);
        for (let hour = new Date(firstHour); hour.getTime() <= end; hour.setHours(hour.getHours() + 1)) {
            const hourX = margin.left + ((hour.getTime() - start) / range) * chartWidth;
            context.strokeStyle = '#2a3a31';
            context.beginPath(); context.moveTo(hourX, margin.top); context.lineTo(hourX, margin.top + chartHeight); context.stroke();
            context.fillStyle = '#8fa59b';
            context.fillText(String(hour.getHours()).padStart(2, '0'), hourX - 6, height - 10);
        }

        const drawLine = (color, y) => {
            context.strokeStyle = color;
            context.lineWidth = 2;
            context.beginPath();
            records.forEach((row, index) => index === 0 ? context.moveTo(x(row), y(row)) : context.lineTo(x(row), y(row)));
            context.stroke();
        };
        drawLine('#e4b44e', solarY);
        drawLine('#64c7be', batteryY);
        drawLine('#d777aa', loadY);
    }

    function openHistory() {
        const wrap = document.getElementById('settingswrap');
        wrap.className = 'modal';
        wrap.style.display = 'flex';
        wrap.innerHTML = '';
        wrap.onclick = event => { if (event.target === wrap) wrap.style.display = 'none'; };

        const panel = el('div', { className: 'modal-panel history-panel' });
        const title = el('h3', {}, 'Solar and Battery History');
        const controls = el('div', { className: 'row' });
        const fileSelect = el('select', {});
        const load = el('button', {}, 'Load day');
        const close = el('button', { className: 'secondary' }, 'Close');
        const message = el('p', { className: 'muted' }, 'Loading available log days...');
        const legend = el('p', { className: 'chart-legend' },
            el('span', { className: 'solar-series' }, 'Solar power (W)'),
            el('span', { className: 'voltage-series' }, 'Battery voltage (V)'),
            el('span', { className: 'current-series' }, 'Load current (A)'));
        const canvas = el('canvas', { className: 'history-chart' });
        const chartScroll = el('div', { className: 'history-chart-scroll' }, canvas);
        close.onclick = () => { wrap.style.display = 'none'; };
        load.onclick = () => {
            if (!fileSelect.value) return;
            message.textContent = 'Loading log...';
            fetch(`/api/log-file?name=${encodeURIComponent(fileSelect.value)}`).then(response => {
                if (!response.ok) throw new Error('log unavailable');
                return response.text();
            }).then(text => {
                const parsed = parseLog(text, fileSelect.value);
                if (!parsed.records.length) {
                    const preview = text.slice(0, 80).replace(/\s+/g, ' ');
                    message.textContent = `No valid records: received ${text.length} bytes, ${parsed.invalidLines} invalid lines. ${preview}`;
                    drawHistory(canvas, []);
                    return;
                }
                message.textContent = `${parsed.records.length} records shown from ${fileSelect.value}`;
                drawHistory(canvas, parsed.records);
            }).catch(() => { message.textContent = 'Unable to load this log.'; });
        };
        controls.appendChild(fileSelect); controls.appendChild(load); controls.appendChild(close);
        panel.appendChild(title); panel.appendChild(controls); panel.appendChild(message); panel.appendChild(legend); panel.appendChild(chartScroll);
        wrap.appendChild(panel);

        api('/api/logs').then(data => {
            const files = (data.files || []).sort((left, right) => right.name.localeCompare(left.name));
            files.forEach(file => {
                const label = /^\d{8}\.ndjson$/i.test(file.name) ? file.name.slice(0, 8) : `${file.name} (RTC unavailable)`;
                fileSelect.appendChild(el('option', { value: file.name }, `${label} (${formatBytes(file.size)})`));
            });
            if (!files.length) {
                message.textContent = 'No log files are available yet.';
                load.disabled = true;
                return;
            }
            message.textContent = 'Choose a day to view its stored measurements.';
            load.click();
        }).catch(() => { message.textContent = 'SD log listing is unavailable.'; load.disabled = true; });
    }

    let statusTimer = null;

    function reload() {
        api('/api/status').then(renderStatus).catch(() => {
            setFlowValue('batteryState', 'EMPTY');
            setFlowValue('pvVoltage', 'xx.xxV');
            setFlowValue('pvCurrent', 'xx.xxA');
            setFlowValue('pvPower', 'xx.xxW');
            setFlowValue('batteryVoltage', 'xx.xxV');
            setFlowValue('batteryCurrent', 'xx.xxA');
            setFlowValue('batteryPower', 'xx.xxW');
            setFlowValue('batteryTemp', 'xx.xxC');
            setFlowValue('loadVoltage', 'xx.xxV');
            setFlowValue('loadCurrent', 'xx.xxA');
            setFlowValue('loadPower', 'xx.xxW');
            document.getElementById('production').textContent = 'Day:   xx.xx kWh\nMonth: xx.xx kWh\nTotal:  xx.xx kWh';
            renderWaterStates({});
        });

        api('/api/schedules').then(data => {
            const wrap = document.getElementById('tablewrap');
            wrap.innerHTML = '';
            wrap.className = 'table-wrap';
            if (!data.schedules || !data.schedules.length) {
                wrap.innerHTML = '<p>No automation entries yet.</p>';
                return;
            }

            const tbl = el('table');
            const thead = el('thead');
            thead.appendChild(el('tr', {},
                el('th', { className: 'time' }, 'Time'),
                el('th', { className: 'dur' }, 'Duration'),
                el('th', { className: 'days' }, 'Days'),
                el('th', { className: 'pump' }, 'Top fill pump'),
                el('th', { className: 'actions' }, 'Actions')
            ));
            const tbody = el('tbody');

            data.schedules.forEach(s => {
                const tr = el('tr');
                tr.appendChild(el('td', { className: 'time', 'data-label': 'Time' }, `${String(s.hour).padStart(2, '0')}:${String(s.minute).padStart(2, '0')}`));
                tr.appendChild(el('td', { className: 'dur', 'data-label': 'Duration' }, `${s.duration5min * 5} min`));
                tr.appendChild(el('td', { className: 'days', 'data-label': 'Days' }, formatDays(s.weekdays)));
                tr.appendChild(el('td', { className: 'pump', 'data-label': 'Top fill pump' }, (s.pumpMask & 1) ? 'Left tank fill pump' : (s.pumpMask & 2) ? 'Right tank fill pump' : '-'));

                const actions = el('td', { className: 'actions', 'data-label': 'Actions' });
                const editBtn = el('button', {}, 'Edit');
                editBtn.onclick = () => openForm(s);
                const delBtn = el('button', { className: 'secondary' }, 'Delete');
                delBtn.onclick = () => {
                    fetch('/api/schedules/delete?id=' + s.id, { method: 'DELETE' }).then(() => reload());
                };
                actions.appendChild(editBtn);
                actions.appendChild(delBtn);
                tr.appendChild(actions);
                tbody.appendChild(tr);
            });

            tbl.appendChild(thead);
            tbl.appendChild(tbody);
            wrap.appendChild(tbl);
        });
    }

    function openSettings() {
        const wrap = document.getElementById('settingswrap');
        wrap.className = 'modal';
        wrap.style.display = 'flex';
        wrap.innerHTML = '';
        wrap.onclick = (e) => { if (e.target === wrap) wrap.style.display = 'none'; };

        const panel = el('div', { className: 'modal-panel' });
        const title = el('h3', {}, 'RTC and pump settings');
        const hint = el('p', { className: 'muted' }, 'Set the DS1307 clock and separate fill/watering pump logic per tank.');
        const liveValue = el('p', { className: 'muted' }, 'RTC value: loading…');
        const form = el('div', { className: 'form' });

        const dateRow = el('div', { className: 'row' });
        const dayInput = el('input', { type: 'number', min: 1, max: 31, inputMode: 'numeric' });
        const monthInput = el('input', { type: 'number', min: 1, max: 12, inputMode: 'numeric' });
        const yearInput = el('input', { type: 'number', min: 2000, max: 2099, inputMode: 'numeric' });
        const timeInput = el('input', { type: 'time' });
        dateRow.appendChild(el('label', {}, 'Date (DD.MM.YYYY): '));
        dateRow.appendChild(dayInput);
        dateRow.appendChild(el('span', {}, '.'));
        dateRow.appendChild(monthInput);
        dateRow.appendChild(el('span', {}, '.'));
        dateRow.appendChild(yearInput);
        dateRow.appendChild(el('label', {}, ' Time: '));
        dateRow.appendChild(timeInput);

        const pumpSection = el('div', { className: 'form' });
        const fillPump1Row = el('div', { className: 'row' });
        const fillPump1Enabled = el('input', { type: 'checkbox' });
        fillPump1Row.appendChild(fillPump1Enabled);
        fillPump1Row.appendChild(el('label', {}, 'Enable left tank fill pump'));

        const fillPump2Row = el('div', { className: 'row' });
        const fillPump2Enabled = el('input', { type: 'checkbox' });
        fillPump2Row.appendChild(fillPump2Enabled);
        fillPump2Row.appendChild(el('label', {}, 'Enable right tank fill pump'));

        const pump1Row = el('div', { className: 'row' });
        const pump1Enabled = el('input', { type: 'checkbox' });
        pump1Row.appendChild(pump1Enabled);
        pump1Row.appendChild(el('label', {}, 'Enable left tank watering pump'));

        const pump2Row = el('div', { className: 'row' });
        const pump2Enabled = el('input', { type: 'checkbox' });
        pump2Row.appendChild(pump2Enabled);
        pump2Row.appendChild(el('label', {}, 'Enable right tank watering pump'));

        const thresholdRow = el('div', { className: 'row' });
        const thresholdInput = el('input', { type: 'number', min: 0, max: 100, step: 0.1, value: 20 });
        thresholdRow.appendChild(el('label', {}, 'PV voltage threshold (V): '));
        thresholdRow.appendChild(thresholdInput);

        const cycleRow = el('div', { className: 'row' });
        const cycleInput = el('input', { type: 'number', min: 10000, step: 1000, value: 60000 });
        cycleRow.appendChild(el('label', {}, 'Cycle time ms: '));
        cycleRow.appendChild(cycleInput);

        const loggingRow = el('div', { className: 'row' });
        const loggingIntervalInput = el('input', { type: 'number', min: 10000, max: 3600000, step: 1000, value: 600000 });
        loggingRow.appendChild(el('label', {}, 'SD logging interval (ms): '));
        loggingRow.appendChild(loggingIntervalInput);

        const loggingSave = el('button', {}, 'Save logging interval');
        loggingSave.onclick = () => {
            const params = new URLSearchParams();
            params.set('intervalMs', String(parseInt(loggingIntervalInput.value, 10) || 600000));
            fetch('/api/logging/config', { method: 'POST', body: params }).then(() => reload()).catch(() => alert('Unable to save logging interval.'));
        };

        const pumpSave = el('button', {}, 'Save pump settings');
        pumpSave.onclick = () => {
            const params = new URLSearchParams();
            params.set('fillPump1Enabled', fillPump1Enabled.checked ? '1' : '0');
            params.set('fillPump2Enabled', fillPump2Enabled.checked ? '1' : '0');
            params.set('wateringPump1Enabled', pump1Enabled.checked ? '1' : '0');
            params.set('wateringPump2Enabled', pump2Enabled.checked ? '1' : '0');
            params.set('pvThresholdV', String(parseFloat(thresholdInput.value) || 20));
            params.set('cycleMs', String(parseInt(cycleInput.value, 10) || 60000));
            fetch('/api/pumps/config', { method: 'POST', body: params }).then(() => reload()).catch(() => alert('Unable to save pump settings.'));
        };

        const actions = el('div', { className: 'row' });
        const save = el('button', {}, 'Save time');
        save.onclick = () => {
            const day = parseInt(dayInput.value, 10);
            const month = parseInt(monthInput.value, 10);
            const year = parseInt(yearInput.value, 10);
            const [hour, minute] = timeInput.value.split(':').map(v => parseInt(v, 10));
            const url = `/api/rtc/set?year=${year}&month=${month}&day=${day}&hour=${hour}&minute=${minute}&second=0`;
            fetch(url, { method: 'POST' }).then(() => {
                wrap.style.display = 'none';
                reload();
            }).catch(() => {
                alert('Unable to set RTC time.');
            });
        };
        const sync = el('button', { className: 'secondary' }, 'Sync time to Epever');
        sync.onclick = () => {
            fetch('/api/epever/sync-time', { method: 'POST' }).then(async (res) => {
                if (!res.ok) throw new Error('sync failed');
                wrap.style.display = 'none';
                reload();
            }).catch(() => {
                alert('Unable to sync RTC time to Epever.');
            });
        };
        const cancel = el('button', { className: 'secondary' }, 'Cancel');
        cancel.onclick = () => { wrap.style.display = 'none'; };
        actions.appendChild(save);
        actions.appendChild(sync);
        actions.appendChild(cancel);

        api('/api/status').then(data => {
            liveValue.textContent = `RTC value: ${data.rtcDisplay || 'unavailable'}`;
            fillPump1Enabled.checked = !!data.fillPump1Enabled;
            fillPump2Enabled.checked = !!data.fillPump2Enabled;
            pump1Enabled.checked = !!data.wateringPump1Enabled;
            pump2Enabled.checked = !!data.wateringPump2Enabled;
            thresholdInput.value = String(Number(data.pvVoltageThresholdV || 20));
            cycleInput.value = String(parseInt(data.autonomousCycleMs, 10) || 60000);
            loggingIntervalInput.value = String(parseInt(data.sdIntervalMs, 10) || 600000);
            const current = splitRtcString(data.rtcDisplay || '');
            if (current) {
                dayInput.value = String(current.day).padStart(2, '0');
                monthInput.value = String(current.month).padStart(2, '0');
                yearInput.value = String(current.year);
                timeInput.value = current.time;
            } else {
                const now = new Date();
                dayInput.value = String(now.getDate()).padStart(2, '0');
                monthInput.value = String(now.getMonth() + 1).padStart(2, '0');
                yearInput.value = String(now.getFullYear());
                timeInput.value = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
            }
        }).catch(() => {
            const now = new Date();
            dayInput.value = String(now.getDate()).padStart(2, '0');
            monthInput.value = String(now.getMonth() + 1).padStart(2, '0');
            yearInput.value = String(now.getFullYear());
            timeInput.value = `${String(now.getHours()).padStart(2, '0')}:${String(now.getMinutes()).padStart(2, '0')}`;
        });

        form.appendChild(dateRow);
        form.appendChild(actions);
        pumpSection.appendChild(fillPump1Row);
        pumpSection.appendChild(fillPump2Row);
        pumpSection.appendChild(pump1Row);
        pumpSection.appendChild(pump2Row);
        pumpSection.appendChild(thresholdRow);
        pumpSection.appendChild(cycleRow);
        pumpSection.appendChild(pumpSave);
        pumpSection.appendChild(loggingRow);
        pumpSection.appendChild(loggingSave);
        panel.appendChild(title);
        panel.appendChild(hint);
        panel.appendChild(liveValue);
        panel.appendChild(form);
        panel.appendChild(pumpSection);
        wrap.appendChild(panel);
    }

    function openForm(s) {
        const wrap = document.getElementById('formwrap');
        wrap.className = 'modal';
        wrap.style.display = 'flex';
        wrap.innerHTML = '';
        wrap.onclick = (e) => { if (e.target === wrap) wrap.style.display = 'none'; };

        const isNew = !s;
        s = s || { hour: 6, minute: 0, duration5min: 1, weekdays: 0b1111111, repeatEvery: 1, pumpMask: 1 };

        const panel = el('div', { className: 'modal-panel' });
        const title = el('h3', {}, isNew ? 'Add automation entry' : 'Edit automation entry');
        const form = el('div', { className: 'form' });

        const timeRow = el('div', { className: 'row' });
        const hourInput = el('input', { type: 'number', min: 0, max: 23, value: s.hour });
        const minuteInput = el('input', { type: 'number', min: 0, max: 55, step: 5, value: s.minute });
        timeRow.appendChild(el('label', {}, 'Time: '));
        timeRow.appendChild(hourInput);
        timeRow.appendChild(el('label', {}, ':'));
        timeRow.appendChild(minuteInput);

        const durationRow = el('div', { className: 'row' });
        const duration = el('select', {});
        for (let i = 1; i <= 12; i++) {
            const v = i * 5;
            const option = el('option', { value: i }, `${v} min`);
            if (i === s.duration5min) option.selected = true;
            duration.appendChild(option);
        }
        durationRow.appendChild(el('label', {}, 'Duration: '));
        durationRow.appendChild(duration);

        const dayGrid = el('div', { className: 'day-grid' });
        names.forEach((name, i) => {
            const id = `d${i}`;
            const label = el('label', { htmlFor: id }, name);
            const cb = el('input', { type: 'checkbox', id, value: i });
            if (s.weekdays & (1 << i)) cb.checked = true;
            label.prepend(cb);
            dayGrid.appendChild(label);
        });

        const pumpRow = el('div', { className: 'row' });
        const pump1 = el('input', { type: 'radio', name: 'pump', value: 1 });
        const pump2 = el('input', { type: 'radio', name: 'pump', value: 2 });
        if (s.pumpMask & 1) pump1.checked = true;
        if (s.pumpMask & 2) pump2.checked = true;
        pumpRow.appendChild(el('label', {}, 'Top fill pump: '));
        pumpRow.appendChild(pump1);
        pumpRow.appendChild(el('label', {}, 'Left tank fill pump'));
        pumpRow.appendChild(pump2);
        pumpRow.appendChild(el('label', {}, 'Right tank fill pump'));
        const save = el('button', {}, 'Save');
        save.onclick = () => {
            const weekdays = Array.from(dayGrid.querySelectorAll('input[type=checkbox]')).reduce((acc, cb, i) => acc | (cb.checked ? (1 << i) : 0), 0);
            const pumpMask = wrap.querySelector('input[name=pump]:checked') ? parseInt(wrap.querySelector('input[name=pump]:checked').value) : 1;
            const payload = {
                hour: parseInt(hourInput.value, 10),
                minute: parseInt(minuteInput.value, 10),
                duration5min: parseInt(duration.value, 10),
                weekdays,
                repeatEvery: 1,
                pumpMask
            };

            const method = isNew ? 'POST' : 'PUT';
            const url = isNew ? '/api/schedules' : '/api/schedules/update?id=' + s.id;
            fetch(url, { method, body: JSON.stringify(payload) }).then(() => {
                wrap.style.display = 'none';
                reload();
            });
        };

        const cancel = el('button', { className: 'secondary' }, 'Cancel');
        cancel.onclick = () => { wrap.style.display = 'none'; };

        actions.appendChild(save);
        actions.appendChild(cancel);

        form.appendChild(timeRow);
        form.appendChild(durationRow);
        form.appendChild(dayGrid);
        form.appendChild(pumpRow);
        form.appendChild(actions);

        panel.appendChild(title);
        panel.appendChild(form);
        wrap.appendChild(panel);
    }

    function startStatusPolling() {
        if (statusTimer) clearInterval(statusTimer);
        statusTimer = setInterval(() => {
            api('/api/status').then(renderStatus).catch(() => {});
        }, 5000);
    }

    document.getElementById('add').onclick = () => openForm(null);
    document.getElementById('refresh').onclick = () => reload();
    document.getElementById('settings').onclick = () => openSettings();
    document.getElementById('history').onclick = () => openHistory();
    reload();
    startStatusPolling();
})();
