#!/usr/bin/env python3
"""Incremental import and USB copy safety, using synthetic discs and artwork."""
import binascii
import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('maintenance',ROOT/'tools/library-maintenance.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def preview():
    frame=zlib.compress(b'\x7c\0'*5120)
    video=struct.pack('>II',8,8+len(frame))+frame
    return b'OM3DVID2'+struct.pack('>HHIIHHIIII',64,80,len(video),binascii.crc32(video),1,15,len(video),0,0,0)+video+b'END3DV02'


def snapshot(root):
    return {p.relative_to(root).as_posix():p.read_bytes() for p in root.rglob('*') if p.is_file() and 'transactions' not in p.parts}


class MaintenanceTest(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.base=Path(self.tmp.name);self.root=self.base/'staging';self.target=self.base/'USB'
        self.root.mkdir();self.target.mkdir();self.cache=self.base/'cache';self.report=self.base/'review.html'
        self.disc='Games/USA/Example (USA)/Example (USA).iso'
        self.put(self.disc,b'fictional disc bytes')
        self.put('.ohmy3do/ohmy3do.ini',(ROOT/'ohmy3do.ini').read_bytes())
        volume=bytearray(2048);volume[:7]=b'\x01ZZZZZ\x01';struct.pack_into('>II',volume,76,2048,1)
        self.put('boot.iso',volume)
        self.record=dict(path=self.disc,title='Example (USA)',region='USA',category='Games',category_review=False,
                         category_reason='Explicit category override',catalog_name=None,variant=None,edition_unresolved=False,
                         disc=None,identification='unverified',status='registered',metadata={'developer':'Café'},
                         cover=None,cover_source=None,preview=None,preview_source=None,suggestions=[],
                         registration={self.disc:{'sha1':'do not re-identify'}})
        self.put('.ohmy3do/library.json',m.o.json_bytes({'schema':1,'sources':[],'games':[self.record]}))
        m.o.index_tools.index(self.root)

    def put(self,name,data,root=None):
        p=(root or self.root)/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data);return p

    def art(self,directory,name='Example (USA).jpg',color='red'):
        from PIL import Image
        directory.mkdir(parents=True,exist_ok=True);p=directory/name
        Image.new('RGB',(200,300),color).save(p);return p

    def run_update(self,sources,apply=False):
        return m.update_media(self.root,sources,self.cache,self.report,apply)

    def test_media_preview_writes_contact_sheet_without_changing_library(self):
        source=self.art(self.base/'Box');before=snapshot(self.root)
        rows=self.run_update({'cover':[source],'preview':[]})
        self.assertEqual(rows[0]['cover']['status'],'matched')
        self.assertEqual(before,snapshot(self.root));self.assertFalse(self.cache.exists())
        self.assertIn('data:image/png;base64,',self.report.read_text(encoding='utf-8'))

    def test_incremental_import_preserves_identity_and_rerun_does_no_conversion(self):
        source=self.art(self.base/'Box');before=snapshot(self.root)
        with mock.patch.object(m.o,'scan_units',side_effect=AssertionError('No disc scan')):
            self.run_update({'cover':[source],'preview':[]},True)
        installed=json.loads((self.root/'.ohmy3do/library.json').read_text(encoding='utf-8'))['games'][0]
        self.assertEqual({k:v for k,v in installed.items() if not k.startswith('cover')},
                         {k:v for k,v in self.record.items() if not k.startswith('cover')})
        with mock.patch.object(m,'cached_conversion',side_effect=AssertionError('No reconversion')):
            self.run_update({'cover':[source],'preview':[]},True)
        journals=list((self.root/'.ohmy3do/transactions').glob('*.json'));self.assertEqual(len(journals),1)
        m.o.Transaction(self.root,path=journals[0].relative_to(self.root).as_posix()).undo()
        self.assertEqual(before,snapshot(self.root))

    def test_existing_custom_association_wins_over_source_and_database(self):
        source=self.art(self.base/'Box');self.put('.ohmy3do/covers/custom.png',b'leave existing media alone')
        self.put('.ohmy3do/covers/index.3di',m.o.media_index({self.disc:'custom.png'}))
        with mock.patch.object(m,'cached_conversion',side_effect=AssertionError('Existing art stays')):
            rows=self.run_update({'cover':[source],'preview':[]},True)
        self.assertEqual(rows[0]['cover']['file'],'custom.png')

    def test_missing_media_index_can_be_repaired_from_database_without_conversion(self):
        source=self.art(self.base/'Box')
        self.run_update({'cover':[source],'preview':[]},True)
        (self.root/'.ohmy3do/covers/index.3di').unlink()
        with mock.patch.object(m,'cached_conversion',side_effect=AssertionError('No conversion for repair')):
            self.run_update({'cover':[source],'preview':[]},True)
        mapping=m.o.index_tools.existing_index(self.root/'.ohmy3do/covers')
        self.assertIn(m.FOLD(self.disc),mapping)

    def test_preview_import_uses_fixed_fps_duration_and_audio_and_can_be_undone(self):
        clip=self.put('clips/Example (USA).mp4',b'synthetic source',self.base)
        before=snapshot(self.root)
        real=m.o.module
        calls=[]
        def module(name):
            result=real(name)
            if name=='prepare-preview':
                def convert(source,target,**kwargs):
                    calls.append(kwargs);target.write_bytes(preview())
                result.convert=convert
            return result
        with mock.patch.object(m.o,'module',side_effect=module):
            self.run_update({'cover':[],'preview':[clip]},True)
        self.assertEqual(calls,[dict(duration=15,fps=15,sound=True)])
        m.validate_media(next((self.root/'.ohmy3do/previews').glob('*.3dv')))
        journal=next((self.root/'.ohmy3do/transactions').glob('*.json'))
        m.o.Transaction(self.root,path=journal.relative_to(self.root).as_posix()).undo()
        self.assertEqual(before,snapshot(self.root))

    def test_ambiguous_or_wrong_region_media_is_not_imported(self):
        a=self.art(self.base/'a');b=self.art(self.base/'b');jp=self.art(self.base/'c','Example (Japan).jpg')
        rows=self.run_update({'cover':[a,b],'preview':[]},True)
        self.assertEqual(rows[0]['cover']['status'],'ambiguous')
        rows=self.run_update({'cover':[jp],'preview':[]},True)
        self.assertEqual(rows[0]['cover']['status'],'missing')
        self.assertFalse((self.root/'.ohmy3do/transactions').exists())

    def test_emumovies_chooses_one_video_quality_per_title(self):
        emu=self.base/'EmuMovies/Panasonic_3DO';self.art(emu/'Box')
        for folder in ('Video_MP4','Video_MP4_HI_QUAL','Video_MP4_HD'):
            self.put(folder+'/Example (USA).mp4',b'source',emu)
        sources=m.media_sources(emu.parent)
        self.assertEqual(sources['preview'],[emu/'Video_MP4_HD/Example (USA).mp4'])
        self.assertEqual(len(sources['cover']),1)

    def test_failed_cover_does_not_block_other_entries_or_create_association(self):
        source=self.put('bad/Example (USA).jpg',b'truncated jpeg',self.base)
        before=snapshot(self.root)
        rows=self.run_update({'cover':[source],'preview':[]},True)
        self.assertEqual(rows[0]['cover']['status'],'failed');self.assertEqual(before,snapshot(self.root))

    def test_cache_reuses_conversion_but_recovers_corrupt_cached_output(self):
        source=self.art(self.base/'Box')
        first=m.cached_conversion('cover',source,self.cache)
        real=m.o.module
        def module(name):
            result=real(name)
            if name=='resize-covers':result.convert=mock.Mock(side_effect=AssertionError('Cache must be used'))
            return result
        with mock.patch.object(m.o,'module',side_effect=module):
            self.assertEqual(first,m.cached_conversion('cover',source,self.cache))
        next(self.cache.glob('*.png')).write_bytes(b'broken')
        self.assertEqual(first,m.cached_conversion('cover',source,self.cache))

    def test_database_failure_rolls_back_new_media_and_index(self):
        source=self.art(self.base/'Box');before=snapshot(self.root)
        with mock.patch.object(m.o,'write_database',side_effect=ValueError('synthetic failure')):
            with self.assertRaisesRegex(ValueError,'synthetic'):
                self.run_update({'cover':[source],'preview':[]},True)
        self.assertEqual(before,snapshot(self.root))

    def test_prepare_preview_is_read_only_and_excludes_pc_files(self):
        self.put('.ohmy3do/recovery/original.bin',b'backup');self.put('.ohmy3do/ignored-cues/old.cue',b'old cue')
        self.put('.ohmy3do/media-import-report.json',b'PC report')
        before=snapshot(self.root)
        result=m.prepare_usb(self.root,self.target,self.base/'copy.json')
        self.assertEqual(snapshot(self.target),{});self.assertEqual(before,snapshot(self.root))
        names={r['path'] for r in result['files']}
        self.assertEqual(names,{'boot.iso',self.disc,'.ohmy3do/ohmy3do.ini','.ohmy3do/library.3di','.ohmy3do/metadata.3di'})

    def test_prepare_rejects_corrupt_metadata_without_copying(self):
        path=self.root/'.ohmy3do/metadata.3di'
        path.write_bytes(b'broken optional metadata')
        with self.assertRaisesRegex(ValueError,'metadata'):
            m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(snapshot(self.target),{})

    def test_settings_accept_gecko_tv_and_reject_unknown_themes(self):
        path=self.root/'.ohmy3do/ohmy3do.ini'
        original=path.read_bytes()
        path.write_bytes(original.replace(b'theme=0',b'theme=3'))
        m.validate_config(path)
        path.write_bytes(original.replace(b'theme=0',b'theme=4'))
        with self.assertRaisesRegex(ValueError,'Invalid menu setting'):
            m.validate_config(path)

    def test_copy_preserves_destination_preferences_and_saves_then_skips_unchanged(self):
        self.put('.ohmy3do/saves/backup.3ds',b'staging save')
        self.put('.ohmy3do/saves/backup.3ds',b'USB save',self.target)
        self.put('.ohmy3do/ohmy3do.ini',b'[display]\nscreen_margin=8\n',self.target)
        self.put('.ohmy3do/favorites.txt',b'USB favorites',self.target)
        result=m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(result['state'],'complete')
        self.assertEqual((self.target/'.ohmy3do/saves/backup.3ds').read_bytes(),b'USB save')
        self.assertIn(b'screen_margin=8',(self.target/'.ohmy3do/ohmy3do.ini').read_bytes())
        self.assertEqual((self.target/'.ohmy3do/favorites.txt').read_bytes(),b'USB favorites')
        self.assertEqual((self.target/self.disc).read_bytes(),b'fictional disc bytes')
        with mock.patch.object(m,'stream_copy',side_effect=AssertionError('Nothing should be copied')):
            m.prepare_usb(self.root,self.target,self.base/'copy2.json',True)

    def test_legacy_settings_are_preserved_without_shadowing_them(self):
        self.put('ohmy3do.ini',b'folder_workaround=1\n',self.target)
        m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertFalse((self.target/'.ohmy3do/ohmy3do.ini').exists())
        self.assertEqual((self.target/'ohmy3do.ini').read_bytes(),b'folder_workaround=1\n')

    def test_bad_disc_or_stale_find_blocks_copy_before_any_destination_changes(self):
        self.put(self.disc,b'PK\x03\x04archive disguised as ISO')
        with self.assertRaisesRegex(ValueError,'Disc problem'):
            m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(snapshot(self.target),{})
        self.put(self.disc,b'fictional disc bytes');self.put('Another.iso',b'new disc')
        with self.assertRaisesRegex(ValueError,'stale'):
            m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(snapshot(self.target),{})

    def test_conflicting_destination_disc_is_never_overwritten(self):
        self.put(self.disc,b'other disc',self.target);before=snapshot(self.target)
        with self.assertRaisesRegex(ValueError,'Different disc'):
            m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(before,snapshot(self.target))

    def test_verify_discs_detects_changes_with_matching_size_and_timestamp(self):
        src=self.root/self.disc;dest=self.put(self.disc,b'x'*src.stat().st_size,self.target)
        os.utime(dest,ns=(src.stat().st_atime_ns,src.stat().st_mtime_ns))
        ops,_=m.usb_plan(self.root,self.target)
        self.assertEqual(next(r for r in ops if r['path']==self.disc)['status'],'unchanged')
        with self.assertRaisesRegex(ValueError,'Different disc'):
            m.usb_plan(self.root,self.target,True)

    def test_verified_copy_receipt_handles_fat_timestamp_rounding_across_previews(self):
        report=self.base/'copy.json'
        result=m.prepare_usb(self.root,self.target,report,True)
        item=next(r for r in result['files'] if r['path']==self.disc)
        dest=self.target/self.disc
        rounded=(dest.stat().st_mtime_ns//2000000000)*2000000000
        os.utime(dest,ns=(rounded,rounded))
        # Simulate the actual post-copy timestamp recorded on a FAT drive.
        item['verified_copy']['destination_mtime_ns']=dest.stat().st_mtime_ns
        report.write_text(json.dumps(result),encoding='utf-8')
        real=m.sha256
        def no_disc_hash(path):
            if path.name.endswith('.iso') and path.name!='boot.iso':
                raise AssertionError('Verified unchanged FAT copy should be skipped')
            return real(path)
        with mock.patch.object(m,'sha256',side_effect=no_disc_hash):
            m.prepare_usb(self.root,self.target,report)
            m.prepare_usb(self.root,self.target,report,True)
        with self.assertRaisesRegex(AssertionError,'FAT copy'):
            with mock.patch.object(m,'sha256',side_effect=no_disc_hash):
                m.prepare_usb(self.root,self.target,report,verify=True)

    def test_only_indexed_media_is_copied_and_all_compressed_frames_are_validated(self):
        self.put('.ohmy3do/previews/good.3dv',preview());self.put('.ohmy3do/previews/unused.3dv',b'unused')
        self.put('.ohmy3do/previews/index.3di',m.o.media_index({self.disc:'good.3dv'}))
        files,_=m.usb_inventory(self.root)
        self.assertIn('.ohmy3do/previews/good.3dv',files);self.assertNotIn('.ohmy3do/previews/unused.3dv',files)
        data=bytearray(preview());data[48]=0
        struct.pack_into('>I',data,16,binascii.crc32(data[40:-8]))
        self.put('.ohmy3do/previews/good.3dv',data)
        with self.assertRaises((ValueError,zlib.error)):
            m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(snapshot(self.target),{})

    def test_runtime_replacement_is_backed_up_and_extra_destination_files_survive(self):
        old=bytearray((self.root/'boot.iso').read_bytes());old[120]=1
        self.put('boot.iso',old,self.target);self.put('keep.txt',b'untouched',self.target)
        result=m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual((Path(result['backup'])/'boot.iso').read_bytes(),old)
        self.assertEqual((self.target/'keep.txt').read_bytes(),b'untouched')

    def test_interrupted_copy_can_be_rerun_without_incomplete_files(self):
        real=m.stream_copy;calls=[]
        def fail(source,dest):
            calls.append(source)
            if len(calls)==2:raise OSError('synthetic interruption')
            real(source,dest)
        with mock.patch.object(m,'stream_copy',side_effect=fail):
            with self.assertRaisesRegex(OSError,'synthetic'):
                m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(json.loads((self.base/'copy.json').read_text(encoding='utf-8'))['state'],'interrupted')
        result=m.prepare_usb(self.root,self.target,self.base/'retry.json',True)
        self.assertEqual(result['state'],'complete');self.assertFalse(list(self.target.rglob('.oh3do-*')))

    def test_injected_readback_failure_keeps_original_destination(self):
        source=self.put('src.bin',b'new',self.base);dest=self.put('dest.bin',b'original',self.base)
        with mock.patch.object(m,'sha256',return_value='mismatched readback'):
            with self.assertRaisesRegex(ValueError,'verification failed'):m.stream_copy(source,dest)
        self.assertEqual(dest.read_bytes(),b'original');self.assertFalse(list(self.base.glob('.oh3do-*')))

    def test_paths_reports_and_nested_destinations_are_guarded(self):
        with self.assertRaises(ValueError):m.prepare_usb(self.root,self.root,self.base/'copy.json')
        with self.assertRaises(ValueError):m.prepare_usb(self.root,self.target,self.root/'report.json')
        with self.assertRaises(ValueError):m.outside(self.root/'cache',self.root)
        for name in ('../escape','Games/../escape','Games/Bad?.iso','Games/CON.iso','Games/trailing. '):
            with self.assertRaises(ValueError):m.checked_path(self.root,name)

    @unittest.skipIf(os.name=='nt','POSIX symlink fixture')
    def test_source_and_destination_links_cannot_escape_roots(self):
        outside=self.base/'outside';outside.mkdir()
        (self.target/'.ohmy3do').symlink_to(outside,target_is_directory=True)
        with self.assertRaises(ValueError):m.prepare_usb(self.root,self.target,self.base/'copy.json',True)
        self.assertEqual(list(outside.iterdir()),[])

    def test_native_png_checksum_and_dimensions_validation(self):
        source=self.art(self.base/'art');resizer=m.o.module('resize-covers');native=self.base/'native.png'
        resizer.convert(source,native);m.validate_media(native)
        data=bytearray(native.read_bytes());data[-5]^=1;native.write_bytes(data)
        with self.assertRaises(ValueError):m.validate_media(native)
        with self.assertRaises(ValueError):m.validate_media(source)

    def test_config_and_find_checksum_errors_are_reported(self):
        self.put('.ohmy3do/ohmy3do.ini',b'[display]\nscreen_margin=99\n')
        with self.assertRaisesRegex(ValueError,'Invalid menu setting'):m.usb_inventory(self.root)
        self.put('.ohmy3do/ohmy3do.ini',(ROOT/'ohmy3do.ini').read_bytes())
        p=self.root/'.ohmy3do/library.3di';data=bytearray(p.read_bytes());data[20]=1;p.write_bytes(data)
        with self.assertRaisesRegex(ValueError,'Find index'):m.usb_inventory(self.root)


if __name__=='__main__':unittest.main()
