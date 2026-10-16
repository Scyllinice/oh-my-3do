#!/usr/bin/env python3
"""Catalog format and cache migration tests use synthetic metadata only."""
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('catalog',ROOT/'tools/library-catalog.py')
catalog=importlib.util.module_from_spec(spec);spec.loader.exec_module(catalog)
XML='''<?xml version="1.0"?><datafile><header><name>Example</name></header>
<game name="Example (USA) (Demo)"><description>Example (USA) (Demo)</description>
<category>Demos</category><serial>TEST-001</serial>
<rom name="Example.cue" size="100" sha1="badcue"/>
<rom name="Example (Track 01).bin" size="2352" sha1="data"/>
<rom name="Example (Track 02).bin" size="4704" sha1="audio"/>
</game></datafile>'''


def zipped(name,content):
    out=io.BytesIO()
    with zipfile.ZipFile(out,'w') as z:z.writestr(name,content)
    return out.getvalue()


class CatalogTest(unittest.TestCase):
    def test_xml_categories_serials_and_all_data_tracks_survive(self):
        row=catalog.dat_games(XML)[0]
        self.assertEqual(row['category'],'Demos')
        self.assertEqual(row['serial'],'TEST-001')
        self.assertEqual([r['sha1'] for r in row['roms']],['data','audio'])

    def test_plain_xml_clrmamepro_and_zip_are_supported(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'input.dat'
            for content in (XML.encode(),zipped('../../reference.dat',XML)):
                path.write_bytes(content)
                self.assertEqual(catalog.read_dat(path)[0]['category'],'Demos')
                self.assertFalse((Path(temp).parent/'reference.dat').exists())
            path.write_text('game ( name "Example (USA)" category "Educational" rom ( name "Example.cue" size 1 sha1 cue ) rom ( name "Example.bin" size 2352 sha1 data ) )')
            row=catalog.read_dat(path)[0]
            self.assertEqual(row['category'],'Educational')
            self.assertEqual(len(row['roms']),1)

    def test_bad_or_oversized_zip_and_html_fail_clearly(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/'input.zip'
            bads=[zipped('readme.txt','No DAT'),zipped('large.dat',' '* (8*1024*1024+1)),b'<html>Unavailable</html>',b'<datafile><game name="No data"><rom name="only.cue"/></game></datafile>']
            for bad in bads:
                path.write_bytes(bad)
                with self.assertRaises(ValueError):catalog.read_dat(path)

    def metadata(self,folder):
        content='<LaunchBox><Game><Name>Example</Name><DatabaseID>1</DatabaseID><Platform>3DO</Platform><Genres>Adventure</Genres></Game></LaunchBox>'
        (folder/'launchbox.zip').write_bytes(zipped('Metadata.xml',content))

    def test_online_upgrade_downloads_full_catalog_and_reuses_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);self.metadata(folder)
            (folder/'catalog.json').write_text(json.dumps({'schema':2,'games':[]}))
            (folder/'redump.dat').write_text('old limited DAT')
            def download(url,target,maximum,offline=False):
                self.assertEqual(url,catalog.REDUMP)
                self.assertEqual(Path(target).name,'redump-full.zip')
                Path(target).write_bytes(zipped('3do.dat',XML))
            with mock.patch.object(catalog,'download',side_effect=download) as request:
                result=catalog.load_catalog(folder)
                self.assertEqual(request.call_count,1)
            self.assertEqual(result['schema'],3)
            self.assertTrue(result['complete_catalog'])
            self.assertEqual(result['games'][0]['category'],'Demos')
            with mock.patch.object(catalog,'download',side_effect=AssertionError('cached')):
                self.assertEqual(catalog.load_catalog(folder),result)
            self.assertEqual((folder/'redump.dat').read_text(),'old limited DAT')

    def test_offline_legacy_cache_warns_and_does_not_download(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            old={'schema':2,'sources':[],'games':[{'name':'Example'}]}
            (folder/'catalog.json').write_text(json.dumps(old))
            output=io.StringIO()
            with mock.patch.object(catalog,'download',side_effect=AssertionError('offline')),mock.patch('sys.stdout',output):
                self.assertEqual(catalog.load_catalog(folder,offline=True),old)
            self.assertIn('may be missing',output.getvalue())

    def test_local_xml_is_not_removed_by_refresh_or_treated_as_complete_default(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp);self.metadata(folder)
            path=folder/'local.dat';path.write_text(XML)
            result=catalog.load_catalog(folder,offline=True,refresh=True,dat=path,launchbox=folder/'launchbox.zip')
            self.assertEqual(path.read_text(),XML)
            self.assertFalse(result['complete_catalog'])
            with mock.patch.object(catalog,'download',side_effect=AssertionError('missing local is not a download')):
                with self.assertRaisesRegex(ValueError,'Local DAT'):catalog.load_catalog(folder,dat=folder/'missing.dat')


if __name__=='__main__':unittest.main()
