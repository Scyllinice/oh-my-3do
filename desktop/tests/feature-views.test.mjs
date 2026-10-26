import { groupGames } from '../ui/model.mjs';
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';
import { escapeHtml as e, formatBytes as bytes, displayPath } from '../ui/model.mjs';
// Exercise the actual pure renderers without starting the Tauri window.
const source = readFileSync(new URL('../ui/app.js', import.meta.url), 'utf8');
// The source of a top-level function in app.js: from its `function name(`
// (or `async function name(`) line to the first closing brace at the start
// of a line.
function functionSource(name) {
  const lines = source.split('\n');
  const start = lines.findIndex(
    (l) => l.startsWith(`function ${name}(`) || l.startsWith(`async function ${name}(`),
  );
  assert.ok(start >= 0, `Missing function ${name}`);
  const end = lines.findIndex((l, i) => i >= start && l.startsWith('}'));
  return lines.slice(start, end + 1).join('\n');
}
function view(name, context = {}) {
  return runInNewContext(`${functionSource(name)}; ${name}`, {
    e,
    escapeHtml: e,
    bytes,
    pathLabel: (p) => e(displayPath(p)),
    action: (id, label, cls = '', extra = '') =>
      `<button data-action="${id}" ${extra}>${label}</button>`,
    ...context,
  });
}
test('readiness report stays visible for the requested path with trailing separators and escapes filesystem messages', () => {
  const state = {
    prefs: { destination: '/usb/' },
    readiness: {
      root: '/usb',
      requested_root: '/usb/',
      ready: false,
      errors: 1,
      warnings: 0,
      checks: [
        {
          name: 'Boot ISO',
          status: 'error',
          detail: '<script>bad path</script>',
          fix: 'Install menu',
        },
      ],
    },
  };
  const render = view('readinessView', { state });
  const html = render();
  assert.ok(html.includes('Drive needs attention'));
  assert.ok(html.includes('&lt;script&gt;'));
  assert.ok(!html.includes('<script>'));
  state.prefs.destination = '/another-drive';
  assert.equal(render(), '');
});
test('storage review displays category sizes and an explicit shortfall without negative free space', () => {
  const html = view('storageView')({
    games: 1024,
    covers: 20,
    previews: 40,
    support: 100,
    total: 1184,
    to_copy: 160,
    reserve: 100,
    required: 260,
    available: 250,
    remaining: 0,
    shortfall: 10,
    fits: false,
  });
  for (const text of [
    'Games',
    'Covers',
    'Previews',
    'Menu & support',
    '1.0 KiB',
    'Not enough room',
    '10 B',
    '0 B',
    'free space is needed',
  ])
    assert.ok(html.includes(text), text);
  assert.ok(!html.includes('-10'));
});
test('save comparison shows game names, metadata changes, absent bytes and only current saves get an editor shortcut', () => {
  const row = {
    game: 'DOOM',
    name: 'DoomPrefs',
    state: 'Changed',
    changed_bytes: 1,
    before_size: 40,
    after_size: 41,
    type_changed: true,
    name_changed: true,
    after_index: 2,
    changes: [[40, null, 255]],
  };
  const render = view('comparisonRow');
  const html = render(row);
  for (const text of [
    'DOOM',
    'DoomPrefs',
    'File type changed',
    'Filename changed',
    'data-index="2"',
    '0028',
    '-- → ff',
  ])
    assert.ok(html.includes(text), text);
  const removed = render({
    ...row,
    game: '<Unknown>',
    state: 'Removed',
    after_index: null,
    after_size: null,
  });
  assert.ok(!removed.includes('data-action="compare-edit"'));
  assert.ok(removed.includes('&lt;Unknown&gt;'));
  assert.ok(removed.includes('absent from the current draft'));
});

