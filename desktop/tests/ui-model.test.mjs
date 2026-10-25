import test from 'node:test';
import assert from 'node:assert/strict';
import {
  groupGames,
  selectedDiscs,
  visibleGames,
  escapeHtml,
  slotMatches,
  displayPath,
  categories,
} from '../ui/model.mjs';
test('multi-disc games copy as one selection, unrelated loose discs remain independent', () => {
  const groups = groupGames([
    { path: 'Games/USA/Example/Disc 1.iso', title: 'Example (Disc 1)', region: 'USA' },
    { path: 'Games/USA/Example/Disc 2.cue', title: 'Example (Disc 2)', region: 'USA' },
    { path: 'Games/one.iso', title: 'One' },
    { path: 'Games/two.iso', title: 'Two' },
  ]);
  assert.equal(groups.length, 3);
  assert.equal(groups[0].title, 'Example');
  assert.deepEqual(selectedDiscs(groups, new Set(['Games/USA/Example'])), [
    'Games/USA/Example/Disc 1.iso',
    'Games/USA/Example/Disc 2.cue',
  ]);
  assert.equal(groups.filter((g) => !g.movable).length, 2);
});
test('filtering selects precisely the shown titles without discarding other selections', () => {
  const groups = groupGames([
    { path: 'Games/USA/Example/one.iso', title: 'Example', region: 'USA' },
    { path: 'Adult/Japan/Other/two.iso', title: 'Other', region: 'Japan' },
    { path: 'Bonus Discs/USA/Bonus/bonus.iso', title: 'Bonus', region: 'USA' },
  ]);
  const selected = new Set(['Adult/Japan/Other']);
  for (const g of visibleGames(groups, { search: 'exam', category: 'Games', region: 'USA' }))
    selected.add(g.key);
  assert.equal(selectedDiscs(groups, selected).length, 2);
  assert.equal(groups.find((g) => g.title === 'Bonus').category, 'Bonus Discs');
});
test('slot searches use known titles, raw filenames and labels across all slots', () => {
  const slot = {
    name: 'Slot 102 — Finished',
    games: 'DOOM',
    files: ['DoomPrefs'],
    search: 'DOOM 3DO',
  };
  assert.ok(slotMatches(slot, 'doom finished'));
  assert.ok(slotMatches(slot, 'DoomPrefs 102'));
  assert.equal(slotMatches(slot, 'doom missing'), false);
});
test('filesystem names and release notes cannot inject markup', () => {
  assert.equal(
    escapeHtml('<img src=x onerror="alert(1)"> &'),
    '&lt;img src=x onerror=&quot;alert(1)&quot;&gt; &amp;',
  );
});

test('Windows path labels hide drive and UNC prefixes without altering other namespaces', () => {
  assert.equal(displayPath(String.raw`\\?\D:\3DO`), String.raw`D:\3DO`);
  assert.equal(displayPath(String.raw`\\?\UNC\server\share\3DO`), String.raw`\\server\share\3DO`);
  for (const path of [
    String.raw`D:\3DO`,
    String.raw`\\server\share`,
    String.raw`\\?\Volume{123}\file`,
    '/mnt/d/3DO',
  ])
    assert.equal(displayPath(path), path);
});

test('configured categories group custom multi-disc titles and remain selectable when hidden on console', () => {
  const old = [...categories];
  try {
    categories.splice(0, categories.length, 'My collection');
    const groups = groupGames([
      {
        path: 'My collection/USA/Game/Disc 1.iso',
        title: 'Game (Disc 1)',
        category: 'My collection',
      },
      {
        path: 'My collection/USA/Game/Disc 2.iso',
        title: 'Game (Disc 2)',
        category: 'My collection',
      },
    ]);
    assert.equal(groups.length, 1);
    assert.equal(groups[0].movable, true);
    assert.equal(selectedDiscs(groups, new Set([groups[0].key])).length, 2);
    assert.equal(visibleGames(groups, { category: 'My collection' }).length, 1);
  } finally {
    categories.splice(0, categories.length, ...old);
  }
});

