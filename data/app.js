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

    function renderStatus(data) {
        document.getElementById('battery').textContent = `${Number(data.batteryV || 0).toFixed(1)} V`;
        document.getElementById('solar').textContent = `${Number(data.solarW || 0).toFixed(0)} W`;
        document.getElementById('apstatus').textContent = data.apActive ? `AP active (${data.ssid || 'Tuttli9000'})` : 'AP idle';
    }

    function reload() {
        api('/api/status').then(renderStatus).catch(() => {
            document.getElementById('battery').textContent = '-- V';
            document.getElementById('solar').textContent = '-- W';
            document.getElementById('apstatus').textContent = 'Status unavailable';
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
        timeRow.appendChild(el('label', {}, ':' ));
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

    document.getElementById('add').onclick = () => openForm(null);
    document.getElementById('refresh').onclick = () => reload();
    reload();
})();