test('cleanup review starts with no selection and labels protected copies without exposing filenames as markup', () => {
  const dialogs = [];
  const state = {
    cleanupSelected: new Set(),
    cleanup: {
      directory: '/usb/saves',
      bytes: 64,
      copies: [
        {
          name: '<save>.3ds.uuid.bak',
          original: '<save>.3ds',
          bytes: 64,
          modified: 0,
          valid: true,
          newest: true,
          keep: true,
        },
      ],
    },
  };
  view('recoveryDialog', { state, dialog: (...args) => dialogs.push(args) })();
  const [title, html, buttons] = dialogs[0];
  assert.equal(title, 'Recovery copies');
  assert.ok(html.includes('0 selected'));
  assert.ok(html.includes('Newest copy'));
  assert.ok(html.includes('&lt;save&gt;'));
  assert.ok(!html.includes('<save>'));
  assert.ok(buttons.includes('disabled'));
});

test('presence and sizes aggregate across every disc, with unknown kept distinct', () => {
  const groups = groupGames([
    { path: 'Games/USA/Two discs/Disc 1.iso', title: 'Two discs', bytes: 100, on_drive: true },
    { path: 'Games/USA/Two discs/Disc 2.iso', title: 'Two discs', bytes: 200, on_drive: false },
  ]);
  assert.equal(groups.length, 1);
  assert.equal(groups[0].bytes, 300);
  assert.equal(groups[0].presentDiscs, 1);
  assert.equal(groups[0].presenceKnown, true);
  assert.equal(groupGames([{ path: 'Unknown.iso' }])[0].presenceKnown, false);
});

test('copy review uses readable nonzero file counts without repeating the transfer size', () => {
  const files = [
    ...Array.from({ length: 2399 }, () => ({ status: 'copy' })),
    ...Array.from({ length: 4 }, () => ({ status: 'preserve' })),
    { status: 'unchanged' },
    ...Array.from({ length: 3 }, () => ({ status: 'update' })),
  ];
  const html = view('copySummary', {
    quantity: (n, unit) => `${n.toLocaleString()} ${unit}${n === 1 ? '' : 's'}`,
  })({ games: 554, files, folders_to_move: 0 });
  for (const text of [
    '554 discs',
    `${(2407).toLocaleString()} files`,
    `${(2399).toLocaleString()} new files`,
    '3 files to update',
    '4 existing files kept',
    '1 file already up to date',
  ])
    assert.ok(html.includes(text), text);
  assert.ok(!html.includes('to move'));
  assert.ok(!html.includes('copy:'));
  assert.ok(!html.includes('GiB'));
});

test('update cards show independent versions and only relevant release notes', () => {
  const menu = { tag: 'menu-v1.2.0', notes: 'Menu only <notes>' };
  const desktop = { tag: 'desktop-v3.4.0', notes: 'Desktop only <notes>' };
  const state = {
    updates: {
      desktop: true,
      desktop_release: desktop,
      menu_release: menu,
      targets: [],
      warnings: [],
    },
  };
  const render = view('updateCards', { state, pathLabel: e });
  let html = render();
  assert.ok(html.includes('desktop-v3.4.0'));
  assert.ok(html.includes('Desktop only &lt;notes&gt;'));
  assert.ok(!html.includes('menu-v1.2.0'));
  assert.ok(!html.includes('Menu only'));
  state.updates.desktop = false;
  state.updates.targets = [{ root: '/card', installed: true }];
  html = render();
  assert.ok(html.includes('menu-v1.2.0'));
  assert.ok(html.includes('Menu only &lt;notes&gt;'));
  assert.ok(!html.includes('desktop-v3.4.0'));
  assert.ok(!html.includes('Desktop only'));
  state.updates.desktop = true;
  html = render();
  assert.ok(html.includes('desktop-v3.4.0'));
  assert.ok(html.includes('menu-v1.2.0'));
  state.updates.desktop = false;
  state.updates.targets = [];
  html = render();
  assert.ok(html.includes('up to date'));
  assert.ok(!html.includes('Release notes'));
});

