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

    function renderStatus(data) {
        const isValid = !!data.tracerValid;
        const waterStates = [
            `Auto: ${data.newPumpsEnabled ? 'ON' : 'OFF'}`,
            `Top: ${data.topTankFull ? 'FULL' : 'OPEN'}`,
            `Left: ${data.leftTankEmpty ? 'EMPTY' : 'OK'}/${data.leftTankFull ? 'FULL' : 'OPEN'}`,
            `Right: ${data.rightTankEmpty ? 'EMPTY' : 'OK'}/${data.rightTankFull ? 'FULL' : 'OPEN'}`,
            `Pumps: A=${data.autonomousPumpMask || 0} S=${data.scheduledPumpMask || 0}`,
            `Batt: ${Number(data.batteryVoltage || 0).toFixed(2)}V / ${Number(data.batterySoc || 0)}%`
        ].join('\n');
        document.getElementById('waterStates').textContent = waterStates;
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
                el('th', { className: 'pump' }, 'Pump'),
                el('th', { className: 'actions' }, 'Actions')
            ));
            const tbody = el('tbody');

            data.schedules.forEach(s => {
                const tr = el('tr');
                tr.appendChild(el('td', { className: 'time', 'data-label': 'Time' }, `${String(s.hour).padStart(2, '0')}:${String(s.minute).padStart(2, '0')}`));
                tr.appendChild(el('td', { className: 'dur', 'data-label': 'Duration' }, `${s.duration5min * 5} min`));
                tr.appendChild(el('td', { className: 'days', 'data-label': 'Days' }, formatDays(s.weekdays)));
                tr.appendChild(el('td', { className: 'pump', 'data-label': 'Pump' }, (s.pumpMask & 1) ? 'Pump 1' : (s.pumpMask & 2) ? 'Pump 2' : '-'));

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
        const hint = el('p', { className: 'muted' }, 'Set the DS1307 clock and the new autonomous pump logic.');
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
        const pumpEnabledRow = el('div', { className: 'row' });
        const pumpEnabled = el('input', { type: 'checkbox' });
        pumpEnabledRow.appendChild(pumpEnabled);
        pumpEnabledRow.appendChild(el('label', {}, 'Enable new autonomous pumps'));

        const thresholdRow = el('div', { className: 'row' });
        const thresholdInput = el('input', { type: 'number', min: 0, max: 100, value: 80 });
        thresholdRow.appendChild(el('label', {}, 'Battery threshold %: '));
        thresholdRow.appendChild(thresholdInput);

        const cycleRow = el('div', { className: 'row' });
        const cycleInput = el('input', { type: 'number', min: 10000, step: 1000, value: 60000 });
        cycleRow.appendChild(el('label', {}, 'Cycle time ms: '));
        cycleRow.appendChild(cycleInput);

        const pumpSave = el('button', {}, 'Save pump settings');
        pumpSave.onclick = () => {
            const params = new URLSearchParams();
            params.set('enabled', pumpEnabled.checked ? '1' : '0');
            params.set('threshold', String(parseInt(thresholdInput.value, 10) || 80));
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
        pumpSection.appendChild(pumpEnabledRow);
        pumpSection.appendChild(thresholdRow);
        pumpSection.appendChild(cycleRow);
        pumpSection.appendChild(pumpSave);
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
        pumpRow.appendChild(el('label', {}, 'Pump: '));
        pumpRow.appendChild(pump1);
        pumpRow.appendChild(el('label', {}, 'Pump 1'));
        pumpRow.appendChild(pump2);
        pumpRow.appendChild(el('label', {}, 'Pump 2'));

        const actions = el('div', { className: 'row' });
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
    reload();
    startStatusPolling();
})();
