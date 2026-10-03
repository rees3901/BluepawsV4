import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import provision as p

class ProvisionTests(unittest.TestCase):
    def setUp(self):
        self.bundle = p.credential_bundle([p.generate_device(i) for i in range(3001, 3005)], p.generate_gateway('0030', 'Personal hub'))

    def test_distinct_material_and_identity(self):
        self.assertEqual(len({d['hmac_key_b64'] for d in self.bundle['devices']}), 4)
        self.assertEqual(len({d['bearer_token'] for d in self.bundle['devices']}), 4)
        for i in range(3001, 3005):
            text = p.collar_header(self.bundle, i)
            self.assertIn(f'PERSONAL_DEVICE_ID = {i}', text)
            self.assertIn('PERSONAL_HUB_ID = 0x0030', text)
            self.assertNotIn('bearer_token', text)

    def test_no_credential_rotation(self):
        sql = p.build_sql(self.bundle, '11111111-2222-3333-4444-555555555555')
        self.assertNotIn('on conflict', sql.lower())
        self.assertNotIn('do update', sql.lower())
        self.assertIn('raise exception', sql)
        self.assertIn('lock table', sql)
        self.assertTrue(sql.startswith('begin;'))
        self.assertTrue(sql.endswith('commit;\n'))

    def test_existing_files_are_not_overwritten(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'private.json'
            p.private_write(path, 'original', overwrite=False)
            with self.assertRaises(FileExistsError):
                p.private_write(path, 'replacement', overwrite=False)
            self.assertEqual(path.read_text(), 'original')

    def test_invalid_ids(self):
        for ident in [0, 3008, 65535, -1]:
            with self.assertRaises(ValueError): p.generate_device(ident)
        with self.assertRaises(ValueError): p.generate_gateway('0031', 'bad')
        with self.assertRaises(ValueError): p.build_sql(self.bundle, "invalid');drop table devices;")

if __name__ == '__main__':
    unittest.main()