test('update notices are dismissible, optional and never create a modal', () => {
  const state = {
    prefs: {},
    updates: {
      desktop: true,
      desktop_release: { tag: 'desktop-v1.0.3' },
      menu_release: { tag: 'menu-v1.0.1' },
      targets: [],
    },
  };
  const render = view('updateNotice', { state });
  assert.ok(render().includes('A desktop update is available.'));
  assert.ok(!render().includes('<dialog'));
  state.noticeDismissed = JSON.stringify(['desktop-v1.0.3', [], 'menu-v1.0.1']);
  assert.equal(render(), '');
  state.updates.desktop_release.tag = 'desktop-v1.0.4';
  assert.ok(render().includes('Review updates'));
  state.prefs.hide_update_notifications = true;
  assert.equal(render(), '');
  state.pendingUpdate = { version: '1.0.4<test>' };
  assert.ok(render().includes('1.0.4&lt;test&gt;'));
  state.updateReport = { success: false, previous_intact: false };
  assert.ok(render().includes('Recovery needs attention'));
  state.reportDismissed = true;
  state.pendingUpdate = null;
  assert.equal(render(), '');
});

test('installed version labels never reuse a stale or unrecognized menu version', () => {
  const state = {
    version: '1.0.2',
    prefs: { library: '/pc', destination: '/card', browser_drives: [] },
    installedMenu: { installed: true, version: '1.0.1' },
  };
  const render = view('installedVersionsView', {
    state,
    versionRoot: () => '/card',
    displayPath,
    option: (v, label) => `<option>${e(label)}</option>`,
  });
  assert.ok(render().includes('v1.0.2'));
  assert.ok(render().includes('v1.0.1'));
  state.installedMenu = { installed: true, version: null };
  assert.ok(render().includes('Version unknown'));
  assert.ok(!render().includes('v1.0.1'));
  state.installedMenu = { installed: false };
  assert.ok(render().includes('Not installed'));
  state.installedMenu = { error: '<unplugged>' };
  assert.ok(render().includes('Drive unavailable'));
  assert.ok(render().includes('&lt;unplugged&gt;'));
});

test('update recovery text distinguishes intact executables from unconfirmed recovery', () => {
  const state = {
    updateReport: { success: false, previous_intact: true, detail: 'Error <details>', log: '/log' },
  };
  const render = view('updateResultView', { state });
  assert.ok(render().includes('previous version is intact'));
  assert.ok(render().includes('Error &lt;details&gt;'));
  state.updateReport.previous_intact = false;
  assert.ok(render().includes('Recovery could not be confirmed'));
  assert.ok(!render().includes('version is intact'));
  state.updateReport.success = true;
  assert.ok(render().includes('startup check'));
});

test('automatic update checks leave the active page and dialogs alone', async () => {
  const line = functionSource('checkUpdates');
  const state = { page: 'saves' };
  let refreshed = 0,
    notices = 0;
  const check = runInNewContext(`${line};checkUpdates`, {
    state,
    versionRoot: () => '/card',
    call: async () => ({ desktop: true, targets: [] }),
    refreshInstalledVersions: async () => {
      refreshed++;
    },
    renderUpdateNotice: () => {
      notices++;
    },
    render: () => {
      throw Error('Active editor must not be rerendered');
    },
    dialog: () => {
      throw Error('No modal');
    },
    toast: () => {
      throw Error('No toast for an available update');
    },
    recordActivity: () => {},
  });
  await check(true);
  assert.equal(state.page, 'saves');
  assert.equal(refreshed, 1);
  assert.equal(notices, 1);
  assert.equal(state.updates.desktop, true);
});

test('scheduling an update preserves open edits and does not quit', async () => {
  const line = functionSource('prepareDesktopUpdate');
  const state = {
    updates: { desktop_release: { tag: 'desktop-v1.0.3' } },
    backup: { dirty: true },
  };
  const calls = [];
  const prepare = runInNewContext(`${line};prepareDesktopUpdate`, {
    state,
    prepareClose: () => {
      throw Error('Downloading must not close editors');
    },
    confirmAction: async () => true,
    call: async (action, args) => {
      calls.push([action, args.on_exit]);
      return { version: '1.0.3' };
    },
    closeDialog: () => {},
    render: () => {},
    toast: () => {},
  });
  await prepare(true);
  assert.deepEqual(calls, [['self_update', true]]);
  assert.equal(state.pendingUpdate.version, '1.0.3');
  assert.equal(state.backup.dirty, true);
});

