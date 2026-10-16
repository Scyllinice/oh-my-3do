#!/usr/bin/env python3
"""Organizer integration tests use tiny synthetic discs, never commercial images."""
import base64
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sqlite3
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
        with sqlite3.connect(self.root/'.ohmy3do/library.sqlite') as db:
            self.assertEqual(db.execute('select path,genre,publisher,year from games').fetchone(),(target,'Adventure','Example Publisher',1995))
        journal=tx.path.relative_to(self.root).as_posix()
        organizer.Transaction(self.root,path=journal).undo()
        self.assertEqual(before,snapshot(self.root))

    def test_six_categories_move_to_separate_roots_and_undo(self):
        cases=[('Example (USA)','Adventure','Games'),('Example (USA) (Demo)','Adventure','Demos'),
               ('Interactive Sampler CD (USA)','Compilation','Samplers'),
               ('Storage Manager (USA)',None,'Applications'),('Video Almanac (USA)',None,'Multimedia'),
               ('Learning Numbers (USA)','Education; Puzzle','Educational')]
        for i,(title,genre,category) in enumerate(cases):
            data=f'disc {i}'.encode();self.put(f'old{i}.iso',data)
            row=game(title,[data]);row['metadata']['genre']=genre
            self.catalog['games'].append(row)
        before=snapshot(self.root)
        plan=self.scan()
        self.assertEqual({r['category'] for r in plan['entries']},set(organizer.CATEGORIES))
        self.assertTrue(all(r['status']=='ready' for r in plan['entries']))
        tx=self.apply(plan)
        with sqlite3.connect(self.root/'.ohmy3do/library.sqlite') as db:
            self.assertEqual({row[0] for row in db.execute('select category from games')},set(organizer.CATEGORIES))
        for row in plan['entries']:self.assertTrue((self.root/row['target']).is_file())
        self.assertTrue(all(r['source']==r['target'] for r in self.scan()['entries']))
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
        with sqlite3.connect(self.root/'.ohmy3do/library.sqlite') as db:
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
        with sqlite3.connect(self.root/'.ohmy3do/library.sqlite') as db:
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
