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

    def test_discs_without_catalog_metadata_still_recognize_game_folders(self):
        names = ['Adult/Japan/Bodyconscious/Bodyconscious.iso', 'Adult/Japan/Super Model/Super Model.iso']
        records = [dict(path=names[0], metadata={})]
        data = m.metadata_index(records, names)
        self.assertEqual(m.metadata_paths(data), names)
        # An absent catalog record and an empty one have the same blank fields.
        self.assertEqual(data, m.metadata_index([], names))
        with self.assertRaises(ValueError): m.metadata_index(records * 2, names)

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
            for record in self.records()[:2]:
                file=root/record['path']; file.parent.mkdir(parents=True); file.write_bytes(b'\1'*2048)
            (support/'library.json').write_text(json.dumps(dict(schema=1,games=self.records())))
            m.index(root)
            self.assertEqual(m.metadata_paths((support/'metadata.3di').read_bytes()), ['Games/USA/Alpha/Disc 1.iso','Games/USA/Beta/Beta.iso'])

def opera_image(*programs):
    """A minimal 3DO disc: header, one root directory block, then one block per file."""
    blocks = [bytearray(2048) for _ in range(2 + len(programs))]
    blocks[0][:7] = b'\x01ZZZZZ\x01'
    struct.pack_into('>II', blocks[0], 88, 1, 2048); struct.pack_into('>I', blocks[0], 100, 1)
    pos = 20
    for i, data in enumerate(programs):
        flags = 2 | (0xc0000000 if i == len(programs) - 1 else 0)
        struct.pack_into('>II4sIIIII32sII', blocks[1], pos, flags, i, b'    ', 2048, len(data), 1, 0, 0,
                         ('File%d' % i).encode(), 0, 2 + i)
        blocks[2 + i][:len(data)] = data
        pos += 72
    struct.pack_into('>iiIII', blocks[1], 0, -1, -1, 0, pos, 20)
    return b''.join(blocks)

class SaveNameTests(unittest.TestCase):
    def test_lowercase_paths_and_brackets(self):
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)/'Icebreaker.iso'
            game.write_bytes(opera_image(b'x'*64+b"/nvram/icebreaker[levelchart]\0/NvRaM/Maps O'Death V1.0\0"))
            self.assertEqual(m.disc_save_names(game), ["=Maps O'Death V1.0", '=icebreaker[levelchart]'])

    def test_disc_names_patterns_titles_and_cache(self):
        program = b'x' * 64 + b'/NVRAM/EA_RRX\0/NVRAM/SLOT%02d\0/NVRAM/%s\0/NVRAM/X\0/NVRAM/bad|name\0'
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); support = root/'.ohmy3do'; support.mkdir()
            game = root/'Games/USA/Road Rash (USA)/Road Rash (USA).iso'; game.parent.mkdir(parents=True)
            game.write_bytes(opera_image(b'no saves here' * 8, program))
            self.assertEqual(m.disc_save_names(game), ['*SLOT', '=EA_RRX'])
            (root/'Other.iso').write_bytes(opera_image(b'z' * 64 + b'/NVRAM/EA_RRX\0'))
            (root/'Junk.iso').write_bytes(b'not a disc' * 300)
            (support/'library.json').write_text(json.dumps(dict(schema=1, games=[dict(path='Games/USA/Road Rash (USA)/Road Rash (USA).iso', title='Road Rash (USA)', metadata={})])))
            m.index(root)
            text = (support/'save-names.txt').read_text()
            self.assertIn('=EA_RRX\tRoad Rash / Other\n', text)
            self.assertIn('*SLOT\tRoad Rash\n', text)
            cache = json.loads((support/'save-names-cache.json').read_text())
            self.assertEqual(len(cache['discs']), 3)
            # Unchanged discs are not read again.
            calls = []
            original = m.disc_save_names
            m.disc_save_names = lambda path: calls.append(path) or original(path)
            try:
                m.index(root)
            finally:
                m.disc_save_names = original
            self.assertEqual(calls, [])

if __name__=='__main__': unittest.main()
