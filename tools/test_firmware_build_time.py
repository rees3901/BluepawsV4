"""Run with: py -3.11 -m unittest discover -s tools -p test_firmware_build_time.py"""
import calendar
from pathlib import Path
import runpy
import unittest
from unittest.mock import Mock, patch


class BuildTimeTests(unittest.TestCase):
    def test_utc_epoch_in_every_month_without_date_or_timezone_parsing(self):
        script = Path(__file__).with_name("firmware_build_time.py")
        for month in range(1, 13):
            with self.subTest(month=month):
                epoch = calendar.timegm((2026, month, 27, 12, 34, 56))
                env = Mock()
                with patch("time.time", return_value=epoch):
                    runpy.run_path(str(script), init_globals={"env": env, "Import": Mock()})
                env.Append.assert_called_once_with(CPPDEFINES=[("BLUEPAWS_BUILD_UNIX_TIME", epoch)])


if __name__ == "__main__":
    unittest.main()
