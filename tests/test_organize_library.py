#!/usr/bin/env python3
"""Organizer integration tests use tiny synthetic discs, never commercial images."""
import base64
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
import os
import re
import sys
from pathlib import Path
import sqlite3
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('organizer', ROOT/'tools/organize-library.py')
organizer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(organizer)


def game(name, payloads, region='USA'):
    return dict(name=name, region=region,
        roms=[dict(name=f'track{i}.bin', size=str(len(data)), sha1=hashlib.sha1(data).hexdigest()) for i,data in enumerate(payloads)],
        metadata=dict(genre='Adventure', publisher='Example Publisher', year=1995, source='https://example.test/metadata'))


def snapshot(root):
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob('*') if p.is_file() and '.ohmy3do/transactions/' not in p.relative_to(root).as_posix()}


class OrganizerTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)/'USB'
        self.root.mkdir()
        self.cache = Path(self.temp.name)/'cache'
        self.cache.mkdir()
        self.catalog = dict(schema=1,sources=[dict(url='https://example.test/catalog')],games=[])
        self.stdout = contextlib.redirect_stdout(io.StringIO())
        self.stdout.__enter__()
        self.addCleanup(self.stdout.__exit__,None,None,None)

    def put(self, path, data):
        target = self.root/path
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(data)
        return target

    def scan(self, quick=False, overrides=None):
        return organizer.build_plan(self.root,self.catalog,quick,overrides)

    def apply(self, plan, names=False):
        return organizer.apply_plan(self.root,plan,self.cache,covers=False,accept_names=names)

    def preview_fixture(self, title='Example (USA)', discs=1):
        clips=Path(self.temp.name)/'clips';clips.mkdir()
        for disc in range(1,discs+1):
            name=title+(f' (Disc {disc})' if discs>1 else '')
            data=f'synthetic disc {disc}'.encode()
            self.put('Unsorted/'+name+'.iso',data)
            self.catalog['games'].append(game(name,[data]))
        return clips

    def test_disc_of_total_and_compound_regions_identify_without_fuzzy_matching(self):
        self.put('Example (1995)(Publisher)(EU-US)(Disc 2 of 3)[!].iso',b'disc')
        self.catalog['games']=[game('Example (USA, Europe) (Disc 1)',[b'1']),game('Example (USA, Europe) (Disc 2)',[b'2'])]
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['canonical'],'Example (USA, Europe) (Disc 2)')
        self.assertEqual(row['status'],'review')
        self.assertEqual(organizer.region_of('Example (GB)'),{'Europe'})
        self.assertEqual(organizer.disc_of('Example (Disk 02 of 03)'),2)

    def converted_game(self, name='Example (USA)', data=None):
        data = data if data is not None else bytes(range(256))*16
        raw = organizer.sector_tools.encode_python(data)
        self.catalog['games'].append(game(name,[raw]))
        return data,raw

    def test_converted_iso_identifies_contents_over_incorrect_name_and_region(self):
        data,raw = self.converted_game()
        source='Japan/Entirely Wrong - Disc 9.iso'
        path=self.put(source,data);before=snapshot(self.root)
        plan=self.scan();row=plan['entries'][0]
        self.assertEqual((row['confidence'],row['status']),('converted-checksum','ready'))
        self.assertEqual(row['canonical'],'Example (USA)')
        self.assertEqual(row['moves'][0]['fingerprint']['sha1'],hashlib.sha1(data).hexdigest())
        self.assertEqual(row['moves'][0]['fingerprint']['mode1_raw']['sha1'],hashlib.sha1(raw).hexdigest())
        self.assertEqual(before,snapshot(self.root))
        tx=self.apply(plan)
        self.assertEqual((self.root/row['target']).read_bytes(),data)
        database=json.loads((self.root/'.ohmy3do/library.json').read_text())
        self.assertEqual(database['games'][0]['identification'],'converted-checksum')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_converted_iso_distinguishes_revisions_and_unnamed_variants(self):
        data,_=self.converted_game('Example (USA) (Rev 1)')
        self.converted_game('Example (USA)',b'\x01'*4096)
        self.put('Example (USA).iso',data)
        row=self.scan()['entries'][0]
        self.assertEqual(row['canonical'],'Example (USA) (Rev 1)')
        self.catalog['games'][0]['name']='Example (USA)'
        row=self.scan()['entries'][0]
        self.assertEqual(row['confidence'],'converted-checksum')
        self.assertIn('[Variant ',row['target'])

    def test_converted_iso_ambiguous_releases_do_not_fall_back_to_filename(self):
        data,_=self.converted_game('Example (USA)')
        self.converted_game('Example (Japan)',data)
        self.put('Example (USA).iso',data)
        row=self.scan()['entries'][0]
        self.assertEqual((row['status'],row['confidence']),('unmatched','ambiguous'))
        self.assertEqual(set(row['suggestions']),{'Example (USA)','Example (Japan)'})
        self.assertFalse(row['moves'])

    def test_converted_iso_checksum_mismatch_remains_unverified(self):
        data,_=self.converted_game()
        changed=bytearray(data);changed[99]^=1
        self.put('Example (USA).iso',changed)
        row=self.scan()['entries'][0]
        self.assertEqual((row['status'],row['confidence']),('review','filename'))
        self.assertIn('not verified',row['reason'])

    def test_converted_iso_only_named_first_track_match_requires_review(self):
        data,raw=self.converted_game()
        self.catalog['games']=[game('Example (USA)',[raw,b'audio'])]
        self.catalog['games'][0]['roms'][0]['name']='Example (USA) (Track 01).bin'
        self.catalog['games'][0]['roms'][1]['name']='Example (USA) (Track 02).bin'
        self.put('unknown.iso',data)
        plan=self.scan();row=plan['entries'][0]
        self.assertEqual((row['status'],row['confidence']),('review','data-track'))
        self.assertIn('absent and unverified',row['reason'])
        tx=self.apply(plan)
        self.assertTrue((self.root/'unknown.iso').exists())
        tx.undo()
        tx=self.apply(plan,names=True)
        self.assertTrue((self.root/row['target']).exists())
        self.assertEqual(len(row['moves']),1)
        tx.undo()
        self.catalog['games'][0]['roms'][0]['name']='Example (Track 02).bin'
        self.assertEqual(self.scan()['entries'][0]['status'],'unmatched')

    def test_reconstructed_shared_data_tracks_never_guess_release(self):
        data,raw=self.converted_game()
        self.catalog['games']=[game('Example (USA)',[raw,b'audio 1']),
                               game('Example (USA) (Rev 1)',[raw,b'audio 2'])]
        for g in self.catalog['games']:
            g['roms'][0]['name']='Example (Track 1).bin'
            g['roms'][1]['name']='Example (Track 2).bin'
        self.put('Example (USA).iso',data)
        row=self.scan()['entries'][0]
        self.assertEqual(row['confidence'],'ambiguous')
        self.assertFalse(row['moves'])

    def test_quick_and_impossible_sizes_never_reconstruct(self):
        data,_=self.converted_game()
        self.put('Example (USA).iso',data)
        with mock.patch.object(organizer.sector_tools,'digests',side_effect=AssertionError('must skip')):
            self.assertEqual(self.scan(quick=True)['entries'][0]['confidence'],'filename')
            self.put('Example (USA).iso',data+b'\0'*2048)
            self.assertEqual(self.scan()['entries'][0]['confidence'],'filename')

    def test_raw_checksum_and_explicit_override_precedence_remain(self):
        data,raw=self.converted_game()
        self.put('raw.iso',raw)
        self.assertEqual(self.scan()['entries'][0]['confidence'],'checksum')
        (self.root/'raw.iso').unlink()
        self.put('cooked.iso',data)
        self.converted_game('Manual Choice (USA)',b'\2'*4096)
        row=self.scan(overrides={'cooked.iso':'Manual Choice (USA)'})['entries'][0]
        self.assertEqual((row['confidence'],row['canonical']),('manual','Manual Choice (USA)'))

    def test_four_converted_discs_from_separate_folders_consolidate_and_undo(self):
        for disc in range(1,5):
            data,_=self.converted_game(f'Example (USA) (Disc {disc})',bytes([disc])*4096)
            self.put(f'Unsorted/Separate {disc}/unknown.iso',data)
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual({r['status'] for r in plan['entries']},{'ready'})
        folders={Path(r['target']).parent.as_posix() for r in plan['entries']}
        self.assertEqual(folders,{'Games/USA/Example (USA)'})
        tx=self.apply(plan)
        for disc in range(1,5):
            self.assertEqual((self.root/f'Games/USA/Example (USA)/Example (USA) (Disc {disc}).iso').read_bytes(),bytes([disc])*4096)
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_converted_iso_redundant_cue_uses_same_checksum_path(self):
        data,_=self.converted_game()
        self.put('Old/unknown.iso',data)
        self.put('Old/unknown.cue',b'FILE "unknown.iso" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n')
        row=self.scan()['entries'][0]
        self.assertEqual(row['confidence'],'converted-checksum')
        self.assertEqual(row['ignored_cues'],['Old/unknown.cue'])

    def test_useful_cue_layout_is_not_reconstructed_as_a_plain_iso(self):
        data,_=self.converted_game()
        self.put('Old/Example (USA).iso',data)
        self.put('Old/Example (USA).cue',b'FILE "Example (USA).iso" BINARY\nTRACK 01 MODE1/2048\nPREGAP 00:02:00\nINDEX 01 00:00:00\n')
        with mock.patch.object(organizer.sector_tools,'digests',side_effect=AssertionError('must retain meaningful CUE layout')):
            row=self.scan()['entries'][0]
        self.assertTrue(row['source'].endswith('.cue'))
        self.assertEqual(row['confidence'],'filename')

    def test_converted_scan_without_accelerator_uses_portable_encoder(self):
        data,_=self.converted_game()
        self.put('unknown.iso',data)
        with mock.patch.object(organizer.sector_tools,'native_encoder',return_value=None):
            self.assertEqual(self.scan()['entries'][0]['confidence'],'converted-checksum')

    def test_review_groups_separate_filename_matches_choices_and_file_errors(self):
        cases=[({'status':'review','confidence':'filename'},'matched'),({'status':'ready'},'matched'),
            ({'status':'unmatched','confidence':'ambiguous'},'decisions'),({'status':'category-review'},'decisions'),
            ({'status':'unmatched','confidence':'unmatched'},'unidentified'),({'status':'blocked'},'problems'),
            ({'status':'unmatched','reason':'Missing CUE file'},'problems')]
        for row,expected in cases:self.assertEqual(organizer.review_bucket(row),expected)

    def test_empty_cleanup_previews_nested_directories_and_preserves_all_files(self):
        (self.root/'Old Games/A/Empty Game').mkdir(parents=True)
        self.put('Old Games/B/Remaining Game/game.iso',b'disc')
        self.put('Old Games/C/readme.txt',b'notes')
        self.put('Old Games/D/.hidden',b'hidden file')
        before=snapshot(self.root)
        planned=organizer.cleanup_empty(self.root)
        self.assertEqual(planned,['Old Games/A/Empty Game','Old Games/A'])
        self.assertTrue((self.root/'Old Games/A/Empty Game').is_dir())
        self.assertEqual(before,snapshot(self.root))
        self.assertEqual(organizer.cleanup_empty(self.root,apply=True),planned)
        self.assertFalse((self.root/'Old Games/A').exists())
        self.assertEqual(before,snapshot(self.root))
        self.assertTrue(self.root.is_dir())

    def test_empty_cleanup_preserves_support_hidden_and_link_directories(self):
        protected=['covers','previews','music','saves','.ohmy3do','SYSTEM','System Volume Information','$RECYCLE.BIN','.private']
        for name in protected:(self.root/name/'empty-child').mkdir(parents=True)
        (self.root/'junction/empty-child').mkdir(parents=True)
        check=organizer.directory_link
        with mock.patch.object(organizer,'directory_link',side_effect=lambda p:p==self.root/'junction' or check(p)):
            self.assertEqual(organizer.cleanup_empty(self.root,apply=True),[])
        for name in protected+['junction']:self.assertTrue((self.root/name/'empty-child').exists())

    def test_empty_cleanup_keeps_folder_if_a_file_arrives_after_preview(self):
        (self.root/'empty').mkdir()
        rmdir=Path.rmdir
        def changed(path):
            (path/'new.txt').write_bytes(b'arrived during cleanup')
            return rmdir(path)
        with mock.patch.object(Path,'rmdir',changed):
            self.assertEqual(organizer.cleanup_empty(self.root,apply=True),[])
        self.assertEqual((self.root/'empty/new.txt').read_bytes(),b'arrived during cleanup')

    def test_empty_cleanup_after_organizing_does_not_break_undo(self):
        self.put('Old Library/A/Example.iso',b'disc');self.catalog['games']=[game('Example (USA)',[b'disc'])]
        before=snapshot(self.root)
        tx=self.apply(self.scan())
        removed=organizer.cleanup_empty(self.root,apply=True)
        self.assertIn('Old Library',removed)
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_empty_cleanup_cli_does_not_need_catalog_or_plan(self):
        (self.root/'empty').mkdir()
        command=[sys.executable,str(ROOT/'tools/organize-library.py'),'clean-empty',str(self.root)]
        result=subprocess.run(command,check=True,stdout=subprocess.PIPE,text=True)
        self.assertIn('Would remove: empty',result.stdout)
        self.assertTrue((self.root/'empty').exists())
        subprocess.run(command+['--apply'],check=True,stdout=subprocess.PIPE)
        self.assertFalse((self.root/'empty').exists())

    def test_support_migration_then_organizing_updates_new_layout_and_both_undo(self):
        self.put('ohmy3do.ini',b'folder_workaround=1\n')
        self.put('Old/Example.iso',b'disc');self.catalog['games']=[game('Example (USA)',[b'disc'])]
        self.put('favorites.txt',b'Old/Example.iso\n');self.put('recent.txt',b'Old/Example.iso\n')
        self.put('last-position.txt',b'OH3DO_POSITION1\n/Storage/Old\nExample.iso\n')
        self.put('covers/Example.3dc',b'personal cover');self.put('previews/Example.3dv',b'personal preview')
        self.put('music/menu.3dm',b'personal music');self.put('saves/slot01.3ds',b'personal save')
        self.put('boot.iso',b'boot');before=snapshot(self.root)
        organizer.migrate_support(self.root)
        self.assertEqual(before,snapshot(self.root))
        migration=organizer.migrate_support(self.root,apply=True)
        self.assertEqual(organizer.index_tools.support_base(self.root),self.root/'.ohmy3do')
        self.assertFalse((self.root/'ohmy3do.ini').exists());self.assertFalse((self.root/'covers').exists())
        self.assertEqual((self.root/'.ohmy3do/music/menu.3dm').read_bytes(),b'personal music')
        tx=self.apply(self.scan())
        self.assertIn(b'Games/USA/Example (USA)/Example (USA).iso',(self.root/'.ohmy3do/favorites.txt').read_bytes())
        self.assertTrue((self.root/'.ohmy3do/library.3di').exists())
        self.assertFalse((self.root/'library.3di').exists())
        self.assertEqual(len(organizer.index_tools.existing_index(self.root/'.ohmy3do/covers')),1)
        tx.undo();migration.undo();self.assertEqual(before,snapshot(self.root))

    def test_support_migration_handles_empty_firmware_probe_and_undo(self):
        self.put('ohmy3do.ini',b'folder_workaround=1\n')
        self.put('.ohmy3do/ohmy3do.ini',b'')
        before=snapshot(self.root)
        self.assertEqual(organizer.index_tools.support_base(self.root),self.root)
        tx=organizer.migrate_support(self.root,apply=True)
        self.assertEqual((self.root/'.ohmy3do/ohmy3do.ini').read_bytes(),b'folder_workaround=1\n')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_support_migration_default_config_and_conflicts(self):
        (self.root/'saves/nested-empty').mkdir(parents=True)
        self.put('boot.iso',b'boot');before=snapshot(self.root)
        tx=organizer.migrate_support(self.root,apply=True)
        self.assertTrue((self.root/'.ohmy3do/ohmy3do.ini').stat().st_size)
        self.assertTrue((self.root/'.ohmy3do/saves/nested-empty').is_dir())
        self.assertIsNone(organizer.migrate_support(self.root,apply=True))
        tx.undo();self.assertEqual(before,snapshot(self.root))
        self.assertTrue((self.root/'saves/nested-empty').is_dir())
        self.put('covers/Game.png',b'old');self.put('.ohmy3do/covers/Game.png',b'new')
        before=snapshot(self.root)
        with self.assertRaisesRegex(ValueError,'destination already exists'):organizer.migrate_support(self.root,apply=True)
        self.assertEqual(before,snapshot(self.root))

    def test_support_migration_failure_rolls_back_without_moving_games(self):
        self.put('ohmy3do.ini',b'folder_workaround=1\n')
        self.put('favorites.txt',b'Game.iso\n');self.put('covers/Game.png',b'art')
        self.put('Game.iso',b'disc');before=snapshot(self.root)
        move=organizer.Transaction.move;calls=[]
        def fail(tx,operation):
            calls.append(operation)
            if len(calls)==2:raise OSError('Synthetic migration failure')
            return move(tx,operation)
        with mock.patch.object(organizer.Transaction,'move',fail):
            with self.assertRaisesRegex(OSError,'Synthetic migration failure'):organizer.migrate_support(self.root,apply=True)
        self.assertEqual(before,snapshot(self.root))

    def test_new_layout_media_import_writes_only_inside_support_directory(self):
        images=self.preview_fixture();self.cover_image(images/'Example.png')
        self.put('.ohmy3do/ohmy3do.ini',b'folder_workaround=1\n')
        before=snapshot(self.root)
        tx=organizer.apply_plan(self.root,self.scan(),self.cache,cover_dir=images)
        self.assertEqual(len(organizer.index_tools.existing_index(self.root/'.ohmy3do/covers')),1)
        self.assertFalse((self.root/'covers').exists())
        tx.undo();self.assertEqual(before,snapshot(self.root))

    @unittest.skipUnless(os.name=='nt','Windows junction check')
    def test_empty_cleanup_skips_real_windows_junction(self):
        import ctypes
        outside=Path(self.temp.name)/'outside';(outside/'empty').mkdir(parents=True)
        link=self.root/'junction'
        hidden=self.root/'hidden';hidden.mkdir()
        self.assertTrue(ctypes.windll.kernel32.SetFileAttributesW(str(hidden),0x12))
        subprocess.run(['cmd.exe','/d','/c','mklink','/J',str(link),str(outside)],cwd=self.temp.name,check=True,stdout=subprocess.PIPE)
        try:
            self.assertTrue(organizer.directory_link(link))
            self.assertEqual(organizer.cleanup_empty(self.root,apply=True),[])
            self.assertTrue((outside/'empty').is_dir())
            self.assertTrue(link.exists())
            self.assertTrue(hidden.is_dir())
        finally:
            link.rmdir()
            ctypes.windll.kernel32.SetFileAttributesW(str(hidden),0x10)

    def test_database_connection_closes_before_copy_and_cleanup_on_success_and_failure(self):
        connect=sqlite3.connect
        write=organizer.Transaction.write
        for fail in (False,True):
            with self.subTest(database_failure=fail):
                self.put('old.iso',b'synthetic')
                self.catalog['games']=[game('Example (USA)',[b'synthetic'])]
                before=snapshot(self.root)
                opened=[];paths=[]
                class TrackedConnection(sqlite3.Connection):
                    closed=False
                    def close(self):
                        super().close();self.closed=True
                    def executemany(self,*args,**kwargs):
                        if fail:raise OSError('Synthetic database failure')
                        return super().executemany(*args,**kwargs)
                def tracked_connect(path):
                    db=connect(path,factory=TrackedConnection)
                    opened.append(db);paths.append(Path(path))
                    return db
                def checked_write(tx,name,content):
                    if name=='.ohmy3do/library.sqlite':
                        self.assertTrue(opened and all(db.closed for db in opened))
                    return write(tx,name,content)
                with mock.patch.object(organizer.sqlite3,'connect',side_effect=tracked_connect), mock.patch.object(organizer.Transaction,'write',checked_write):
                    if fail:
                        with self.assertRaisesRegex(OSError,'Synthetic database failure'):self.apply(self.scan())
                    else:
                        tx=self.apply(self.scan())
                        self.assertEqual(tx.data['state'],'complete')
                        tx.undo()
                self.assertTrue(opened and all(db.closed for db in opened))
                self.assertTrue(all(not p.parent.exists() for p in paths))
                for db in opened:
                    with self.assertRaises(sqlite3.ProgrammingError):db.execute('SELECT 1')
                self.assertEqual(before,snapshot(self.root))

    def test_interactive_report_rebuild_does_not_scan_or_change_usb(self):
        self.put('Example.iso',b'disc');self.catalog['games']=[game('Example (USA)',[b'disc'])]
        plan=self.scan();before=snapshot(self.root)
        plan_path=Path(self.temp.name)/'plan.json';plan_path.write_bytes(organizer.json_bytes(plan))
        output=Path(self.temp.name)/'report.html'
        subprocess.run([sys.executable,str(ROOT/'tools/organize-library.py'),'report',str(plan_path),'--output',str(output)],check=True,stdout=subprocess.PIPE)
        self.assertIn('Matched (1)',output.read_text())
        self.assertEqual(before,snapshot(self.root))

    def test_interactive_report_filters_exports_choices_and_escapes_options(self):
        dangerous='Example </option><script>alert(1)</script> (USA)'
        plan=dict(root=str(self.root),entries=[
            dict(source='Example.iso',status='review',confidence='filename',reason='Unique name'),
            dict(source='Choose.iso',status='unmatched',confidence='ambiguous',reason='Choose a release',suggestions=[dangerous]),
            dict(source='Category.iso',status='category-review',reason='Choose category')])
        report=organizer.report_html(plan).decode()
        self.assertNotIn('</option><script>alert(1)</script>',report)
        self.assertIn('&lt;script&gt;',report)
        self.assertIn('Needs a choice (2)',report)
        if not shutil.which('node'):return
        script=re.search(r'<script>(.*?)</script>',report,re.S)[1]
        probe='''const assert=require('node:assert/strict');let downloaded;
const choice={value:'Example (USA)',addEventListener(){}};
function row(bucket,source,select){return {dataset:{bucket,source},textContent:source,hidden:true,querySelector(q){return q==='select.release'?select:null;}};}
const rowsFixture=[row('matched','Example.iso'),row('decisions','Choose.iso',choice)];
const elements={filter:{value:'',addEventListener(){}},visible:{},choices:{}};
const buttons=['decisions','matched','all'].map(v=>({dataset:{view:v},setAttribute(){},addEventListener(e,f){this.click=f;}}));
const releaseButton={addEventListener(e,f){this.click=f;}};
const document={querySelectorAll(q){return q==='tbody tr'?rowsFixture:q==='[data-view]'?buttons:[];},
getElementById(id){return id==='export-releases'?releaseButton:id==='export-categories'?{addEventListener(){}}:elements[id];},
createElement(){return {click(){}};}};
const URL={createObjectURL(blob){downloaded=blob;return 'blob:test';},revokeObjectURL(){}};
'''+script+'''
assert.equal(rowsFixture[0].hidden,true);assert.equal(rowsFixture[1].hidden,false);
buttons[1].click();assert.equal(rowsFixture[0].hidden,false);assert.equal(rowsFixture[1].hidden,true);
releaseButton.click();downloaded.text().then(text=>assert.deepEqual(JSON.parse(text),{'Choose.iso':'Example (USA)'}));
'''
        subprocess.run(['node','-e',probe],check=True,stdout=subprocess.PIPE)

    def cover_image(self, path, color='red'):
        from PIL import Image
        path.parent.mkdir(parents=True,exist_ok=True)
        Image.new('RGB',(300,420),color).save(path)
        return path

    def test_local_cover_resize_matching_multidisc_database_and_undo(self):
        from PIL import Image
        images=self.preview_fixture(discs=2)
        original=self.cover_image(images/'Box - Front'/'Example (USA).jpg')
        original_bytes=original.read_bytes()
        before=snapshot(self.root)
        with mock.patch.object(organizer.catalog_tools,'cover_names',side_effect=AssertionError('No network for local covers')):
            tx=organizer.apply_plan(self.root,self.scan(),self.cache,cover_dir=images,offline=True)
        index=organizer.index_tools.existing_index(self.root/'covers')
        self.assertEqual(len(index),2);self.assertEqual(len(set(index.values())),1)
        with Image.open(self.root/'covers'/next(iter(index.values()))) as image:
            self.assertEqual(image.size,(84,105));self.assertEqual(image.mode,'RGB');self.assertEqual(image.format,'PNG')
        records=json.loads((self.root/'.ohmy3do/library.json').read_text())['games']
        self.assertTrue(all(r['cover_source']=='Box - Front/Example (USA).jpg' for r in records))
        self.assertEqual(original.read_bytes(),original_bytes)
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_local_covers_ambiguous_and_region_disc_demo_filters(self):
        images=self.preview_fixture(discs=2)
        for name in ('Example.jpg','Example.png','Example (Japan).png','Example (Demo).png'):
            self.cover_image(images/name)
        prepared,warnings=organizer.prepare_local_covers(self.scan()['entries'],images)
        self.assertFalse(prepared);self.assertTrue(all('Ambiguous' in w for w in warnings))
        for path in images.iterdir():path.unlink()
        self.cover_image(images/'Example (USA) (Disc 1).jpg')
        prepared,warnings=organizer.prepare_local_covers(self.scan()['entries'],images)
        self.assertEqual(len(prepared),1);self.assertEqual(len(warnings),1)
        self.assertIn('Disc 2',warnings[0])

    def test_local_cover_exact_name_beats_generic_and_other_regions(self):
        images=self.preview_fixture()
        self.cover_image(images/'Example.png','blue')
        self.cover_image(images/'Example (USA).jpg','red')
        self.cover_image(images/'Example (Japan).jpg','green')
        rows=self.scan()['entries']
        prepared,warnings=organizer.prepare_local_covers(rows,images)
        self.assertEqual(len(prepared),1);self.assertFalse(warnings)
        self.assertEqual(rows[0]['cover_source'],'Example (USA).jpg')

    def test_local_cover_added_after_scan_is_preserved(self):
        images=self.preview_fixture()
        plan=self.scan()
        self.cover_image(self.root/'covers/Example (USA).png','blue')
        self.cover_image(images/'Example (USA).jpg','red')
        before=snapshot(self.root)
        tx=organizer.apply_plan(self.root,plan,self.cache,cover_dir=images)
        index=organizer.index_tools.existing_index(self.root/'covers')
        self.assertEqual(list(index.values()),['Example (USA).png'])
        self.assertEqual(len(list((self.root/'covers').glob('*.png'))),1)
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_local_cover_corrupt_missing_symlink_and_conflicting_flags(self):
        images=self.preview_fixture()
        (images/'Example.png').write_bytes(b'not an image')
        prepared,warnings=organizer.prepare_local_covers(self.scan()['entries'],images)
        self.assertFalse(prepared);self.assertIn('Local cover failed',warnings[0])
        (images/'Example.png').unlink()
        outside=self.cover_image(Path(self.temp.name)/'outside.png')
        (images/'Example.png').symlink_to(outside)
        prepared,warnings=organizer.prepare_local_covers(self.scan()['entries'],images)
        self.assertFalse(prepared);self.assertIn('No local cover',warnings[0])
        before=snapshot(self.root)
        for directory,covers in ((images/'missing',True),(images,False)):
            with self.assertRaises(ValueError):
                organizer.apply_plan(self.root,self.scan(),self.cache,covers=covers,cover_dir=directory)
        self.assertEqual(before,snapshot(self.root))

    def test_local_cover_rollback_removes_imported_images(self):
        images=self.preview_fixture()
        self.cover_image(images/'Example.jpg')
        before=snapshot(self.root)
        with mock.patch.object(organizer,'write_database',side_effect=OSError('test failure')):
            with self.assertRaises(OSError):
                organizer.apply_plan(self.root,self.scan(),self.cache,cover_dir=images)
        self.assertEqual(before,snapshot(self.root))

    def test_local_covers_and_previews_import_together(self):
        images=self.preview_fixture()
        self.cover_image(images/'Example.png')
        (images/'Example.mp4').write_bytes(b'fake clip')
        before=snapshot(self.root)
        def prepare(rows,directory,start):
            rows[0]['preview']='sample.3dv'
            return {'sample.3dv':b'clip'},[]
        with mock.patch.object(organizer,'prepare_previews',side_effect=prepare) as previews:
            tx=organizer.apply_plan(self.root,self.scan(),self.cache,cover_dir=images,preview_dir=images)
        previews.assert_called_once()
        self.assertEqual(len(organizer.index_tools.existing_index(self.root/'covers')),1)
        self.assertEqual(list(organizer.index_tools.existing_index(self.root/'previews').values()),['sample.3dv'])
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_preview_conversion_association_database_and_undo(self):
        clips=self.preview_fixture(discs=2)
        source=clips/'Example (USA).mp4';source.write_bytes(b'fake clip')
        before=snapshot(self.root)
        converter=organizer.module('prepare-preview')
        def convert(source,target,**kwargs):
            self.assertEqual(kwargs,dict(start=5))
            target.write_bytes(b'converted clip')
        with mock.patch.object(converter,'convert',side_effect=convert) as conversion, mock.patch.object(organizer,'module',return_value=converter):
            tx=organizer.apply_plan(self.root,self.scan(),self.cache,covers=False,preview_dir=clips,preview_start=5)
        conversion.assert_called_once()
        index=organizer.index_tools.existing_index(self.root/'previews')
        self.assertEqual(len(index),2)
        self.assertEqual(len(set(index.values())),1)
        self.assertEqual((self.root/'previews'/next(iter(index.values()))).read_bytes(),b'converted clip')
        records=json.loads((self.root/'.ohmy3do/library.json').read_text())['games']
        self.assertTrue(all(r['preview_source']=='Example (USA).mp4' for r in records))
        self.assertEqual(source.read_bytes(),b'fake clip')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_preview_ambiguous_region_demo_and_disc_filtering(self):
        clips=self.preview_fixture(discs=2)
        for name in ('Example.mp4','Example.avi','Example (Japan).mp4','Example (Demo).mp4'):
            (clips/name).write_bytes(b'fake')
        rows=self.scan()['entries']
        prepared,warnings=organizer.prepare_previews(rows,clips)
        self.assertFalse(prepared);self.assertEqual(len(warnings),2)
        self.assertTrue(all('Ambiguous' in w for w in warnings))
        for path in clips.iterdir():path.unlink()
        (clips/'Example (USA) (Disc 1).mp4').write_bytes(b'fake')
        converter=organizer.module('prepare-preview')
        with mock.patch.object(converter,'convert',side_effect=lambda s,t,**k:t.write_bytes(b'clip')), mock.patch.object(organizer,'module',return_value=converter):
            prepared,warnings=organizer.prepare_previews(rows,clips)
        self.assertEqual(len(prepared),1);self.assertEqual(len(warnings),1)
        self.assertIn('Disc 2',warnings[0])

    def test_preview_existing_added_after_scan_is_preserved(self):
        clips=self.preview_fixture()
        plan=self.scan()
        self.put('previews/Example (USA).3dv',b'personal preview')
        before=snapshot(self.root)
        converter=organizer.module('prepare-preview')
        with mock.patch.object(converter,'convert') as conversion, mock.patch.object(organizer,'module',return_value=converter):
            tx=organizer.apply_plan(self.root,plan,self.cache,covers=False,preview_dir=clips)
        conversion.assert_not_called()
        self.assertEqual(list(organizer.index_tools.existing_index(self.root/'previews').values()),['Example (USA).3dv'])
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_preview_failed_conversion_and_bad_directory(self):
        clips=self.preview_fixture();(clips/'Example.mp4').write_bytes(b'bad video')
        converter=organizer.module('prepare-preview')
        rows=self.scan()['entries']
        with mock.patch.object(converter,'convert',side_effect=subprocess.CalledProcessError(1,'ffmpeg')), mock.patch.object(organizer,'module',return_value=converter):
            prepared,warnings=organizer.prepare_previews(rows,clips)
        self.assertFalse(prepared);self.assertIn('Preview failed',warnings[0]);self.assertIsNone(rows[0]['preview'])
        before=snapshot(self.root)
        with self.assertRaises(ValueError):
            organizer.apply_plan(self.root,self.scan(),self.cache,covers=False,preview_dir=clips/'missing')
        self.assertEqual(before,snapshot(self.root))

    def test_preview_conversion_rollback_removes_new_files(self):
        clips=self.preview_fixture();(clips/'Example.mp4').write_bytes(b'fake')
        before=snapshot(self.root)
        converter=organizer.module('prepare-preview')
        with mock.patch.object(converter,'convert',side_effect=lambda s,t,**k:t.write_bytes(b'clip')), mock.patch.object(organizer,'module',return_value=converter), mock.patch.object(organizer,'write_database',side_effect=OSError('test failure')):
            with self.assertRaises(OSError):
                organizer.apply_plan(self.root,self.scan(),self.cache,covers=False,preview_dir=clips)
        self.assertEqual(before,snapshot(self.root))

    @unittest.skipUnless(shutil.which('ffmpeg'),'FFmpeg required for actual clip conversion')
    def test_preview_real_ffmpeg_import_and_native_player(self):
        clips=self.preview_fixture()
        subprocess.run(['ffmpeg','-v','error','-f','lavfi','-i','testsrc2=size=96x80:rate=6',
            '-t','5','-c:v','mpeg4',str(clips/'Example (USA).mp4')],check=True)
        before=snapshot(self.root)
        tx=organizer.apply_plan(self.root,self.scan(),self.cache,covers=False,preview_dir=clips,preview_start=1)
        index=organizer.index_tools.existing_index(self.root/'previews')
        clip=self.root/'previews'/next(iter(index.values()))
        data=clip.read_bytes()
        self.assertEqual(data[:8],b'OM3DVID1')
        self.assertEqual(data[-8:],b'END3DV01')
        self.assertEqual(len(data),24+24*64*80*2+8)
        player=ROOT/'build/test_usb_media'
        if player.exists():
            subprocess.run([str(player),'video',str(clip)],check=True,stdout=subprocess.PIPE)
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_iso_checksum_identification_database_history_and_undo(self):
        source='Unsorted/random.iso'
        payload=b'synthetic disc data'
        self.put(source,payload)
        self.put('boot.iso',b'menu')
        self.put('ohmy3do.ini',b'folder_workaround=1\r\n')
        self.put('favorites.txt',b'\xef\xbb\xbf# Favourites\r\n  Unsorted/random.iso \r\n')
        self.put('recent.txt',b'Unsorted/random.iso\n')
        self.put('last-position.txt',b'OH3DO_POSITION1\n/Storage/Unsorted\nrandom.iso\n')
        self.catalog['games']=[game('Example (USA)',[payload])]
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual(before,snapshot(self.root))

        self.assertEqual(plan['entries'][0]['confidence'],'checksum')
        tx=self.apply(plan)
        target='Games/USA/Example (USA)/Example (USA).iso'
        self.assertEqual((self.root/target).read_bytes(),payload)
        self.assertIn(target.encode(),(self.root/'favorites.txt').read_bytes())
        self.assertTrue((self.root/'favorites.txt').read_bytes().startswith(b'\xef\xbb\xbf# Favourites\r\n  '))
        self.assertIn(b'/Storage/Games/USA/Example (USA)\nExample (USA).iso',(self.root/'last-position.txt').read_bytes())
        self.assertEqual((self.root/'library.3di').read_bytes()[:8],b'OM3DLIB1')
        with contextlib.closing(sqlite3.connect(self.root/'.ohmy3do/library.sqlite')) as db:
            self.assertEqual(db.execute('select path,genre,publisher,year from games').fetchone(),(target,'Adventure','Example Publisher',1995))
        journal=tx.path.relative_to(self.root).as_posix()
        organizer.Transaction(self.root,path=journal).undo()
        self.assertEqual(before,snapshot(self.root))

    def test_menu_nul_terminated_text_with_stale_tail_remaps_and_undoes(self):
        source='Unsorted/old.iso';data=b'synthetic disc'
        self.put(source,data);self.catalog['games']=[game('Example (USA)',[data])]
        self.put('last-position.txt',b'OH3DO_POSITION1\n/Storage/Unsorted\nold.iso\n\0old data\nextra lines\n')
        self.put('favorites.txt',b'Unsorted/old.iso\0Unsorted/stale.iso\n')
        self.put('recent.txt',b'Unsorted/old.iso\n\0stale history')
        before=snapshot(self.root)
        tx=self.apply(self.scan())
        target='Games/USA/Example (USA)/Example (USA).iso'
        self.assertEqual((self.root/'favorites.txt').read_bytes(),target.encode())
        self.assertEqual((self.root/'recent.txt').read_bytes(),target.encode()+b'\n')
        self.assertEqual((self.root/'last-position.txt').read_bytes(),b'OH3DO_POSITION1\n/Storage/Games/USA/Example (USA)\nExample (USA).iso\n')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_invalid_saved_positions_reset_without_blocking_and_undo_restores_bytes(self):
        for raw in (b'bad header\n',b'OH3DO_POSITION1\n/Storage/../bad\nold.iso\n',
                    b'OH3DO_POSITION1\n/Other\nold.iso\n',b'OH3DO_POSITION1\n/Storage\n\xff\n',
                    b'OH3DO_POSITION1\n/Storage\nold.iso\nextra\n'):
            with self.subTest(raw=raw):
                self.put('old.iso',b'synthetic');self.catalog['games']=[game('Example (USA)',[b'synthetic'])]
                self.put('last-position.txt',raw);before=snapshot(self.root)
                tx=self.apply(self.scan())
                self.assertEqual((self.root/'last-position.txt').read_bytes(),b'OH3DO_POSITION1\n/Storage\n\n')
                tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_saved_position_bom_crlf_and_empty_selection(self):
        self.put('Unsorted/old.iso',b'synthetic');self.catalog['games']=[game('Example (USA)',[b'synthetic'])]
        self.put('last-position.txt',b'\xef\xbb\xbfOH3DO_POSITION1\r\n/Storage/Unsorted\r\n\r\n\0')
        before=snapshot(self.root)
        tx=self.apply(self.scan())
        self.assertEqual((self.root/'last-position.txt').read_bytes(),b'OH3DO_POSITION1\n/Storage\n\n')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_text_preflight_failure_does_not_create_journal_or_move_files(self):
        self.put('old.iso',b'synthetic');self.catalog['games']=[game('Example (USA)',[b'synthetic'])]
        self.put('favorites.txt',b'\xff')
        before=snapshot(self.root)
        with mock.patch.object(organizer.Transaction,'move',side_effect=AssertionError('No moves before text preflight')):
            with self.assertRaises(UnicodeError):self.apply(self.scan())
        self.assertEqual(before,snapshot(self.root))
        self.assertFalse((self.root/'.ohmy3do/transactions').exists())


    def test_seven_categories_move_to_separate_roots_and_undo(self):
        cases=[('Example (USA)','Adventure','Games'),('Example (USA) (Demo)','Adventure','Demos'),
               ('Interactive Sampler CD (USA)','Compilation','Samplers'),
               ('Storage Manager (USA)',None,'Applications'),('Video Almanac (USA)',None,'Multimedia'),
               ('Learning Numbers (USA)','Education; Puzzle','Educational'),
               ('Example (USA) (Bonus Disc)','Adventure','Bonus Discs')]
        for i,(title,genre,category) in enumerate(cases):
            data=f'disc {i}'.encode();self.put(f'old{i}.iso',data)
            row=game(title,[data]);row['metadata']['genre']=genre
            self.catalog['games'].append(row)
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual({r['category'] for r in plan['entries']},set(organizer.CATEGORIES))
        self.assertTrue(all(r['status']=='ready' for r in plan['entries']))
        tx=self.apply(plan)
        with contextlib.closing(sqlite3.connect(self.root/'.ohmy3do/library.sqlite')) as db:
            self.assertEqual({row[0] for row in db.execute('select category from games')},set(organizer.CATEGORIES))
        for row in plan['entries']:self.assertTrue((self.root/row['target']).is_file())
        self.assertTrue(all(r['source']==r['target'] for r in self.scan()['entries']))
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_bonus_folder_separates_extras_preserves_cue_labels_and_undoes(self):
        self.catalog['games']=[game('Example (USA)',[b'game'])]
        self.put('Games/Example (USA).iso',b'game')
        source='4 Bonus Discs/Example/Example (USA) [CD-Audio].cue'
        cue=self.put(source,b'FILE "audio.bin" BINARY\r\nTRACK 01 AUDIO\r\n')
        self.put('4 Bonus Discs/Example/audio.bin',b'extra audio')
        before=snapshot(self.root)
        plan=self.scan(quick=True)
        bonus=next(r for r in plan['entries'] if r['source']==source)
        self.assertEqual(bonus['category'],'Bonus Discs')
        self.assertIn('[CD-Audio]',bonus['target'])
        self.assertTrue(bonus['target'].startswith('Bonus Discs/USA/'))
        self.assertTrue(all(r['status']=='review' for r in plan['entries']))
        tx=self.apply(plan,names=True)
        self.assertIn(b'[CD-Audio].bin',(self.root/bonus['target']).read_bytes())
        self.assertTrue((self.root/'Games/USA/Example (USA)/Example (USA).iso').exists())
        with contextlib.closing(sqlite3.connect(self.root/'.ohmy3do/library.sqlite')) as db:
            self.assertEqual({r[0] for r in db.execute('SELECT category FROM games')},{'Games','Bonus Discs'})
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_bonus_manual_override_wins_over_folder_and_persists(self):
        source='4 Bonus Discs/Example.iso'
        self.put(source,b'game');self.catalog['games']=[game('Example (USA)',[b'game'])]
        plan=organizer.build_plan(self.root,self.catalog,categories={source:'Games'})
        self.assertEqual(plan['entries'][0]['category'],'Games')
        source='4 Bonus Discs/Example.iso'
        plan=organizer.build_plan(self.root,self.catalog,categories={source:'bonus discs'})
        before=snapshot(self.root)
        tx=self.apply(plan)
        row=self.scan()['entries'][0]
        self.assertEqual(row['category'],'Bonus Discs')
        self.assertEqual(row['category_reason'],'Explicit category override')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_uncertain_categories_stay_put_even_with_filename_approval(self):
        cases=[('Music Experience (USA)','Music'),('Unknown Disc (USA)',None),
               ('Sampler and Storage Manager (USA)',None),('Atlas Adventure (USA)','Adventure'),
               ('Example (USA) (Sample)','Adventure')]
        for i,(title,genre) in enumerate(cases):
            data=f'uncertain {i}'.encode();self.put(f'old{i}.iso',data)
            row=game(title,[data]);row['metadata']['genre']=genre;self.catalog['games'].append(row)
        plan=self.scan()
        self.assertTrue(all(r['status']=='category-review' and not r['moves'] and r['target'] is None for r in plan['entries']))
        tx=self.apply(plan,names=True)
        self.assertTrue(all((self.root/f'old{i}.iso').exists() for i in range(len(cases))))
        self.assertFalse((self.root/'Games').exists())
        with contextlib.closing(sqlite3.connect(self.root/'.ohmy3do/library.sqlite')) as db:
            self.assertEqual(db.execute('select count(*) from games where category_review=1 and category is null').fetchone()[0],len(cases))
        tx.undo()

    def test_category_override_persists_after_move_with_cue_and_saved_links(self):
        self.put('old.cue',b'FILE "old.bin" BINARY\n');self.put('old.bin',b'utility data')
        self.put('favorites.txt',b'old.cue\n')
        row=game('Unknown Program (USA)',[b'utility data']);row['metadata']['genre']=None
        self.catalog['games']=[row]
        before=snapshot(self.root)
        self.assertEqual(self.scan()['entries'][0]['status'],'category-review')
        plan=organizer.build_plan(self.root,self.catalog,categories={'old.cue':'Applications'})
        self.assertEqual(plan['entries'][0]['status'],'ready')
        tx=self.apply(plan)
        target=plan['entries'][0]['target']
        self.assertTrue(target.startswith('Applications/'))
        self.assertIn(target.encode(),(self.root/'favorites.txt').read_bytes())
        row=self.scan()['entries'][0]
        self.assertEqual(row['category'],'Applications');self.assertEqual(row['source'],row['target'])
        self.assertEqual(row['category_reason'],'Explicit category override')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_classification_does_not_confuse_full_games_or_fmv_with_samplers(self):
        for title,genre in [('Demolition Man (USA)','Action'),('Game Collection (USA)','Compilation'),
                            ('Interactive Movie Adventure (USA)','Adventure'),('Story (USA)','Visual Novel')]:
            row=game(title,[b'data']);row['metadata']['genre']=genre
            classified=organizer.release_records([row])[0]
            self.assertEqual(organizer.classify(classified)[0],'Games')
        row=game('Learning Atlas (USA)',[b'data']);row['metadata']['genre']='Education'
        self.assertEqual(organizer.classify(organizer.release_records([row])[0])[0],'Multimedia')
        for title in ('Learning Numbers (USA) (Demo)','Learning Numbers Sampler (USA)'):
            row=game(title,[b'data']);row['metadata']['genre']='Education'
            expected='Demos' if '(Demo)' in title else 'Samplers'
            self.assertEqual(organizer.classify(organizer.release_records([row])[0])[0],expected)

    def test_invalid_category_overrides_and_legacy_plan_are_rejected(self):
        self.put('old.iso',b'data');self.catalog['games']=[game('Example (USA)',[b'data'])]
        for overrides in ({'old.iso':'../Games'},{'old.iso':None},{'missing.iso':'Games'},['Games']):
            with self.assertRaises(ValueError):organizer.build_plan(self.root,self.catalog,categories=overrides)
        plan=self.scan();plan['schema']=1
        with self.assertRaisesRegex(ValueError,'scan this drive again'):self.apply(plan)

    def test_metadata_cache_upgrade_reuses_downloads_and_adds_release_type(self):
        dat='game ( name "Example (USA)" region "USA" rom ( name "Example.bin" size 4 sha1 ABCD ) )'
        (self.cache/'redump.dat').write_text(dat)
        xml=b'<LaunchBox><Game><Name>Example</Name><DatabaseID>42</DatabaseID><Platform>3DO Interactive Multiplayer</Platform><Genres>Adventure</Genres><ReleaseType>Demo</ReleaseType></Game></LaunchBox>'
        with zipfile.ZipFile(self.cache/'launchbox.zip','w') as z:z.writestr('Metadata.xml',xml)
        (self.cache/'catalog.json').write_text(json.dumps(self.catalog))
        with mock.patch.object(organizer.catalog_tools,'download',side_effect=AssertionError('must reuse local files')):
            catalog=organizer.catalog_tools.load_catalog(self.cache,offline=True)
        self.assertEqual(catalog['schema'],2)
        row=organizer.release_records(catalog['games'])[0]
        self.assertEqual(row['metadata']['release_type'],'Demo')
        self.assertEqual(organizer.classify(row)[0],'Demos')

    def test_multidisc_cue_renames_tracks_and_preserves_crlf(self):
        for disc in (1,2):
            self.put(f'Old{disc}/disc.cue', b'REM a comment\r\nFILE "track01.bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n')
            self.put(f'Old{disc}/track01.bin',f'data for disc {disc}'.encode())
            self.catalog['games'].append(game(f'Example (USA) (Disc {disc})',[f'data for disc {disc}'.encode()]))
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual([r['status'] for r in plan['entries']],['ready','ready'])
        tx=self.apply(plan)
        for disc in (1,2):
            folder='Games/USA/Example (USA)'
            cue=self.root/f'{folder}/Example (USA) (Disc {disc}).cue'
            self.assertIn(f'FILE "Example (USA) (Disc {disc}).bin" BINARY\r\n'.encode(),cue.read_bytes())
            self.assertIn(b'REM a comment\r\n',cue.read_bytes())
            refs,*_=organizer.cue_files(self.root,cue.relative_to(self.root).as_posix())
            self.assertEqual(len(refs),1)
        tx.undo()
        self.assertEqual(before,snapshot(self.root))

    def test_redundant_iso_cue_archives_and_remaps_links_with_complete_undo(self):
        cue='Old/Example (USA).cue';iso='Old/data.iso';data=b'synthetic ISO'
        raw=b'CATALOG 4984824063921\r\nFILE "data.iso" BINARY\r\n  TRACK 01 MODE1/2048\r\n    INDEX 01 00:00:00\r\n'
        self.put(cue,raw);self.put(iso,data)
        self.catalog['games']=[game('Example (USA)',[data])]
        self.put('favorites.txt',(cue+'\n').encode());self.put('recent.txt',(cue+'\n').encode())
        self.put('last-position.txt',b'OH3DO_POSITION1\n/Storage/Old\nExample (USA).cue\n\0')
        self.put('covers/Example (USA).3dc',b'existing cover')
        self.put('previews/Example (USA).3dv',b'existing preview')
        before=snapshot(self.root)
        plan=organizer.build_plan(self.root,self.catalog,quick=True,overrides={cue:'Example (USA)'},categories={cue:'Games'})
        self.assertEqual(len(plan['entries']),1)
        row=plan['entries'][0]
        self.assertEqual(row['source'],iso);self.assertEqual(row['confidence'],'manual')
        self.assertEqual(row['ignored_cues'],[cue]);self.assertTrue(row['target'].endswith('.iso'))
        tx=self.apply(plan)
        archived=[m['target'] for m in row['moves'] if m['source']==cue][0]
        self.assertTrue(archived.startswith('.ohmy3do/ignored-cues/'))
        self.assertEqual((self.root/archived).read_bytes(),raw)
        self.assertFalse((self.root/cue).exists())
        self.assertEqual((self.root/'favorites.txt').read_bytes(),(row['target']+'\n').encode())
        self.assertIn(b'Example (USA).iso',(self.root/'last-position.txt').read_bytes())
        self.assertEqual(list(organizer.index_tools.existing_index(self.root/'covers').values()),['Example (USA).3dc'])
        self.assertEqual(list(organizer.index_tools.existing_index(self.root/'previews').values()),['Example (USA).3dv'])
        self.assertEqual([u['source'] for u in organizer.scan_units(self.root)],[row['target']])
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_useful_or_uncertain_cues_are_never_treated_as_redundant(self):
        prefix='FILE "data.iso" BINARY\n'
        for tail in ('TRACK 01 AUDIO\nINDEX 01 00:00:00\n',
                     'TRACK 01 MODE2/2352\nINDEX 01 00:00:00\n',
                     'TRACK 01 MODE1/2048\nPREGAP 00:02:00\nINDEX 01 00:00:00\n',
                     'TRACK 01 MODE1/2048\nINDEX 00 00:00:00\nINDEX 01 00:02:00\n',
                     'TRACK 01 MODE1/2048\nINDEX 01 00:00:01\n',
                     'TRACK 01 MODE1/2048\nINDEX 01 00:00:00\nTRACK 02 AUDIO\nINDEX 01 01:00:00\n',
                     'TRACK 01 MODE1/2048\nINDEX 01 00:00:00\nFLAGS PRE\n'):
            self.assertIsNone(organizer.index_tools.redundant_iso_name(prefix+tail))
        self.assertIsNone(organizer.index_tools.redundant_iso_name('FILE "data.bin" BINARY\nTRACK 01 MODE1/2352\nINDEX 01 00:00:00\n'))
        self.assertIsNone(organizer.index_tools.redundant_iso_name('FILE "../data.iso" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n'))

    def test_shared_iso_wrappers_remain_blocked_without_archival(self):
        for name in ('a.cue','b.cue'):
            self.put(name,b'FILE "data.iso" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n')
        self.put('data.iso',b'synthetic')
        units=organizer.scan_units(self.root)
        self.assertEqual({u['source'] for u in units},{'a.cue','b.cue'})
        self.assertTrue(all(u.get('error') and not u.get('ignored_cues') for u in units))

    def test_redundant_cue_conflicting_choices_fail_before_changes(self):
        self.put('Example.cue',b'FILE "data.iso" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n')
        self.put('data.iso',b'synthetic');self.catalog['games']=[game('Example (USA)',[b'synthetic'])]
        before=snapshot(self.root)
        with self.assertRaisesRegex(ValueError,'Conflicting ISO/CUE'):
            organizer.build_plan(self.root,self.catalog,categories={'Example.cue':'Games','data.iso':'Demos'})
        self.assertEqual(before,snapshot(self.root))

    def test_media_index_ignores_redundant_wrappers_and_archived_cues(self):
        self.put('Example.cue',b'FILE "data.iso" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n')
        self.put('data.iso',b'synthetic')
        self.put('.ohmy3do/ignored-cues/old.cue',b'archived original')
        organizer.index_tools.index(self.root)
        data=(self.root/'library.3di').read_bytes()
        self.assertEqual(int.from_bytes(data[8:12],'big'),1)
        self.assertIn(b'data.iso\0',data);self.assertNotIn(b'Example.cue',data)

    def test_whole_track_set_needed_for_hash_match(self):
        for name,data in [('one.bin',b'same first track'),('two.bin',b'actual second track')]:self.put(name,data)
        self.put('unidentified.cue',b'FILE "one.bin" BINARY\nFILE "two.bin" BINARY\n')
        self.catalog['games']=[game('Wrong (USA)',[b'same first track',b'other second track']),game('Right (USA)',[b'same first track',b'actual second track'])]
        self.assertEqual(self.scan()['entries'][0]['canonical'],'Right (USA)')

    def test_cue_case_insensitive_references_and_broken_iso_group(self):
        self.put('Valid/disc.cue',b'FILE "TRACK01.BIN" BINARY\r\n')
        self.put('Valid/track01.bin',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        self.put('Broken/disc.cue',b'FILE "DATA.ISO" BINARY\nFILE "missing.bin" BINARY\n')
        self.put('Broken/data.iso',b'payload')
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual(len(plan['entries']),2)
        self.assertEqual(next(r for r in plan['entries'] if r['source'].startswith('Broken'))['status'],'unmatched')
        tx=self.apply(plan)
        self.assertTrue((self.root/'Broken/data.iso').exists())
        cue=self.root/'Games/USA/Example (USA)/Example (USA).cue'
        self.assertIn(b'FILE "Example (USA).bin"',cue.read_bytes())
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_filename_matches_require_explicit_acceptance(self):
        self.put('Example (1995)(Publisher)(US)[!].iso',b'converted, no matching hash')
        self.catalog['games']=[game('Example (USA)',[b'original raw sectors'])]
        plan=self.scan(quick=True)
        self.assertEqual(plan['entries'][0]['status'],'review')
        tx=self.apply(plan)
        self.assertTrue((self.root/'Example (1995)(Publisher)(US)[!].iso').exists())
        tx.undo()
        tx=self.apply(plan,names=True)
        self.assertTrue((self.root/'Games/USA/Example (USA)/Example (USA).iso').exists())
        tx.undo()

    def test_ambiguous_revision_no_fuzzy_moves_and_manual_override(self):
        self.put('Example (USA).iso',b'unknown')
        self.catalog['games']=[game('Example (USA) (Rev 1)',[b'one']),game('Example (USA) (Rev 2)',[b'two'])]
        plan=self.scan(quick=True)
        self.assertEqual(plan['entries'][0]['confidence'],'ambiguous')
        self.assertFalse(plan['entries'][0]['moves'])
        self.put('Exampl (USA).iso',b'typo')
        plan=self.scan(quick=True)
        self.assertEqual(next(r for r in plan['entries'] if r['source'].startswith('Exampl ('))['status'],'unmatched')
        plan=self.scan(quick=True,overrides={'Example (USA).iso':'Example (USA) (Rev 1)'})
        self.assertEqual(next(r for r in plan['entries'] if r['source'].startswith('Example ('))['confidence'],'manual')

    def test_changed_source_and_destination_collision_preflight(self):
        source=self.put('old.iso',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        plan=self.scan()
        self.put('Games/USA/Example (USA)/Example (USA).iso',b'do not replace')
        with self.assertRaisesRegex(ValueError,'already exists'):self.apply(plan)
        self.assertEqual(source.read_bytes(),b'payload')
        (self.root/'Games/USA/Example (USA)/Example (USA).iso').unlink()
        source.write_bytes(b'changed payload')
        with self.assertRaisesRegex(ValueError,'changed since scan'):self.apply(plan)

    def test_duplicate_targets_block_both_entries(self):
        self.put('one.iso',b'duplicate');self.put('two.iso',b'duplicate')
        self.catalog['games']=[game('Example (USA)',[b'duplicate'])]
        self.assertTrue(all(r['status']=='blocked' for r in self.scan()['entries']))

    def test_catalog_dump_variants_with_same_title_and_long_disc_names(self):
        title='A very long game title ' * 7 + '(Japan) (Disc 2)'
        self.put('unknown.iso',b'payload')
        self.catalog['games']=[game(title,[b'payload'],'Japan'),game(title,[b'other dump'],'Japan')]
        plan=self.scan()
        self.assertEqual(plan['entries'][0]['status'],'ready')
        self.assertTrue(plan['entries'][0]['target'].endswith('(Disc 2).iso'))
        self.assertIn('[Variant ',plan['entries'][0]['target'])
        self.assertLessEqual(len(Path(plan['entries'][0]['target']).stem.encode()),110)
        variant_name=plan['entries'][0]['canonical']
        plan=self.scan(quick=True,overrides={'unknown.iso':variant_name})
        self.assertEqual(plan['entries'][0]['status'],'ready')
        self.put(title[:80]+'.iso',b'does not matter')
        self.catalog['games']=[game('Example (USA)',[b'one']),game('Example (USA)',[b'two'])]
        self.put('Example (USA).iso',b'converted')
        row=next(r for r in self.scan(quick=True)['entries'] if r['source']=='Example (USA).iso')
        self.assertEqual(row['confidence'],'ambiguous')

    def test_unnamed_variants_have_stable_distinct_codes_and_undo(self):
        self.catalog['games']=[game('Example (USA)',[b'one']),game('Example (USA)',[b'two'])]
        self.put('one.iso',b'one');self.put('two.iso',b'two')
        before=snapshot(self.root)
        plan=self.scan()
        self.assertTrue(all(r['status']=='ready' for r in plan['entries']))
        self.assertEqual(len({r['target'] for r in plan['entries']}),2)
        self.assertTrue(all('[Variant ' in r['target'] for r in plan['entries']))
        targets={r['source']:r['target'] for r in plan['entries']}
        self.catalog['games'].reverse()
        self.assertEqual(targets,{r['source']:r['target'] for r in self.scan()['entries']})
        tx=self.apply(plan)
        rescanned=self.scan(quick=True)
        self.assertTrue(all(r['status']=='review' and r['source']==r['target'] for r in rescanned['entries']))
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_rom_alt_label_kept_in_iso_and_cue_track_names(self):
        standard=game('Example (USA)',[b'normal'])
        alternate=game('Example (USA)',[b'alternate'])
        standard['roms'][0]['name']='Example (USA).bin'
        alternate['roms'][0]['name']='Example (USA) (Alt).bin'
        self.catalog['games']=[standard,alternate]
        self.put('normal.iso',b'normal')
        self.put('alt.cue',b'FILE "track.bin" BINARY\n')
        self.put('track.bin',b'alternate')
        before=snapshot(self.root)
        plan=self.scan()
        self.assertTrue(all(r['status']=='ready' for r in plan['entries']))
        row=next(r for r in plan['entries'] if r['source']=='alt.cue')
        self.assertTrue(row['target'].endswith('Example (USA) (Alt).cue'))
        self.assertNotIn('[Variant ',row['target'])
        tx=self.apply(plan)
        self.assertIn(b'FILE "Example (USA) (Alt).bin"',(self.root/row['target']).read_bytes())
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_serial_only_catalog_duplicates_do_not_get_variant_codes(self):
        first=game('Example (USA)',[b'same bytes'])
        second=copy.deepcopy(first);second['serial']='another printing'
        self.catalog['games']=[first,second]
        self.put('copy1.iso',b'same bytes');self.put('copy2.iso',b'same bytes')
        rows=self.scan()['entries']
        self.assertTrue(all(r['status']=='blocked' for r in rows))
        self.assertTrue(all('[Variant ' not in r['target'] for r in rows))

    def test_dragon_lore_trailing_disc_number_matches_only_that_disc(self):
        source='1 Games/1 US/A - F/Dragon Lore - The Legend Begins/Dragon Lore - The Legend Begins - Disc 1.iso'
        self.put(source,b'converted ISO')
        self.catalog['games']=[game(f'Dragon Lore - The Legend Begins (USA, Germany) (Disc {disc})',[f'data {disc}'.encode()]) for disc in (1,2,3)]
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['status'],'review')
        self.assertEqual(row['disc'],1)
        self.assertEqual(row['canonical'],'Dragon Lore - The Legend Begins (USA, Germany) (Disc 1)')
        self.assertTrue(row['target'].endswith('(Disc 1).iso'))
        for suffix in (' - Disc 2',' Disk 02',' CD3','_CD_03',' - Disc 2 of 3'):
            self.assertIsNotNone(organizer.disc_of('Game'+suffix))
            self.assertEqual(organizer.identification_title('Game'+suffix),'game')
        self.assertIsNone(organizer.disc_of('Discworld'))

    def test_region_variants_still_ambiguous_with_trailing_disc(self):
        self.put('Example - Disc 1.iso',b'unknown')
        self.catalog['games']=[game('Example (USA) (Disc 1)',[b'USA']),game('Example (Japan) (Disc 1)',[b'JP'],'Japan')]
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['confidence'],'ambiguous')
        self.assertEqual(len(row['suggestions']),2)

    def test_numbered_region_folder_resolves_d_and_daedalus_disc_sets(self):
        for title,other,discs in [('D','Germany',2),('Daedalus Encounter, The','Korea',4)]:
            for disc in range(1,discs+1):
                self.put(f'1 Games/1 US/A - F/{title}/{title} - Disc {disc}.iso',b'converted ISO')
                self.catalog['games'].append(game(f'{title} (USA, Europe) (Disc {disc})',[b'raw US']))
                self.catalog['games'].append(game(f'{title} ({other}) (Disc {disc})',[b'raw other'],other))
        rows=self.scan(quick=True)['entries']
        self.assertEqual(len(rows),6)
        self.assertTrue(all(r['status']=='review' and '(USA, Europe)' in r['canonical'] for r in rows))
        self.assertTrue(all('folder region USA' in r['reason'] for r in rows))
        self.assertFalse(organizer.folder_regions('US Games/Game.iso'))
        self.assertEqual(organizer.folder_regions('1 Games/1 US/A - F/Game.iso'),{'USA'})
        self.assertEqual(organizer.folder_regions('US/JP/Game.iso'),{'USA','Japan'})

    def test_unknown_hub_codes_keep_distinct_names_without_claiming_redump_edition(self):
        title="Dragon's Lair"
        self.catalog['games']=[game(title+' (Japan)',[b'jp'],'Japan'),game(title+' (USA) (Rev 1)',[b'rev1']),game(title+' (USA, Korea)',[b'base'])]
        for code in ('DH 65003','DH 65003-2'):
            self.put(f'1 Games/1 US/A - F/{title}/{title} [{code}].iso',code.encode())
        before=snapshot(self.root)
        plan=self.scan(quick=True)
        self.assertTrue(all(r['status']=='review' and r['edition_unresolved'] for r in plan['entries']))
        self.assertTrue(all(r['catalog_name'] is None for r in plan['entries']))
        self.assertEqual(len({r['target'] for r in plan['entries']}),2)
        for row in plan['entries']:
            self.assertIn(row['variant'],row['target'])
            self.assertNotIn('Rev 1',row['target'])
            self.assertNotIn('RE1',row['target'])
            self.assertIn('exact edition unresolved',row['reason'])
        tx=self.apply(plan,names=True)
        with contextlib.closing(sqlite3.connect(self.root/'.ohmy3do/library.sqlite')) as db:
            self.assertEqual(db.execute('select count(*) from games where edition_unresolved=1 and catalog_name is null').fetchone()[0],2)
        rescanned=self.scan(quick=True)
        self.assertTrue(all(r['source']==r['target'] for r in rescanned['entries']))
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_exact_catalog_serial_matches_and_conflicting_region_stays_ambiguous(self):
        usa=game('Example (USA)',[b'USA']);usa['serial']='DH 65003'
        japan=game('Example (Japan)',[b'JP'],'Japan');japan['serial']='FZ-SJ0153'
        self.catalog['games']=[usa,japan]
        self.put('Example [dh-65003].iso',b'converted')
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['status'],'review');self.assertEqual(row['canonical'],'Example (USA)')
        self.assertIn('serial match',row['reason'])
        self.put('1 US/Example [FZ SJ0153].iso',b'converted')
        row=next(r for r in self.scan(quick=True)['entries'] if r['source'].startswith('1 US'))
        self.assertEqual(row['confidence'],'ambiguous')
        self.assertIn('conflicts',row['reason'])

    def test_unknown_code_survives_even_with_only_one_regional_candidate(self):
        self.catalog['games']=[game('Example (USA)',[b'raw'])]
        self.put('1 US/Example [AB 1234].iso',b'converted')
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['status'],'review')
        self.assertTrue(row['edition_unresolved'])
        self.assertIn('[AB 1234]',row['target'])
        stem=organizer.disc_stem('A long title '*15+' (USA) [DH 65003-2] (Disc 1)',95)
        self.assertIn('[DH 65003-2]',stem)
        self.assertTrue(stem.endswith('(Disc 1)'))
        self.assertLessEqual(len(stem.encode()),95)

    def test_explicit_filename_region_precedes_folder_region(self):
        self.catalog['games']=[game('Example (USA) (Disc 1)',[b'US']),game('Example (Japan) (Disc 1)',[b'JP'],'Japan')]
        self.put('1 US/Example (Japan) - Disc 1.iso',b'converted')
        row=self.scan(quick=True)['entries'][0]
        self.assertEqual(row['status'],'review')
        self.assertEqual(row['canonical'],'Example (Japan) (Disc 1)')

    def test_broken_shared_and_traversing_cues_stay_put(self):
        self.put('missing.cue',b'FILE "absent.bin" BINARY\n')
        self.put('escape.cue',b'FILE "../outside.bin" BINARY\n')
        self.put('shared.bin',b'shared')
        self.put('a.cue',b'FILE "shared.bin" BINARY\n')
        self.put('b.cue',b'FILE "shared.bin" BINARY\n')
        self.assertTrue(all(r['status']=='unmatched' and not r['moves'] for r in self.scan()['entries']))

    def test_symlink_and_case_collision_rejected(self):
        outside=Path(self.temp.name)/'outside';outside.mkdir()
        (self.root/'Games').symlink_to(outside,target_is_directory=True)
        self.put('old.iso',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        self.assertEqual(self.scan()['entries'][0]['status'],'blocked')
        (self.root/'Games').unlink();(self.root/'games').mkdir()
        self.assertEqual(self.scan()['entries'][0]['status'],'blocked')
        for path in ('../outside.iso','/etc/passwd','Games/../bad','Games\\bad', 'Games/CON:'):
            with self.assertRaises(ValueError):organizer.relative_path(self.root,path)

    def test_custom_art_and_preview_associations_survive_move_and_undo(self):
        self.put('old.iso',b'payload')
        self.put('covers/custom.png',b'not read beyond header by indexer')
        self.put('previews/custom.3dv',b'video fixture')
        self.put('covers/index.3di',organizer.media_index({'old.iso':'custom.png'}))
        self.put('previews/index.3di',organizer.media_index({'old.iso':'custom.3dv'}))
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        before=snapshot(self.root)
        tx=self.apply(self.scan())
        key='games/usa/example (usa)/example (usa).iso'
        self.assertEqual(organizer.index_tools.existing_index(self.root/'covers')[key],'custom.png')
        self.assertEqual(organizer.index_tools.existing_index(self.root/'previews')[key],'custom.3dv')
        tx.undo();self.assertEqual(snapshot(self.root),before)

    def test_undo_refuses_later_edits_without_partial_restore(self):
        self.put('old.iso',b'payload')
        self.put('favorites.txt',b'old.iso\n')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        tx=self.apply(self.scan())
        self.put('favorites.txt',b'edited after organization\n')
        before=snapshot(self.root)
        with self.assertRaisesRegex(ValueError,'changed after'):tx.undo()
        self.assertEqual(before,snapshot(self.root))

    def test_apply_failure_automatically_restores_original_files(self):
        self.put('old.iso',b'payload')
        self.put('favorites.txt',b'old.iso\n')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        before=snapshot(self.root)
        with mock.patch.object(organizer,'write_database',side_effect=ValueError('synthetic failure')):
            with self.assertRaisesRegex(ValueError,'synthetic failure'):self.apply(self.scan())
        self.assertEqual(before,snapshot(self.root))

    def test_interrupted_move_reserved_destination_recovery(self):
        self.put('old.iso',b'payload')
        tx=organizer.Transaction(self.root,{})
        tx.data['moves']=[dict(source='old.iso',target='new.iso',fingerprint=organizer.fingerprint(self.root/'old.iso'),state='moving')]
        tx.save();self.put('new.iso',b'')
        tx.undo()
        self.assertTrue((self.root/'old.iso').exists());self.assertFalse((self.root/'new.iso').exists())

    def test_undo_resumes_after_source_has_already_been_restored(self):
        self.put('old.iso',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        tx=self.apply(self.scan())
        (self.root/'Games/USA/Example (USA)/Example (USA).iso').rename(self.root/'old.iso')
        tx.undo()
        self.assertEqual((self.root/'old.iso').read_bytes(),b'payload')

    def test_undo_refuses_modified_image_and_html_escapes_names(self):
        self.put('old.iso',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        plan=self.scan()
        report=copy.deepcopy(plan);report['entries'][0]['source']='<script>alert(1)</script>.iso'
        rendered=organizer.report_html(report).decode()
        self.assertNotIn('<script>alert(1)</script>',rendered)
        self.assertIn('&lt;script&gt;',rendered)
        tx=self.apply(plan)
        self.put('Games/USA/Example (USA)/Example (USA).iso',b'changed')
        before=snapshot(self.root)
        with self.assertRaisesRegex(ValueError,'contents changed'):tx.undo()
        self.assertEqual(before,snapshot(self.root))

    def test_catalog_dat_and_streaming_launchbox_adapter(self):
        text='clrmamepro ( name "3DO" ) game ( name "Example (USA)" region "USA" rom ( name "Example.bin" size 7 sha1 ABCD ) )'
        games=organizer.catalog_tools.dat_games(text)
        xml=b'''<LaunchBox><Game><Name>Example</Name><DatabaseID>42</DatabaseID><Platform>3DO Interactive Multiplayer</Platform><Genres>Adventure</Genres><Publisher>Publisher</Publisher><ReleaseYear>1995</ReleaseYear></Game><Game><Name>Other platform</Name><DatabaseID>99</DatabaseID><Platform>PC</Platform></Game><GameAlternateName><DatabaseID>42</DatabaseID><AlternateName>Alternate Example</AlternateName></GameAlternateName></LaunchBox>'''
        archive=self.cache/'metadata.zip'
        with zipfile.ZipFile(archive,'w') as z:z.writestr('Metadata.xml',xml)
        rows=organizer.catalog_tools.launchbox_games(archive)
        self.assertEqual(len(rows),1);self.assertEqual(rows[0]['aliases'],['Alternate Example'])
        enriched=organizer.catalog_tools.enrich(games,rows)
        self.assertEqual(enriched[0]['metadata']['year'],1995)
        self.assertEqual(enriched[0]['metadata']['genre'],'Adventure')

    def test_cached_cover_is_resized_and_undo_removes_only_generated_art(self):
        try:from PIL import Image
        except ImportError:self.skipTest('Pillow is optional outside the dev container')
        self.put('old.iso',b'payload')
        self.catalog['games']=[game('Example (USA)',[b'payload'])]
        plan=self.scan()
        name='Example (USA).png'
        key=hashlib.sha1(name.encode()).hexdigest()
        original=self.cache/'artwork'/f'{key}.png';original.parent.mkdir()
        Image.new('RGB',(300,400),(120,50,80)).save(original)
        (self.cache/'cover-list.html').write_text('<a href="Example%20%28USA%29.png">Example</a>')
        before=snapshot(self.root)
        tx=organizer.apply_plan(self.root,plan,self.cache,covers=True,offline=True)
        covers=list((self.root/'covers').glob('*.png'))
        self.assertEqual(len(covers),1)
        with Image.open(covers[0]) as im:self.assertEqual(im.size,(84,105));self.assertEqual(im.mode,'RGB')
        tx.undo();self.assertEqual(before,snapshot(self.root))

    def test_different_disc_cover_scans_do_not_overwrite_each_other(self):
        try:from PIL import Image
        except ImportError:self.skipTest('Pillow is optional outside the dev container')
        links=[]
        for disc in (1,2):
            data=f'disc {disc}'.encode()
            self.put(f'old{disc}.iso',data)
            title=f'Example (USA) (Disc {disc})'
            self.catalog['games'].append(game(title,[data]))
            name=title+'.png';key=hashlib.sha1(name.encode()).hexdigest()
            path=self.cache/'artwork'/f'{key}.png';path.parent.mkdir(exist_ok=True)
            Image.new('RGB',(84,105),(disc*80,20,30)).save(path)
            links.append('<a href="'+organizer.catalog_tools.urllib.parse.quote(name)+'">Cover</a>')
        (self.cache/'cover-list.html').write_text(''.join(links))
        before=snapshot(self.root)
        tx=organizer.apply_plan(self.root,self.scan(),self.cache,covers=True,offline=True)
        index=organizer.index_tools.existing_index(self.root/'covers')
        self.assertEqual(len(set(index.values())),2)
        tx.undo();self.assertEqual(before,snapshot(self.root))


if __name__=='__main__':unittest.main()
