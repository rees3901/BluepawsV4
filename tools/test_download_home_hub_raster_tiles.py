import unittest

from tools import download_home_hub_raster_tiles as raster_tiles


class SatelliteCoverageTests(unittest.TestCase):
    def test_default_pack_uses_coarse_uk_and_detailed_gloucestershire(self):
        tiles = raster_tiles.enumerate_tiles("gloucestershire")
        zooms = {zoom for zoom, _, _ in tiles}

        self.assertEqual(zooms, set(range(5, 15)))
        self.assertEqual(len(tiles), 3787)

        uk_pass, local_pass = raster_tiles.SATELLITE_PASSES
        self.assertEqual((uk_pass.minimum_zoom, uk_pass.maximum_zoom), (5, 9))
        self.assertEqual((local_pass.minimum_zoom, local_pass.maximum_zoom), (10, 14))
        self.assertEqual(local_pass.bounds, (-2.72, 51.55, -1.62, 52.15))


if __name__ == "__main__":
    unittest.main()
