"""Regression checks for the public/off-grid filesystem boundary."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PublicAssetTests(unittest.TestCase):
    def test_captive_routes_redirect_to_absolute_ap_ip_only_for_ap_clients(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        portal = source.split('static bool isCaptivePortalClient() {', 1)[1].split(
            'static void handleFavicon()', 1)[0]
        self.assertIn('httpServer.client().localIP() == WiFi.softAPIP()', portal)
        self.assertIn('if (!isCaptivePortalClient())', portal)
        self.assertIn('"http://" + WiFi.softAPIP().toString() + "/welcome"', portal)
        self.assertIn('sendHeader("Location", target, true)', portal)
        self.assertIn('send(302,', portal)
        for path in ['/redirect', '/fwlink', '/connecttest.txt', '/ncsi.txt', '/generate_204', '/hotspot-detect.html']:
            self.assertIn(f'httpServer.on("{path}", HTTP_GET, handleCaptiveProbe)', source)
        catchall = source.split('static void handleNotFound() {', 1)[1].split('// Register all HTTP routes', 1)[0]
        self.assertIn('hasForeignPortalHost()', catchall)
        self.assertIn('!path.startsWith("/api/")', catchall)
        self.assertIn('httpServer.send(404,', catchall)

    def test_local_favicon_matches_cloud_brand_asset(self):
        self.assertEqual((ROOT / 'hub/platformio/data/favicon.svg').read_text(),
                         (ROOT / 'web/src/app/icon.svg').read_text())
        self.assertIn('href="/favicon.svg"', (ROOT / 'hub/platformio/data/index.html').read_text())

    def test_fallback_serves_only_named_public_assets(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleNotFound() {', 1)[1].split(
            '// Register all HTTP routes', 1)[0]
        allowed = set(re.findall(r'path == "([^"]+)"', handler))
        self.assertEqual(allowed, {
            '/leaflet.js', '/leaflet.css', '/basemap.json',
            '/images/marker-icon.png', '/images/marker-icon-2x.png',
            '/images/marker-shadow.png',
            '/brand-favicon.ico', '/brand-mascot.avif',
            '/location-fit-markers.png', '/map-location.png',
            '/welcome.js', '/feedback.js', '/hub-presence.js', '/hub-presence.css',
        })
        self.assertIn('HTTP_GET && publicAsset && LittleFS.exists(path)', handler)
        for path in allowed:
            self.assertTrue((ROOT / 'hub/platformio/data' / path.lstrip('/')).is_file())
        self.assertIn('path.endsWith(".avif")', handler)

    def test_offgrid_dashboard_keeps_practical_web_ux_parity(self):
        html = (ROOT / 'hub/platformio/data/index.html').read_text(encoding='utf-8')
        js = (ROOT / 'hub/platformio/data/app.js').read_text(encoding='utf-8')
        css = (ROOT / 'hub/platformio/data/style.css').read_text(encoding='utf-8')
        for asset in ['/brand-favicon.ico', '/brand-mascot.avif', '/location-fit-markers.png']:
            self.assertIn(asset, html + js + css)
        init_map = js.split('function initMap() {', 1)[1].split('// Theme Toggle', 1)[0]
        self.assertIn("map.on('contextmenu'", init_map)
        self.assertEqual(js.count("map.on('contextmenu'"), 1)
        self.assertIn("showToast('Coordinates copied to clipboard')", js)
        self.assertIn('Temporary meeting point', js)
        self.assertIn('Drop temporary pin', js)
        self.assertIn('Measure from here', js)
        self.assertIn("isExpanded ? '<button type=\"button\" class=\"card-avatar-edit\"", js)
        self.assertIn('M3 6h12M3 12h12', js)
        self.assertNotIn('M3 5h12M3 9h12M3 13h12', js)
        self.assertIn('bp_offline_pinned_device', js)
        self.assertIn('bp_offline_device_order', js)
        self.assertIn('global-trails-btn', js)
        self.assertIn('.card-avatar-wrap:hover .card-avatar-edit', css)
        self.assertIn('@media (max-width: 768px)', css)

    def test_offgrid_hub_is_default_pin_but_explicit_unpin_persists(self):
        js = (ROOT / 'hub/platformio/data/app.js').read_text(encoding='utf-8')
        self.assertIn("data.entity === 'hub' && !hasPinnedDevicePreference", js)
        self.assertIn("localStorage.setItem('bp_offline_pinned_device', 'none')", js)
        self.assertIn("savedPin === null || savedPin === 'none'", js)

    def test_p4_hub_starter_position_creates_a_map_marker(self):
        defaults = (ROOT / 'hub/esp-idf-p4/main/home_hub_defaults.h').read_text(encoding='utf-8')
        server = (ROOT / 'hub/esp-idf-p4/main/home_hub_web.cpp').read_text(encoding='utf-8')
        adapter = (ROOT / 'hub/platformio/data/hub-presence.js').read_text(encoding='utf-8')
        self.assertIn('51.905857, -2.239923', defaults)
        self.assertIn('defaults::kStarterLocation.latitude', server)
        self.assertIn('defaults::kStarterLocation.longitude', server)
        self.assertIn('"position_source", "starter"', server)
        self.assertIn("s.position_source === 'starter'", adapter)

    def test_hub_marker_survives_optional_adapter_failure_and_overlap(self):
        js = (ROOT / 'hub/platformio/data/app.js').read_text(encoding='utf-8')
        css = (ROOT / 'hub/platformio/data/style.css').read_text(encoding='utf-8')
        self.assertIn("function startHubPresence()", js)
        self.assertIn("fetch('/api/hub-presence', {cache: 'no-store'})", js)
        self.assertIn("zIndexOffset: isHubMarker ? 600 : 0", js)
        self.assertIn("data.entity === 'hub' ? ' marker-hub' : ''", js)
        self.assertIn('.bp-marker.marker-hub', css)

    def test_p4_offgrid_http_server_stops_eventsource_reconnect_churn(self):
        server = (ROOT / 'hub/esp-idf-p4/main/home_hub_web.cpp').read_text(encoding='utf-8')
        defaults = (ROOT / 'hub/esp-idf-p4/sdkconfig.defaults').read_text(encoding='utf-8')
        self.assertIn('config.max_open_sockets = 4', server)
        self.assertIn('config.backlog_conn = 4', server)
        self.assertIn('config.lru_purge_enable = true', server)
        self.assertIn('"204 No Content"', server)
        self.assertNotIn('CONFIG_LWIP_MAX_SOCKETS=20', defaults)

    def test_p4_offgrid_pmtiles_is_default_with_raster_fallbacks(self):
        server = (ROOT / 'hub/esp-idf-p4/main/home_hub_web.cpp').read_text(encoding='utf-8')
        html = (ROOT / 'hub/platformio/data/index.html').read_text(encoding='utf-8')
        js = (ROOT / 'hub/platformio/data/app.js').read_text(encoding='utf-8')
        bootstrap = (ROOT / 'hub/platformio/data/map-bootstrap.mjs').read_text(encoding='utf-8')
        for asset in [
            'maplibre-gl.mjs', 'maplibre-gl-shared.mjs', 'maplibre-gl-worker.mjs',
            'maplibre-gl.css', 'leaflet-maplibre-gl.js', 'pmtiles.js', 'map-style.json',
            'noto-sans-regular-0-255.pbf', 'noto-sans-regular-256-511.pbf',
        ]:
            self.assertTrue((ROOT / 'hub/platformio/data' / asset).is_file(), asset)
        self.assertIn('type="module" src="/map-bootstrap.mjs', html)
        self.assertIn("maplibregl.addProtocol('pmtiles'", bootstrap)
        self.assertIn("format === 'pmtiles'", js)
        self.assertIn("initialName = vectorLayer ?", js)
        self.assertIn('OFFLINE_VECTOR_DISPLAY_MAX_ZOOM = 22', js)
        self.assertIn('maxZoom: OFFLINE_VECTOR_DISPLAY_MAX_ZOOM', js)
        self.assertIn('maxzoom: nativeMaxZoom', js)
        self.assertIn('"format", "pmtiles"', server)
        self.assertIn('"/maps/uk.pmtiles"', server)

    def test_p4_pmtiles_ranges_use_unsigned_fatfs_offsets(self):
        server = (ROOT / 'hub/esp-idf-p4/main/home_hub_web.cpp').read_text(encoding='utf-8')
        handler = server.split('esp_err_t serve_pmtiles_range', 1)[1].split(
            'esp_err_t serve_map_font', 1
        )[0]
        self.assertIn('f_open(&file, kPmtilesFatFsPath, FA_READ)', handler)
        self.assertIn('static_cast<FSIZE_t>(start)', handler)
        self.assertIn('206 Partial Content', handler)
        self.assertIn('Accept-Ranges', handler)
        self.assertIn('Content-Range', handler)
        self.assertIn('valid_pmtiles_archive(&file)', handler)
        self.assertIn('section_length > size - section_start', server)

    def test_mdns_hostname_is_not_redirected_as_foreign(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        host = source.split('static bool hasForeignPortalHost() {', 1)[1].split(
            'static void handleCaptiveProbe()', 1)[0]
        self.assertIn('host.toLowerCase()', host)
        self.assertIn('host.endsWith(":80")', host)
        self.assertIn('host.endsWith(".")', host)
        self.assertIn('host != String(MDNS_HOSTNAME) + ".local"', host)

    def test_welcome_page_is_separate_and_offline_ready(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        html = (ROOT / 'hub/platformio/data/welcome.html').read_text(encoding='utf-8')
        self.assertIn('httpServer.on("/welcome", HTTP_GET, handleWelcome)', source)
        self.assertIn('httpServer.on("/",             HTTP_GET,  handleRoot)', source)
        self.assertIn('href="http://192.168.4.1/"', html)
        self.assertIn('href="http://bluepaws.local/"', html)
        self.assertIn('Use this network as is', html)
        self.assertIn('Open tracking dashboard', html)
        self.assertIn('<noscript>', html)
        self.assertNotRegex(html, r'(?:src|href)="https://')
        self.assertNotIn('window.open', html)
        self.assertNotIn('http-equiv="refresh"', html)

    def test_welcome_stats_are_small_read_only_and_ap_scoped(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiWelcome() {', 1)[1].split(
            'static void handleFavicon()', 1)[0]
        self.assertIn('if (!isCaptivePortalClient())', handler)
        self.assertIn('sendHeader("Cache-Control", "no-store")', handler)
        self.assertIn('xSemaphoreTake(deviceMutex, pdMS_TO_TICKS(50))', handler)
        self.assertIn('httpServer.send(503', handler)
        self.assertIn('age < 600', handler)
        self.assertIn('doc["last_report_age_s"] = nullptr', handler)
        self.assertEqual(set(re.findall(r'doc\["([^\"]+)"\]', handler)), {
            'hub_id', 'recent_collars', 'known_collars', 'last_report_age_s', 'time_synced',
        })

    def test_config_write_requires_provisioning_outside_offgrid(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiConfig() {', 1)[1].split(
            '// Catch-all handler', 1)[0]
        guard = '!hubProvisioningMode || hubCommProfile == HUB_COMM_OFF_GRID'
        self.assertIn(guard, handler)
        self.assertLess(handler.index(guard), handler.index('saveHubConfigToFlash'))
        self.assertIn('httpServer.send(403', handler)

    def test_settings_start_hidden_and_hidden_class_actually_hides(self):
        html = (ROOT / 'hub/platformio/data/index.html').read_text(encoding='utf-8')
        css = (ROOT / 'hub/platformio/data/style.css').read_text(encoding='utf-8')
        self.assertIn('id="provisioningFields" class="hidden"', html)
        self.assertIn('id="btnSaveConfig" class="btn-primary hidden"', html)
        self.assertRegex(css, r'\.hidden\s*\{\s*display:\s*none\s*!important;')

    def test_mode_switch_reads_form_fields_not_only_raw_body(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiHubMode() {', 1)[1].split(
            'static bool requireCommandAccess()', 1)[0]
        self.assertIn('getPostField(body, "mode")', handler)
        self.assertNotIn('body.indexOf("mode=', handler)
        self.assertIn('getPostField(body, "confirm") != "true"', handler)
        self.assertIn('requestHubMode(', handler)
        self.assertNotIn('WiFi.', handler)
        self.assertIn('httpServer.send(202', handler)

    def test_network_task_owns_reconnect_and_dns(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        web = source.split('static void webTask(void *param) {', 1)[1].split(
            'static void consoleTask(void *param) {', 1)[0]
        self.assertNotIn('WiFi.begin', web)
        self.assertNotIn('captiveDns.processNextRequest', web)
        self.assertLess(web.index('while (!networkStackReady)'), web.index('initWebServer()'))
        network = source.split('static void networkTask(void *param) {', 1)[1].split(
            'static void initBLE()', 1)[0]
        self.assertIn('WiFi.setAutoReconnect(false)', network)
        self.assertIn('WiFi.softAPgetStationNum() > 0', network)
        self.assertIn('!busy && now - lastScan >= 60000', network)
        self.assertIn('pending.confirmed', network)
        self.assertIn('captiveDns.processNextRequest()', network)
        self.assertNotIn('syncHubClock(', network)
        self.assertIn('false, MAX_SSE_CLIENTS', source)
        self.assertNotIn('WiFi.setSleep(false)', source)
        self.assertIn('WiFi.setSleep(true)', network)

    def test_secondary_credentials_persist_without_public_exposure(self):
        source = (ROOT / 'hub/platformio/src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('key == "secondary_ssid"', source)
        self.assertIn('key == "secondary_pass"', source)
        self.assertIn('f.printf("secondary_pass=', source)
        status = source.split('static void handleApiStatus() {', 1)[1].split('static String getPostField', 1)[0]
        self.assertNotIn('secondaryPass', status)
        self.assertNotIn('staPass', status)


if __name__ == '__main__':
    unittest.main()
