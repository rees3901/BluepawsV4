/* Local UI semantics. No credentials, radio writes or browser persistence. */
(function (root) {
    'use strict';
    var TTL = 600000, RETAIN = 900000;
    var pending = ['queued', 'transmitted', 'waiting'];
    var states = pending.concat(['acked', 'expired', 'failed', 'superseded']);
    function profileLabel(value) {
        var key = String(value || '').toLowerCase().replace(/[ _-]/g, '');
        return { normal: 'Normal', powersave: 'Power Save', active: 'Active',
            lost: 'Lost Alert', lostalert: 'Lost Alert', emergencylost: 'Lost Alert', debug: 'Debug' }[key] || 'Unknown';
    }
    function createStore(now) {
        now = now || function () { return performance.now(); };
        var commands = {};
        return {
            reset: function () { commands = {}; },
            accept: function (item) {
                if (!item || !Number.isInteger(item.device) || item.device < 1 || item.device > 65534
                    || !Number.isInteger(item.cmdSeq) || item.cmdSeq < 1 || item.cmdSeq > 65535
                    || states.indexOf(item.status) < 0 || !Number.isFinite(item.age_ms) || item.age_ms < 0
                    || ['profile', 'find', 'status'].indexOf(item.type) < 0) return false;
                var key = item.device + ':' + item.cmdSeq, old = commands[key];
                // A late HTTP snapshot must not roll back an SSE ACK or newer state.
                if (old && (item.age_ms < old.reportedAge ||
                    (pending.indexOf(old.status) < 0 && pending.indexOf(item.status) >= 0))) return false;
                if (item.age_ms >= RETAIN) { delete commands[key]; return false; }
                commands[key] = { device: item.device, cmdSeq: item.cmdSeq, type: item.type,
                    profile: profileLabel(item.profile), status: item.status,
                    reportedAge: item.age_ms, created: old ? old.created : now() - item.age_ms };
                Object.keys(commands).forEach(function (k) {
                    if (now() - commands[k].created >= RETAIN) delete commands[k];
                });
                var keys = Object.keys(commands).sort(function (a,b) { return commands[a].created - commands[b].created; });
                while (keys.length > 64) delete commands[keys.shift()];
                return true;
            },
            latest: function (id) {
                var latest = null;
                Object.keys(commands).forEach(function (key) {
                    var cmd = commands[key], age = now() - cmd.created;
                    if (age >= RETAIN) { delete commands[key]; return; }
                    if (cmd.device === Number(id) && (!latest || cmd.created >= latest.created)) latest = cmd;
                });
                if (!latest) return null;
                var status = pending.indexOf(latest.status) >= 0 && now() - latest.created >= TTL ? 'expired' : latest.status;
                var action = latest.type === 'profile' ? 'profile → ' + latest.profile : latest.type === 'find' ? 'Find alert' : 'status request';
                var prefix = {acked:'Command acknowledged', expired:'Command expired — no ACK', failed:'Command not queued', superseded:'Command replaced'}[status] || 'Command pending';
                return { text: prefix + ': ' + action, pending: pending.indexOf(status) >= 0,
                    status: status, seq: latest.cmdSeq };
            }
        };
    }
    function receiveWindow(dev, data, now) {
        var remaining = Number(data.rxWindowMs);
        if (!Number.isFinite(remaining) || remaining <= 0) { dev.rxUntil = 0; return; }
        var deadline = now + Math.min(10000, remaining);
        // Snapshot/appearance refresh of one packet must never restart its window.
        var identity = data.localId == null ? null : String(data.localId);
        dev.rxUntil = identity !== null && identity === dev.rxIdentity
            ? Math.min(dev.rxUntil || 0, deadline) : deadline;
        dev.rxIdentity = identity;
    }
    // Same report-only rules as web/src/lib/collarFault.ts; parity-tested.
    function fault(report, legacyFault) {
        var flags = report && report.flags;
        var hasFlags = typeof flags === 'number' && Number.isInteger(flags) && flags >= 0 && flags <= 255;
        if (hasFlags ? !(flags & 0x80) : !legacyFault) return null;
        var reasons = [];
        if (hasFlags) {
            if (flags & 0x40) reasons.push('stale GPS');
            else if (!(flags & 0x01) && [0,3,4,5].indexOf(report.txReason) >= 0) reasons.push('GPS fix unavailable');
            if (flags & 0x04) reasons.push('low battery');
        }
        var detail = reasons.length ? reasons[0] + (reasons.length > 1 ? ' +' + (reasons.length - 1) : '') : 'cause unspecified';
        var title = reasons.length
            ? 'Reported fault — ' + reasons.join('; ') + '. These indicators accompany ERROR_PRESENT; they do not establish the root cause.'
            : 'Reported fault — cause unspecified. The report does not identify a specific cause.';
        var reset = report && report.resetReason;
        if (hasFlags && typeof reset === 'number' && Number.isInteger(reset) && reset >= 0 && reset <= 255) {
            title += ' Reset diagnostic 0x' + reset.toString(16).padStart(2, '0').toUpperCase() + ' describes the previous reset, not necessarily this fault.';
        }
        return {label: 'Reported fault — ' + detail, title: title};
    }
    root.HubFeedback = { profileLabel: profileLabel, createStore: createStore, receiveWindow: receiveWindow, fault: fault };
})(globalThis);
