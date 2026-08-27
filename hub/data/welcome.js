'use strict';

// One small snapshot at a time; no map, credentials, commands or SSE clients.
(() => {
    let timer;
    let controller;
    let stopped = false;
    const text = (id, value) => { document.getElementById(id).textContent = value; };
    function reportAge(seconds) {
        if (seconds === null) return 'No reports yet';
        if (seconds < 60) return 'Less than a minute ago';
        if (seconds < 3600) return `${Math.floor(seconds / 60)} min ago`;
        if (seconds < 86400) return `${Math.floor(seconds / 3600)} h ago`;
        return `${Math.floor(seconds / 86400)} d ago`;
    }
    async function refresh() {
        clearTimeout(timer);
        if (stopped || document.hidden || controller) return;
        controller = new AbortController();
        const timeout = setTimeout(() => controller?.abort(), 5000);
        try {
            const response = await fetch('/api/welcome', {cache: 'no-store', signal: controller.signal});
            if (!response.ok) throw new Error('Hub unavailable');
            const data = await response.json();
            if (!/^[0-9A-F]{4}$/.test(data.hub_id)
                || !Number.isInteger(data.recent_collars) || data.recent_collars < 0 || data.recent_collars > 16
                || !(data.last_report_age_s === null || (Number.isInteger(data.last_report_age_s) && data.last_report_age_s >= 0))
                || typeof data.time_synced !== 'boolean') throw new Error('Invalid hub snapshot');
            if (stopped || document.hidden) return;
            text('hub-id', data.hub_id);
            text('recent-collars', String(data.recent_collars));
            text('last-report', reportAge(data.last_report_age_s));
            text('clock-state', data.time_synced ? 'Synced' : 'Approximate');
            text('connection', 'Hub connected · updates every 15 seconds');
        } catch (_) {
            if (stopped || document.hidden) return;
            for (const id of ['hub-id', 'recent-collars', 'last-report', 'clock-state']) text(id, '—');
            text('connection', 'Hub information unavailable. Stay connected to Bluepaws Wi-Fi and try the dashboard link.');
        } finally {
            clearTimeout(timeout);
            controller = undefined;
            if (!stopped && !document.hidden) timer = setTimeout(refresh, 15000);
        }
    }
    document.addEventListener('visibilitychange', () => {
        clearTimeout(timer);
        if (document.hidden) controller?.abort();
        else refresh();
    });
    window.addEventListener('pagehide', () => {
        stopped = true;
        clearTimeout(timer);
        controller?.abort();
    });
    window.addEventListener('pageshow', () => { stopped = false; refresh(); });
    refresh();
})();
