"""Regression checks for the public/off-grid filesystem boundary."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PublicAssetTests(unittest.TestCase):
    def test_fallback_serves_only_named_public_assets(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleNotFound() {', 1)[1].split(
            '// Register all HTTP routes', 1)[0]
        allowed = set(re.findall(r'path == "([^"]+)"', handler))
        self.assertEqual(allowed, {
            '/leaflet.js', '/leaflet.css', '/basemap.json',
            '/images/marker-icon.png', '/images/marker-icon-2x.png',
            '/images/marker-shadow.png',
        })
        self.assertIn('HTTP_GET && publicAsset && LittleFS.exists(path)', handler)
        for path in allowed:
            self.assertTrue((ROOT / 'hub/data' / path.lstrip('/')).is_file())

    def test_config_write_requires_provisioning_outside_offgrid(self):
        source = (ROOT / 'hub/src/main.cpp').read_text(encoding='utf-8')
        handler = source.split('static void handleApiConfig() {', 1)[1].split(
            '// Catch-all handler', 1)[0]
        guard = '!hubProvisioningMode || hubCommProfile == HUB_COMM_OFF_GRID'
        self.assertIn(guard, handler)
        self.assertLess(handler.index(guard), handler.index('LittleFS.open'))
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


if __name__ == '__main__':
    unittest.main()
