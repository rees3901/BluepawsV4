"""Offline tests; never import pyserial or contact hardware/cloud."""
import unittest
from walter_soak import observe, ready_to_start


class SoakEvidenceTests(unittest.TestCase):
    def test_start_guards(self):
        good = '[WALTER] device=1010 hub=16 busy=0 running=0 profile=Normal home_stub=0 credentials=configured'
        self.assertTrue(ready_to_start([good]))
        for old, new in [('1010', '1001'), ('busy=0', 'busy=1'),
                         ('running=0', 'running=1'), ('configured', 'missing')]:
            self.assertFalse(ready_to_start([good.replace(old, new)]))
        self.assertFalse(ready_to_start([]))

    def test_evidence_is_bounded_and_not_inferred(self):
        state = {}
        observe(state, '[CYCLE] START n=1 profile=Normal', 't1')
        observe(state, '[LTE] ACCEPTED device=1010 seq=12 hash=abc', 't2')
        observe(state, '[CYCLE] SLEEP profile=Active seconds=60', 't3')
        self.assertEqual(state['cycle']['at'], 't1')
        self.assertEqual(state['sleep']['at'], 't3')
        self.assertEqual(len(state['events']), 3)
        self.assertNotIn('cloud_verified', state)
        for i in range(1100):
            observe(state, '[LTE CMD] Poll empty', str(i))
        self.assertEqual(len(state['events']), 1000)
        self.assertEqual(state['events'][0]['at'], '100')


if __name__ == '__main__':
    unittest.main()
