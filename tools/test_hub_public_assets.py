"""Regression checks for the public/off-grid filesystem boundary."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PublicAssetTests(unittest.TestCase):
    def test_captive_routes_redirect_to_absolute_ap_ip_only_for_ap_clients(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
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
        self.assertEqual((ROOT / 'hub/data/favicon.svg').read_text(),
                         (ROOT / 'web/src/app/icon.svg').read_text())
        self.assertIn('href="/favicon.svg"', (ROOT / 'hub/data/index.html').read_text())

    def test_fallback_serves_only_named_public_assets(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleNotFound() {', 1)[1].split(
            '// Register all HTTP routes', 1)[0]
        allowed = set(re.findall(r'path == "([^"]+)"', handler))
        self.assertEqual(allowed, {
            '/leaflet.js', '/leaflet.css', '/basemap.json',
            '/images/marker-icon.png', '/images/marker-icon-2x.png',
            '/images/marker-shadow.png',
            '/welcome.js', '/feedback.js', '/hub-presence.js', '/hub-presence.css',
        })
        self.assertIn('HTTP_GET && publicAsset && LittleFS.exists(path)', handler)
        for path in allowed:
            self.assertTrue((ROOT / 'hub/data' / path.lstrip('/')).is_file())

    def test_mdns_hostname_is_not_redirected_as_foreign(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        host = source.split('static bool hasForeignPortalHost() {', 1)[1].split(
            'static void handleCaptiveProbe()', 1)[0]
        self.assertIn('host.toLowerCase()', host)
        self.assertIn('host.endsWith(":80")', host)
        self.assertIn('host.endsWith(".")', host)
        self.assertIn('host != String(MDNS_HOSTNAME) + ".local"', host)

    def test_welcome_page_is_separate_and_offline_ready(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        html = (ROOT / 'hub/data/welcome.html').read_text(encoding='utf-8')
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
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
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
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiConfig() {', 1)[1].split(
            '// Catch-all handler', 1)[0]
        guard = '!hubProvisioningMode || hubCommProfile == HUB_COMM_OFF_GRID'
        self.assertIn(guard, handler)
        self.assertLess(handler.index(guard), handler.index('saveHubConfigToFlash'))
        self.assertIn('httpServer.send(403', handler)

    def test_settings_start_hidden_and_hidden_class_actually_hides(self):
        html = (ROOT / 'hub/data/index.html').read_text(encoding='utf-8')
        css = (ROOT / 'hub/data/style.css').read_text(encoding='utf-8')
        self.assertIn('id="provisioningFields" class="hidden"', html)
        self.assertIn('id="btnSaveConfig" class="btn-primary hidden"', html)
        self.assertRegex(css, r'\.hidden\s*\{\s*display:\s*none\s*!important;')

    def test_mode_switch_reads_form_fields_not_only_raw_body(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiHubMode() {', 1)[1].split(
            'static bool requireCommandAccess()', 1)[0]
        self.assertIn('getPostField(body, "mode")', handler)
        self.assertNotIn('body.indexOf("mode=', handler)
        self.assertIn('getPostField(body, "confirm") != "true"', handler)
        self.assertIn('requestHubMode(', handler)
        self.assertNotIn('WiFi.', handler)
        self.assertIn('httpServer.send(202', handler)

    def test_network_task_owns_reconnect_and_dns(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
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
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('key == "secondary_ssid"', source)
        self.assertIn('key == "secondary_pass"', source)
        self.assertIn('f.printf("secondary_pass=', source)
        status = source.split('static void handleApiStatus() {', 1)[1].split('static String getPostField', 1)[0]
        self.assertNotIn('secondaryPass', status)
        self.assertNotIn('staPass', status)


if __name__ == '__main__':
    unittest.main()
