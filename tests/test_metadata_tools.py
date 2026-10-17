import importlib.util
import binascii
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('index_media', ROOT/'tools/index-media.py')
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

class MetadataTests(unittest.TestCase):
    def records(self):
        return [dict(path='Games/USA/Alpha/Disc 1.iso', metadata=dict(genre='Action',publisher='Café Studio',release_date='1995-02-03T00:00:00',year=1995)),
                dict(path='Games/USA/Beta/Beta.iso', metadata=dict(publisher='Test',year=1994)),
                dict(path='missing.iso', metadata=dict(genre='Ignored'))]

    def fixture(self):
        return m.metadata_index(self.records(), ['Games/USA/Beta/Beta.iso', 'Games/USA/Alpha/Disc 1.iso'])

    def test_roundtrip_dates_and_ascii(self):
        data = self.fixture()
        self.assertEqual(m.metadata_paths(data), ['Games/USA/Alpha/Disc 1.iso','Games/USA/Beta/Beta.iso'])
        self.assertIn(b'Cafe Studio\0',data)
        self.assertIn(b'1994\0', data)
        self.assertEqual(m.metadata_date(dict(release_date='1995-02-30',year=1995)), '1995')
        self.assertEqual(m.metadata_date(dict(release_date='unknown')), '')
        self.assertEqual(m.metadata_date(dict(release_date='2000-02-29')), '2000-02-29')

    def test_empty_corrupt_and_duplicates(self):
        self.assertEqual(m.metadata_paths(m.metadata_index([], [])), [])
        data = self.fixture()
        for i in range(len(data)):
            with self.assertRaises(ValueError): m.metadata_paths(data[:i])
        duplicate = self.records()[:1]*2
        with self.assertRaises(ValueError): m.metadata_index(duplicate, [duplicate[0]['path']])

    def test_console_decoder_and_io_faults(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'metadata.3di'; path.write_bytes(self.fixture())
            subprocess.run([str(ROOT/'build/test_usb_media'),'metadata',str(path)],check=True)
            for payload in (b'\0\1\0'*4, b'\0\3..\0'+b'\0\1\0'*3):
                path.write_bytes(b'OM3DMET1'+struct.pack('>IIII',1,len(payload),binascii.crc32(payload),0)+payload+b'END3DM01')
                subprocess.run([str(ROOT/'build/test_usb_media'),'badmetadata',str(path)],check=True)

    def test_index_export(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); support=root/'.ohmy3do'; support.mkdir()
            (support/'ohmy3do.ini').write_text('layout marker')
            for record in self.records()[:2]:
                file=root/record['path']; file.parent.mkdir(parents=True); file.write_bytes(b'\1'*2048)
            (support/'library.json').write_text(json.dumps(dict(schema=1,games=self.records())))
            m.index(root)
            self.assertEqual(m.metadata_paths((support/'metadata.3di').read_bytes()), ['Games/USA/Alpha/Disc 1.iso','Games/USA/Beta/Beta.iso'])

if __name__=='__main__': unittest.main()