test('local desktop-only offers render without menu metadata and remain explicit about the source', () => {
  const state = {
    prefs: { developer_mode: true },
    updates: {
      local: true,
      desktop: true,
      desktop_release: { tag: 'desktop-v1.0.2', local: true, notes: 'Directory: <build>' },
      menu_release: null,
      targets: [],
      warnings: [],
    },
  };
  const html = view('updateCards', { state })();
  assert.ok(html.includes('verified local package'));
  assert.ok(html.includes('Local build'));
  assert.ok(html.includes('&lt;build&gt;'));
  assert.ok(!html.includes('GitHub'));
  assert.ok(
    view('updateNotice', { state })().includes('Local build: A desktop update is available.'),
  );
  state.updates = null;
  assert.ok(view('updateCards', { state })().includes('local build directory'));
});
test('developer settings expose the source directory safely and local checks stay enabled offline', () => {
  const state = {
    prefs: { developer_mode: true, developer_updates_directory: 'C:\\<build>', offline: true },
  };
  const html = view('developerSettingsView', {
    state,
    checkbox: () => '<input type="checkbox" checked>',
  })();
  assert.ok(html.includes('C:\\&lt;build&gt;'));
  assert.ok(html.includes('same version'));
  assert.ok(html.includes('never fall back to GitHub'));
  const render = view('updatesView', {
    state,
    heading: (title, note, actions) => e(note) + actions,
    installedVersionsView: () => '',
    updateResultView: () => '',
    pendingUpdateView: () => '',
    updateCards: () => '',
  });
  assert.ok(!render().includes('disabled'));
  assert.ok(render().includes('local builds from'));
  state.prefs.developer_mode = false;
  assert.ok(render().includes('disabled'));
});

test('boot updates keep the Updates page open for the other destination', async () => {
  const line = functionSource('installBootUpdate');
  const state = {
    page: 'library',
    updates: {
      menu_release: { tag: 'menu-v1.0.2' },
      targets: [{ root: '/library' }, { root: '/card' }],
    },
  };
  let selected,
    closed = 0;
  const rendered = [];
  const install = runInNewContext(`${line};installBootUpdate`, {
    state,
    displayPath,
    inspectWriteDrive: async (root) => ({ root, token: 'identity' }),
    confirmAction: async () => true,
    call: async (action, args) => {
      assert.equal(action, 'boot_update');
      selected = args.root;
    },
    closeDialog: () => {
      closed++;
    },
    render: () => rendered.push(state.page),
    checkUpdates: async () => {
      assert.equal(state.page, 'updates');
      state.updates.targets = state.updates.targets.filter((t) => t.root !== selected);
    },
    toast: () => {},
  });
  await install({ dataset: { root: '/library' } });
  assert.equal(closed, 1);
  assert.deepEqual(rendered, ['updates']);
  assert.deepEqual(state.updates.targets, [{ root: '/card' }]);
  await install({ dataset: { root: '/card' } });
  assert.equal(state.page, 'updates');
  assert.equal(state.updates.targets.length, 0);
});
test('failed boot updates leave the current page and update offers available', async () => {
  const line = functionSource('installBootUpdate');
  const state = {
    page: 'library',
    updates: { menu_release: { tag: 'menu-v1.0.2' }, targets: [{ root: '/card' }] },
  };
  const install = runInNewContext(`${line};installBootUpdate`, {
    state,
    displayPath,
    inspectWriteDrive: async (root) => ({ root, token: 'identity' }),
    confirmAction: async () => true,
    call: async () => {
      throw Error('Write failed');
    },
    closeDialog: () => {
      throw Error('Must keep failure visible');
    },
    render: () => {
      throw Error('Must keep current page');
    },
  });
  await assert.rejects(install({ dataset: { root: '/card' } }), /Write failed/);
  assert.equal(state.page, 'library');
  assert.equal(state.updates.targets.length, 1);
});

