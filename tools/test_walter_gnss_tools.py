import json
import pathlib
import tempfile
import unittest
from walter_gnss_analyse import analyse, separation


class AnalysisTests(unittest.TestCase):
    def test_selection_is_logged_sample_not_nearest_reference(self):
        records=[]
        def add(trial, text, elapsed=1, kind='serial'):
            records.append(dict(trial=trial,text=text,elapsed_s=elapsed,kind=kind))
        add('1:gnss single','gnss single',0,'command')
        add('1:gnss single','[GNSS SAMPLE] n=1 elapsed_ms=1000 ttf_ms=900 status=0 uncertainty=20.0m lat=51.0 lon=-2.0 utc=100 entries=8 cn0_ge30=4')
        add('1:gnss single','[GNSS SETTLE] CLOSED elapsed_ms=1000 attempts=1 usable=1 consistent=1 selected=1 uncertainty=20.0m utc=100')
        add('1:gnss window 60','gnss window 60',2,'command')
        add('1:gnss window 60','[GNSS SAMPLE] n=1 elapsed_ms=1000 ttf_ms=900 status=0 uncertainty=20.0m lat=51.0 lon=-2.0 utc=101 entries=8 cn0_ge30=4',3)
        add('1:gnss window 60','[GNSS SAMPLE] n=2 elapsed_ms=1000 ttf_ms=900 status=0 uncertainty=1.0m lat=51.001 lon=-2.0 utc=102 entries=8 cn0_ge30=4',5)
        add('1:gnss window 60','[GNSS SETTLE] CLOSED elapsed_ms=3000 attempts=2 usable=2 consistent=2 selected=1 uncertainty=1.0m utc=102',5)
        add('2:gnss window 60','[GNSS SETTLE] CLOSED elapsed_ms=60000 attempts=1 usable=0 consistent=0 selected=0 uncertainty=0.0m utc=0',60)
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d)/'log.jsonl';p.write_text('\n'.join(map(json.dumps,records)))
            trials,samples=analyse([str(p)],(51,-2))
        self.assertEqual([r['mode'] for r in trials],['gnss single','gnss window 60','gnss window 60'])
        self.assertEqual(trials[0]['selected_error_m'],0)
        self.assertGreater(trials[1]['selected_error_m'],100)
        self.assertIsNone(trials[2]['selected_error_m'])
        self.assertEqual(trials[1]['first_event_s'],1)
        self.assertEqual(len(samples),3)

    def test_distance_units(self):
        self.assertEqual(separation(51,-2,(51,-2)),0)
        self.assertAlmostEqual(separation(0,0,(.001,0)),111.195,places=2)


if __name__=='__main__':
    unittest.main()
