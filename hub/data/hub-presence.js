/* Hub-only marker: never inserted into collar telemetry or command arrays. */
(function (root) {
    'use strict';
    var marker = null;
    root.HubPresencePanel = {
        point: function () { return marker ? marker.getLatLng() : null; },
        start: function (map, protectedFetch, onPosition) {
            var card = document.createElement('article');
            card.className = 'device-card hub-card';
            document.getElementById('deviceCards').prepend(card);
            var expanded = false, editing = false, busy = false, last = null, lastReceived = 0;
            function el(tag, text, className) {
                var e=document.createElement(tag); if(text != null)e.textContent=text;
                if(className)e.className=className; return e;
            }
            function button(text, action) {
                var b=el('button',text,'btn-action'); b.type='button'; b.onclick=action; return b;
            }
            function send(values) {
                return protectedFetch('/api/hub-preferences', {method:'POST',
                    headers:{'Content-Type':'application/json'}, body:JSON.stringify(values)})
                    .then(function(r){if(!r.ok)throw new Error('Could not save hub preferences'); editing=false; return load();})
                    .catch(function(e){card.appendChild(el('p',e.message));});
            }
            function render(s) {
                card.replaceChildren();
                var emoji=s.mode==='home' ? s.home_emoji || '🏡' : s.portable_emoji || '📱';
                var colour=/^#[0-9a-f]{6}$/i.test(s.marker_colour) ? s.marker_colour : '#38bdf8';
                var summary=el('button',null,'hub-summary'); summary.type='button';
                summary.setAttribute('aria-expanded',String(expanded));
                summary.onclick=function(){expanded=!expanded;render(s);};
                var avatar=el('span',emoji,'card-avatar'); avatar.style.borderColor=colour;summary.appendChild(avatar);
                var label=el('span');label.appendChild(el('strong',s.display_name || 'Home Hub'));
                label.appendChild(el('small',(s.mode==='home'?'Home':s.mode==='portable'?'Portable':'Off-Grid')+' · Hub '+s.gateway_guid16));
                label.appendChild(el('small','Locally connected · Wi-Fi '+(s.wifi_rssi_dbm == null ? '—':s.wifi_rssi_dbm+' dBm')));
                summary.appendChild(label);summary.appendChild(el('span',expanded?'▲':'▼'));card.appendChild(summary);
                var hasFix=typeof s.latitude==='number' && typeof s.longitude==='number';
                if(hasFix) {
                    var pos=[s.latitude,s.longitude];
                    // Emoji and colours are text/style, never unescaped HTML.
                    var iconNode=el('span',emoji,'hub-map-icon');iconNode.style.borderColor=colour;iconNode.style.setProperty('--hub-colour',colour);
                    var icon=L.divIcon({className:'hub-map-marker',html:iconNode,iconSize:[48,58],iconAnchor:[24,58]});
                    if(!marker)marker=L.marker(pos,{icon:icon}).addTo(map);
                    else marker.setLatLng(pos).setIcon(icon);
                    var popup=el('div');popup.appendChild(el('strong',s.display_name || 'Home Hub'));
                    popup.appendChild(el('p','Hub GNSS · fix '+s.fix_age_s+'s old'));
                    marker.bindPopup(popup);
                    onPosition(s.latitude,s.longitude);
                } else {
                    if(marker){map.removeLayer(marker);marker=null;}
                    onPosition(null,null);
                }
                if(!expanded)return;
                var detail=el('div',null,'card-detail');
                detail.appendChild(el('p',hasFix?'Hub GPS: '+s.latitude.toFixed(6)+', '+s.longitude.toFixed(6)+' · fix '+s.fix_age_s+'s old':'Waiting for the hub’s own GPS fix'));
                detail.appendChild(el('p','Uptime '+Math.floor(s.uptime_s/60)+' min · Free memory '+Math.round(s.free_heap/1024)+' KB'));
                detail.appendChild(el('p','Home beacon: '+(s.ble_advertising?'advertising':'off')+(s.mode!=='home'?' (disabled while roaming)':'')));
                var actions=el('div',null,'card-actions');
                var jump=button('↗ Jump To',function(){if(marker)map.setView(marker.getLatLng(),17);});jump.disabled=!hasFix;actions.appendChild(jump);
                var ble=button('Bluetooth '+(s.ble_enabled?'On':'Off'),function(){send({ble_enabled:!s.ble_enabled});});
                var bleIcon=document.createElementNS('http://www.w3.org/2000/svg','svg');
                bleIcon.setAttribute('width','14');bleIcon.setAttribute('height','16');bleIcon.setAttribute('viewBox','0 0 16 20');bleIcon.setAttribute('aria-hidden','true');
                var stroke=document.createElementNS('http://www.w3.org/2000/svg','path');
                stroke.setAttribute('d','M4 5l9 10-5 4V1l5 4L4 15');stroke.setAttribute('fill','none');stroke.setAttribute('stroke','currentColor');stroke.setAttribute('stroke-width','2');
                bleIcon.appendChild(stroke);ble.prepend(bleIcon);
                ble.setAttribute('aria-pressed',String(s.ble_enabled));ble.title='Home beacon only operates on primary Home Wi-Fi';actions.appendChild(ble);
                actions.appendChild(button('Edit appearance',function(){editing=!editing;render(s);}));detail.appendChild(actions);
                if(editing) {
                    var form=el('form',null,'hub-editor');
                    [['display_name','Name',s.display_name],['home_emoji','Home emoji',s.home_emoji],
                        ['portable_emoji','Portable / Off-Grid emoji',s.portable_emoji],['marker_colour','Marker colour',colour]].forEach(function(f){
                        var label=el('label',f[1]),input=el('input'); input.name=f[0];input.value=f[2] || '';
                        input.required=true;input.maxLength=f[0]==='display_name'?32:16;
                        input.type=f[0]==='marker_colour'?'color':'text';label.appendChild(input);form.appendChild(label);
                    });
                    var save=el('button','Save appearance');save.type='submit';form.appendChild(save);
                    form.onsubmit=function(e){e.preventDefault();send(Object.fromEntries(new FormData(form)));};detail.appendChild(form);
                }
                card.appendChild(detail);
            }
            function load() {
                if(busy || document.hidden)return Promise.resolve();
                busy=true;
                return fetch('/api/hub-presence',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('Hub disconnected');return r.json();})
                    .then(function(s){last=s;lastReceived=performance.now();card.classList.remove('stale');if(!editing)render(s);})
                    .catch(function(){card.classList.add('stale');})
                    .finally(function(){busy=false;});
            }
            load();setInterval(load,5000);
            document.addEventListener('visibilitychange',load);
            setInterval(function(){if(last && performance.now()-lastReceived>=15000)card.classList.add('stale');},1000);
        }
    };
})(globalThis);