test('import review shows an empty category explicitly and disables unresolved selection', () => {
  const state = {
    prefs: { library: '/library' },
    paths: ['/incoming/example.cue'],
    plan: {
      entries: [
        {
          title: 'Example',
          source: '/incoming/example.cue',
          category: '',
          target: '',
          selected: false,
          blocked: false,
          reason: 'Choose category',
          confidence: 'unmatched',
        },
      ],
    },
  };
  const ready = runInNewContext(
    `${functionSource('importReady')};importReady`,
  );
  const start = source.indexOf('function importView('),
    end = source.indexOf('\nfunction savesView(', start);
  const render = runInNewContext(`${source.slice(start, end)};importView`, {
    state,
    e,
    escapeHtml: e,
    bytes,
    pathLabel: e,
    importReady: ready,
    categories: ['Games', 'Bonus Discs', 'Music'],
    option: (value, label, selected) =>
      `<option value="${e(value)}" ${selected ? 'selected' : ''}>${e(label)}</option>`,
    heading: () => '',
    action: () => '',
    checkbox: () => '',
  });
  let html = render();
  assert.match(html, /<option value="" selected disabled>Choose a category…/);
  assert.match(html, /data-import-check="0"[^>]*disabled/);
  const entry = state.plan.entries[0];
  entry.category = 'Music';
  entry.target = 'Music/Unknown/Example/Example.cue';
  entry.confidence = 'unverified';
  html = render();
  assert.match(html, /<option value="Music" selected>/);
  assert.ok(!/data-import-check="0"[^>]*disabled/.test(html));
  entry.blocked = true;
  html = render();
  assert.match(html, /data-import-check="0"[^>]*disabled/);
});

test('PC libraries and USB drives are separate lists that grow, show connection and end with add and manage', () => {
  const state = {
    prefs: {
      library: '/pc/3DO',
      destination: '/media/card',
      libraries: ['/pc/3DO'],
      browser_drives: [],
      drives: { '/media/card': { name: 'LOCODE' } },
    },
    driveStatus: {},
  };
  const option = (v, label, selected) =>
    `<option value="${e(v)}"${selected ? ' selected' : ''}>${e(label)}</option>`;
  const context = { state, option, displayPath };
  const rootName = view('rootName', context);
  const driveName = view('driveName', context);
  const remember = view('rememberRoots', context);
  remember();
  assert.equal(JSON.stringify(state.prefs.browser_drives), JSON.stringify(['/media/card']));
  state.prefs.library = '/pc/Japan';
  remember();
  remember();
  assert.equal(JSON.stringify(state.prefs.libraries), JSON.stringify(['/pc/3DO', '/pc/Japan']));
  const libraries = view('pcLibraryOptions', { ...context, rootName })();
  assert.ok(libraries.includes('value="/pc/Japan" selected'));
  assert.ok(libraries.includes('3DO — /pc/3DO'));
  assert.ok(libraries.includes('Add a PC library…'));
  assert.ok(libraries.includes('Manage libraries…'));
  state.driveStatus = { '/media/card': { connected: false, removable: false } };
  const drives = view('driveOptions', { ...context, driveName })();
  assert.ok(drives.includes('LOCODE — /media/card (not connected)'));
  assert.ok(drives.includes('Add a drive…'));
  assert.ok(drives.includes('Manage drives…'));
  state.prefs.destination = '';
  assert.ok(view('driveOptions', { ...context, driveName })().includes('Choose a USB / SD drive'));
});

test('the view switch shows which list is on screen and needs a drive for the drive view', () => {
  const state = { browseRoot: '', prefs: { destination: '' } };
  const html = view('viewSwitch', { state })();
  assert.ok(html.includes('data-action="view-pc" aria-pressed="true"'));
  assert.ok(html.includes('aria-pressed="false" disabled'));
  state.browseRoot = '/media/card';
  state.prefs.destination = '/media/card';
  const drive = view('viewSwitch', { state })();
  assert.ok(drive.includes('data-action="view-drive" aria-pressed="true"'));
  assert.ok(!drive.includes('disabled'));
});