test('saved library views restore valid selections and filters while dropping removed games/categories/regions', async () => {
  const { restoreLibraryView } = await import('../ui/model.mjs');
  const groups = [
    { key: 'Games/USA/Doom', region: 'USA' },
    { key: 'Demos/Japan/Test', region: 'Japan' },
  ];
  const saved = {
    selected: ['Games/USA/Doom', 'Missing', 'Games/USA/Doom'],
    search: 'doom',
    category: 'Games',
    region: 'USA',
    media: '',
    information: '',
    searchScope: 'title',
  };
  assert.deepEqual(restoreLibraryView(saved, groups, ['Games', 'Demos']), {
    selected: ['Games/USA/Doom'],
    search: 'doom',
    category: 'Games',
    region: 'USA',
    media: '',
    information: '',
    searchScope: 'title',
  });
  assert.deepEqual(restoreLibraryView(saved, [], ['New category']), {
    selected: [],
    search: 'doom',
    category: '',
    region: '',
    media: '',
    information: '',
    searchScope: 'title',
  });
  assert.deepEqual(restoreLibraryView(null, groups, ['Games']), {
    selected: [],
    search: '',
    category: '',
    region: '',
    media: '',
    information: '',
    searchScope: 'title',
  });
});
test('saved views remain independent between libraries', async () => {
  const { restoreLibraryView } = await import('../ui/model.mjs');
  const views = {
    A: { selected: ['A/Game'], search: 'alpha', category: 'Games' },
    B: { selected: ['B/Game'], search: 'beta', region: 'Japan' },
  };
  assert.deepEqual(
    restoreLibraryView(views.A, [{ key: 'A/Game', region: 'USA' }], ['Games']).selected,
    ['A/Game'],
  );
  const b = restoreLibraryView(views.B, [{ key: 'B/Game', region: 'Japan' }], ['Games']);
  assert.deepEqual(b.selected, ['B/Game']);
  assert.equal(b.search, 'beta');
  assert.equal(b.region, 'Japan');
});

test('missing media filters include partially populated multi-disc titles and combine with search', () => {
  const groups = groupGames([
    { path: 'Games/USA/Complete/disc.iso', title: 'Complete', cover: 'a.png', preview: 'a.3dv' },
    {
      path: 'Games/USA/Partial/Disc 1.iso',
      title: 'Partial (Disc 1)',
      cover: 'b.png',
      preview: 'b.3dv',
      cover_missing: false,
      preview_missing: false,
    },
    {
      path: 'Games/USA/Partial/Disc 2.iso',
      title: 'Partial (Disc 2)',
      cover: 'c.png',
      preview: 'c.3dv',
      cover_missing: true,
      preview_missing: false,
    },
    { path: 'Games/USA/No video/disc.iso', title: 'No video', cover: 'd.png' },
    { path: 'Games/USA/Neither/disc.iso', title: 'Neither' },
  ]);
  const titles = (filter) =>
    visibleGames(groups, filter)
      .map((g) => g.title)
      .sort();
  assert.deepEqual(titles({ media: 'cover' }), ['Neither', 'Partial']);
  assert.deepEqual(titles({ media: 'preview' }), ['Neither', 'No video']);
  assert.deepEqual(titles({ media: 'either' }), ['Neither', 'No video', 'Partial']);
  assert.deepEqual(titles({ media: 'either', search: 'partial' }), ['Partial']);
  assert.equal(visibleGames(groups).length, 4);
});
test('missing media preference survives restoration and invalid values reset', async () => {
  const { restoreLibraryView } = await import('../ui/model.mjs');
  assert.equal(restoreLibraryView({ media: 'either' }, [], []).media, 'either');
  assert.equal(restoreLibraryView({ media: 'unknown' }, [], []).media, '');
});
