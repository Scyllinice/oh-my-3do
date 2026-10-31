// The desktop app's interface.
//
// How it works:
//
// - `state` holds everything shown: the current page, the library's
//   records, the selection, open reviews and dialogs, and the preferences.
// - `render()` redraws the current page from `state` by building its HTML
//   (each page has a `...View()` function returning a string). Dialogs are
//   drawn the same way with `dialog()`.
// - Buttons carry a `data-action` name. One click listener looks the name up
//   in `handlers` and runs it, so each button's behavior is in one place.
// - `call(action, args)` asks the native backend (desktop/shell/src/bridge.rs)
//   to do something. Only one call runs at a time; its progress shows in the
//   status bar or in the open dialog.
//
// Small pure helpers (grouping discs into games, filtering, formatting) are
// in model.mjs; keyboard focus and screen reader support in
// accessibility.mjs; dialog progress panels in dialog-progress.mjs.

import {
  categories,
  groupGames,
  visibleGames,
  selectedDiscs,
  escapeHtml,
  formatBytes as bytes,
  slotMatches,
  displayPath,
  restoreLibraryView,
} from './model.mjs';
import { DialogProgress } from './dialog-progress.mjs';
import {
  captureFocus,
  restoreFocus,
  focusElement,
  focusHeading,
  announce,
  moveGameFocus,
  trapDialogTab,
  textScale,
} from './accessibility.mjs';

const dialogProgress = new DialogProgress(document);
const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

/** The first element matching a CSS selector. */
const $ = (selector) => document.querySelector(selector);

/** Everything the interface shows. */
const state = {
  // The page shown (a key of `titles`).
  page: 'library',
  prefs: {},
  version: '1.0.0',
  // The library's (or drive's) disc records, and the same discs grouped
  // into games (a multi-disc game is one group).
  records: [],
  groups: [],
  // The keys of the selected games.
  selected: new Set(),
  libraryRoot: null,
  // The drive being browsed, or '' for the PC library.
  browseRoot: '',
  // Each saved drive's connection status, by path.
  driveStatus: {},
  browser: null,
  browserError: '',
  // Recovery copies found for cleanup, and those chosen.
  cleanup: null,
  cleanupSelected: new Set(),
  filter: {
    search: '',
    category: '',
    region: '',
    media: '',
    information: '',
    searchScope: 'title',
  },
  // Files and folders chosen for import, and the scanned import plan.
  paths: [],
  plan: null,
  // The copy review, and the drive readiness check.
  copy: null,
  readiness: null,
  // The open save backup, the save shown, and whether its hex is edited.
  backup: null,
  entry: null,
  entryIndex: null,
  hexDirty: false,
  slots: [],
  slotQuery: '',
  // Console settings being edited.
  config: null,
  // The last update check, a downloaded update waiting for exit, and the
  // last update's outcome.
  updates: null,
  pendingUpdate: null,
  updateReport: null,
  installedMenu: null,
  versionTarget: '',
  noticeDismissed: '',
  reportDismissed: false,
  // The cheat database editor's text, and whether it has unsaved edits.
  cheats: '',
  cheatsDirty: false,
  // Activity messages, newest last.
  logs: [],
  busy: false,
  // The game a Shift+click selection range starts from.
  anchor: null,
  // Cover thumbnails already loaded, by name.
  coverCache: new Map(),
  mediaDisc: '',
};

/** Each page's icon in the sidebar. */
const icons = {
  library: '▦',
  import: '＋',
  saves: '▤',
  media: '▧',
  drive: '▱',
  settings: '⚙',
  updates: '↻',
};

/** Each page's title. */
const titles = {
  library: 'Library',
  import: 'Add discs',
  saves: 'Save Manager',
  media: 'Artwork & previews',
  drive: 'Drive setup',
  settings: 'Settings & tools',
  updates: 'Updates',
};

// ---- Small HTML builders ----

/** A button that runs the handler named `id` (see `handlers`). */
const action = (id, label, cls = '', extra = '') =>
  `<button data-action="${id}" class="${cls}" ${extra}>${label}</button>`;

/** A path as shown to the person, escaped for HTML. */
const pathLabel = (value) => escapeHtml(displayPath(value));

const option = (value, label, selected) =>
  `<option value="${escapeHtml(value)}" ${selected ? 'selected' : ''}>${escapeHtml(label)}</option>`;

/** A labeled text (or other `type`) input. */
const field = (label, id, value, type = 'text') =>
  `<label class="field">${escapeHtml(label)}<input id="${id}" type="${type}" value="${escapeHtml(value)}"></label>`;

const checkbox = (id, label, checked) =>
  `<label><input id="${id}" type="checkbox" ${checked ? 'checked' : ''}>${escapeHtml(label)}</label>`;

/** A page's title and description, with its buttons on the right. */
const heading = (title, note, actions = '') =>
  [
    '<div class="page-heading row spread">',
    `<div><h1 id="page-title">${escapeHtml(title)}</h1><p>${escapeHtml(note)}</p></div>`,
    `<div class="row">${actions}</div>`,
    '</div>',
  ].join('');

/** "1 game", "1,234 games". */
const quantity = (count, unit) =>
  `${Number(count).toLocaleString('en-US')} ${unit}${count === 1 ? '' : 's'}`;

/** The last part of a path. */
const basename = (path) =>
  String(path || '')
    .split(/[\\/]/)
    .at(-1);

/** The console settings a new drive starts with (see console_config.rs). */
const defaults = {
  folder_workaround: 0,
  usb_covers: true,
  screen_margin: 0,
  large_titles: false,
  remember_position: true,
  recent_games: true,
  video_previews: true,
  preview_sound: true,
  hide_adult: false,
};

// ---- Status, messages and backend calls ----

/** Shows a message in the status bar (and the open dialog's progress). */
function setStatus(message) {
  dialogProgress.update(message);
  const status = $('#status-message');
  if (status) {
    status.textContent = message;
    status.title = message;
  }
}

/**
 * Shows a short message that disappears by itself (errors stay longer),
 * and announces it to screen readers. With a dialog open it appears inside
 * the dialog; if the dialog's progress panel already shows the result, the
 * panel shows the message instead.
 */
function toast(message, error = false) {
  const text = String(message);
  announce(message, error);
  document.querySelectorAll('.toast,.dialog-toast').forEach((node) => node.remove());
  const front = [...document.querySelectorAll('dialog[open]')].at(-1);
  const node = document.createElement('div');
  node.className = `${front ? 'dialog-toast' : 'toast'}${error ? ' error' : ''}`;
  node.textContent = text;

  const panel = front?.querySelector('.dialog-progress');
  if (panel?.dataset.state === 'complete') {
    panel.querySelector('[role="status"]').textContent = text;
    if (error) panel.dataset.state = 'error';
    setStatus(text);
    return;
  }
  const panelShowsError =
    panel?.dataset.state === 'error' && panel.querySelector('[role="status"]').textContent === text;
  if (panelShowsError) {
    setStatus(text);
    return;
  }

  if (front) front.querySelector('.dialog-head').after(node);
  else document.body.append(node);
  setTimeout(() => node.remove(), error ? 15000 : 5500);
  setStatus(text);
}

/** What the status bar says while each backend action runs, and after. */
const operationMessages = {
  titles_remove_review: ['Reviewing selected title files…', 'Title removal ready to review.'],
  titles_remove: ['Removing selected titles…', 'Selected titles removed.'],
  library: ['Refreshing the library…', 'Library refreshed.'],
  drive_browser: ['Reading games on the drive…', 'Drive contents refreshed.'],
  scan: ['Identifying discs…', 'Disc scan complete. Review the import.'],
  import: ['Importing selected discs…', 'Disc import complete.'],
  copy_preview: ['Checking destination files and space…', 'Copy review ready.'],
  copy: ['Copying selected games and support files…', 'Copy complete.'],
  drive_setup: ['Preparing the console drive…', 'Drive setup complete.'],
  drive_check: ['Checking the console drive…', 'Drive check complete.'],
  drive_cleanup_review: ['Finding menu files on the drive…', 'Drive cleanup ready to review.'],
  drive_cleanup: ['Removing menu files from the drive…', 'Menu files removed.'],
  drive_eject: ['Safely ejecting the drive…', 'Drive ejected.'],
  media_update: ['Preparing missing media and game information…', 'Media preparation complete.'],
  media_selected: ['Preparing media for selected games…', 'Selected media preparation complete.'],
  media_rebuild: ['Rebuilding media from source files…', 'Media rebuild complete.'],
  game_info_matches: ['Reading LaunchBox matches…', 'Information matches ready to review.'],
  game_info_choose_match: ['Saving the chosen information match…', 'Information match saved.'],
  game_info_backups: ['Reading automatic metadata backups…', 'Metadata backup list ready.'],
  game_info_backup_preview: [
    'Comparing the snapshot with your library…',
    'Snapshot restore ready to review.',
  ],
  game_info_export: ['Exporting custom metadata…', 'Custom metadata exported.'],
  game_info_import_preview: [
    'Comparing imported metadata with your library…',
    'Metadata import ready to review.',
  ],
  game_info_import: ['Importing checked metadata entries…', 'Custom metadata imported.'],
  game_info_restore_preview: [
    'Reading downloaded information…',
    'Downloaded information ready to review.',
  ],
  game_info_save: ['Saving game information…', 'Game information saved.'],
  game_information: ['Updating game information from LaunchBox…', 'Game information updated.'],
  categories_save: ['Saving categories and folder changes…', 'Categories saved.'],
  move: ['Moving selected games…', 'Selected games moved.'],
  indexes: ['Rebuilding console indexes…', 'Console indexes rebuilt.'],
  undo: ['Restoring the previous library state…', 'Last change undone.'],
  backup_open: ['Opening the save archive…', 'Save archive opened.'],
  backup_save: ['Saving and verifying the save archive…', 'Save archive saved and verified.'],
  backup_compare: ['Comparing save archives…', 'Save comparison ready.'],
  updates_check: ['Checking GitHub for updates…', 'Update check complete.'],
  boot_update: ['Downloading and installing the menu image…', 'Menu image updated.'],
  self_update: ['Downloading the desktop update…', 'Desktop update prepared.'],
  video_tools: ['Downloading and verifying video tools…', 'Video tools installed.'],
  support_export: ['Writing the support report…', 'Support report exported.'],
};

/** Backend actions too quick or quiet to show progress for. */
const quietActions = ['pick', 'preferences', 'init', 'ready', 'quit', 'drive_status'];

/** Adds a line to Activity (keeping the last 500). */
function recordActivity(message) {
  state.logs.push(String(message));
  if (state.logs.length > 500) state.logs.shift();
}

/** Whether an error means the person canceled the operation. */
function wasCanceled(error) {
  return /\boperation cancel(?:led|ed)\b/i.test(String(error));
}

/**
 * Asks the backend to run `action` with `args` and returns its result.
 * Shows progress while it runs and the outcome after (also in Activity).
 * Errors are thrown on to the caller.
 */
async function call(action, args = {}) {
  const messages = operationMessages[action];
  const job = quietActions.includes(action)
    ? null
    : dialogProgress.start(messages?.[0] || 'Working…');
  if (messages && !state.busy) {
    recordActivity(messages[0]);
    setStatus(messages[0]);
  } else if (job && !messages) setStatus('Working…');

  try {
    const result = await invoke('request', { action, args, requestId: job?.id || null });
    dialogProgress.finish(job, messages?.[1] || 'Operation complete.');
    if (messages) {
      recordActivity(messages[1]);
      setStatus(messages[1]);
    } else if (job) setStatus('Ready');
    return result;
  } catch (error) {
    const canceled = wasCanceled(error);
    dialogProgress.finish(job, String(error), canceled ? 'canceled' : 'error');
    recordActivity(String(error));
    if (!state.busy)
      setStatus(
        canceled
          ? 'Operation canceled.'
          : 'Could not complete the operation. Open Activity for details.',
      );
    throw error;
  }
}

/** Runs `work`, showing any error as a message instead of throwing it. */
async function perform(work) {
  try {
    return await work();
  } catch (error) {
    const canceled = wasCanceled(error);
    toast(canceled ? 'Operation canceled.' : error, !canceled);
    return null;
  }
}

/** Shows a file or folder chooser (`kind`: see `pick` in bridge.rs). */
async function pick(kind, start = '') {
  return call('pick', { kind, start });
}

// ---- Theme and text size ----

const systemTheme = window.matchMedia('(prefers-color-scheme: dark)');

/** Applies the chosen theme (or the system's) and text size. */
function applyTheme() {
  const mode = state.prefs.theme || 'system';
  const scale = textScale(state.prefs.text_scale);
  document.documentElement.dataset.textScale = String(scale);
  document.documentElement.style.fontSize = scale + '%';
  document.documentElement.dataset.theme =
    mode === 'system' ? (systemTheme.matches ? 'dark' : 'light') : mode;
  if ($('#content')) resizeSelectionBar();
}

systemTheme.addEventListener('change', () => {
  if (!state.prefs.theme || state.prefs.theme === 'system') applyTheme();
});

const narrowLayout = window.matchMedia('(max-width:760px)');
narrowLayout.addEventListener('change', () => {
  if ($('#content')) {
    resizeSelectionBar();
    loadCovers();
  }
});

// ---- Preferences ----

/** Saves the preferences. */
async function preferences() {
  rememberRoots();
  await call('preferences', state.prefs);
  updateLocation();
}

/** Saves developer update settings, which makes the last update check stale. */
async function developerPreferences() {
  await preferences();
  state.updates = null;
  state.noticeDismissed = '';
  render();
}

function developerSettingsView() {
  const directory = escapeHtml(state.prefs.developer_updates_directory || '');
  return [
    '<div class="card"><h3>Developer updates</h3>',
    checkbox(
      'pref-developer',
      'Use local builds instead of GitHub releases',
      state.prefs.developer_mode,
    ),
    '<label class="field">Build output directory<div class="row">',
    `<input class="grow" id="developer-directory" value="${directory}" placeholder="Choose the repository’s iso folder">`,
    action('developer-folder', 'Browse…', '', 'aria-label="Choose developer build folder"'),
    '</div></label>',
    '<p class="note">Builds write update manifests into the iso folder automatically. ',
    'Local updates work offline and detect rebuilt desktop apps even at the same version. ',
    'Check for updates after building. Missing local builds never fall back to GitHub.</p>',
    '</div>',
  ].join('');
}

// Each library's filters and selection are saved (see rememberLibraryView),
// a moment after the last change, without blocking the interface.
let viewTimer = null;
let viewDirty = false;
let viewRevision = 0;

/** How long to wait after a change before saving the library view. */
const VIEW_SAVE_DELAY = 400;

/** Remembers the current library's filters and selection, saving soon. */
function rememberLibraryView() {
  if (!state.libraryRoot) return;
  state.prefs.library_views ??= {};
  const next = { ...state.filter, selected: [...state.selected] };
  const saved = state.prefs.library_views[state.libraryRoot];
  if (JSON.stringify(saved) === JSON.stringify(next)) return;

  state.prefs.library_views[state.libraryRoot] = next;
  viewDirty = true;
  ++viewRevision;
  clearTimeout(viewTimer);
  viewTimer = setTimeout(() => perform(flushLibraryView), VIEW_SAVE_DELAY);
}

/**
 * Saves remembered library views now. While a backend operation runs (or
 * the views change during the save), tries again shortly.
 */
async function flushLibraryView() {
  clearTimeout(viewTimer);
  if (!viewDirty) return;
  const retrySoon = () => {
    viewTimer = setTimeout(() => perform(flushLibraryView), VIEW_SAVE_DELAY);
  };
  if (state.busy) {
    retrySoon();
    return;
  }

  const revision = viewRevision;
  try {
    await invoke('save_library_views', { views: state.prefs.library_views });
    viewDirty = revision !== viewRevision;
    if (viewDirty) retrySoon();
  } catch (error) {
    if (String(error).includes('Another operation')) {
      retrySoon();
      return;
    }
    throw error;
  }
}

// ---- PC libraries and drives ----

/** Updates the top bar's library and drive choosers and the eject button. */
function updateLocation() {
  const library = $('#pc-library');
  if (library) library.innerHTML = pcLibraryOptions();
  const drive = $('#usb-drive');
  if (drive) drive.innerHTML = driveOptions();

  const eject = $('#top-eject');
  if (eject) {
    const root = state.prefs.destination;
    const removable = !!root && !!state.driveStatus[root]?.removable;
    eject.hidden = !removable;
    eject.dataset.root = root || '';
    eject.title = removable ? 'Safely eject ' + displayPath(root) : '';
  }
}

/** A folder's own name (the last part of its path). */
function rootName(path) {
  const parts = String(path)
    .replace(/[\\/]+$/, '')
    .split(/[\\/]/);
  return parts[parts.length - 1] || String(path);
}

/** The name the person gave a drive, or "USB / SD". */
function driveName(path) {
  return state.prefs.drives?.[path]?.name || 'USB / SD';
}

/** The PC library chooser's options. */
function pcLibraryOptions() {
  const none = state.prefs.library ? '' : option('', 'Choose a PC library', true);
  const libraries = (state.prefs.libraries || [])
    .map((path) =>
      option(path, `${rootName(path)} — ${displayPath(path)}`, path === state.prefs.library),
    )
    .join('');
  return (
    none +
    libraries +
    option('add', 'Add a PC library…', false) +
    option('manage', 'Manage libraries…', false)
  );
}

/** The drive chooser's options. */
function driveOptions() {
  const none = state.prefs.destination ? '' : option('', 'Choose a USB / SD drive', true);
  const drives = (state.prefs.browser_drives || [])
    .map((path) => {
      const disconnected = state.driveStatus[path]?.connected === false;
      const label = `${driveName(path)} — ${displayPath(path)}${disconnected ? ' (not connected)' : ''}`;
      return option(path, label, path === state.prefs.destination);
    })
    .join('');
  return (
    none + drives + option('add', 'Add a drive…', false) + option('manage', 'Manage drives…', false)
  );
}

/** Adds the current library and drive to the saved lists. */
function rememberRoots() {
  const prefs = state.prefs;
  prefs.libraries = [...(prefs.libraries || [])];
  prefs.browser_drives = [...(prefs.browser_drives || [])];
  if (prefs.library && !prefs.libraries.includes(prefs.library))
    prefs.libraries.push(prefs.library);
  if (prefs.destination && !prefs.browser_drives.includes(prefs.destination))
    prefs.browser_drives.push(prefs.destination);
}

/** Checks which saved drives are connected and can be ejected. */
async function refreshDriveStatus() {
  const roots = state.prefs.browser_drives || [];
  const list = roots.length ? await call('drive_status', { roots }) : [];
  state.driveStatus = Object.fromEntries(list.map((status) => [status.root, status]));
  updateLocation();
}

/** The PC library / drive switch above the library. */
function viewSwitch() {
  const onDrive = !!state.browseRoot;
  const driveDisabled = state.prefs.destination ? '' : ' disabled';
  return [
    '<div class="view-switch" role="group" aria-label="Show">',
    action('view-pc', 'PC library', '', `aria-pressed="${!onDrive}"`),
    action('view-drive', 'USB / SD drive', '', `aria-pressed="${onDrive}"${driveDisabled}`),
    '</div>',
  ].join('');
}

/** A saved drive's status for the Manage drives list. */
function driveStatusText(status) {
  if (status?.connected === false) return 'not connected';
  if (status?.removable) return 'removable';
  if (status) return 'can’t be ejected here';
  return '';
}

/** Shows the Manage PC libraries or Manage drives (`kind` "drives") dialog. */
async function manageRoots(kind) {
  const drives = kind === 'drives';
  if (drives) await refreshDriveStatus();
  const roots = drives ? state.prefs.browser_drives || [] : state.prefs.libraries || [];
  const current = drives ? state.prefs.destination : state.prefs.library;

  const rows = roots.map((root) => {
    const status = state.driveStatus[root];
    const detail = [
      displayPath(root),
      root === current ? 'selected' : '',
      drives ? driveStatusText(status) : '',
    ]
      .filter(Boolean)
      .join(' · ');
    const name = drives ? driveName(root) : rootName(root);
    const eject =
      drives && status?.removable
        ? action('drive-eject', 'Safely eject', 'small', `data-root="${escapeHtml(root)}"`)
        : '';
    const forget = action(
      'forget-root',
      'Remove from list',
      'small danger',
      `data-kind="${kind}" data-root="${escapeHtml(root)}"`,
    );
    return [
      '<div class="root-row"><div class="grow">',
      `<strong>${escapeHtml(name)}</strong><div class="note">${escapeHtml(detail)}</div>`,
      `</div>${eject}${forget}</div>`,
    ].join('');
  });
  const list = roots.length
    ? rows.join('')
    : `<p>No ${drives ? 'drives' : 'PC libraries'} yet.</p>`;
  const notice =
    `<div class="notice">Removing ${drives ? 'a drive' : 'a library'} only takes it off this list. ` +
    'Nothing on it is changed, and you can add it again any time.</div>';

  dialog(
    drives ? 'Manage drives' : 'Manage PC libraries',
    list + notice,
    action('manage-add', drives ? 'Add a drive…' : 'Add a PC library…', '', `data-kind="${kind}"`) +
      action('close-dialog', 'Done', 'primary', 'data-dialog="main-dialog"'),
  );
}

/** Shows the PC library (`root` '') or a drive's games. */
async function switchLibrary(root) {
  rememberLibraryView();
  await flushLibraryView();
  state.browseRoot = root;
  state.page = 'library';
  state.coverCache.clear();
  updateLocation();
  await loadLibrary();
}

// ---- The window and its pages ----

/** A sidebar button for a page. */
function navButton(page) {
  return `<button class="nav" data-page="${page}"><span aria-hidden="true">${icons[page]}</span>${titles[page]}</button>`;
}

/** Draws the window's frame (sidebar, top bar, status bar), then the page. */
function shell() {
  const sidebar = [
    '<nav class="sidebar" aria-label="Main navigation">',
    '<img class="brand" src="assets/logo.png" alt="Oh My 3DO">',
    ['library', 'saves', 'media', 'drive'].map(navButton).join(''),
    '<div class="nav-bottom"><div class="nav-divider"></div>',
    ['updates', 'settings'].map(navButton).join(''),
    `<div class="version">Oh My 3DO · ${escapeHtml(state.version)}</div>`,
    '</div></nav>',
  ].join('');
  const topbar = [
    '<header class="topbar">',
    '<div class="library-location"><label for="pc-library">PC LIBRARY</label><select id="pc-library"></select></div>',
    '<div class="library-location"><label for="usb-drive">USB / SD DRIVE</label><select id="usb-drive"></select>',
    action('drive-eject', 'Safely eject', 'small', 'id="top-eject" hidden'),
    '</div>',
    `<div class="row">${action('library-refresh', 'Refresh', 'small')}</div>`,
    '</header>',
  ].join('');
  // Hidden regions that screen readers announce (see accessibility.mjs).
  const announcers = [
    '<div data-announcer="polite" class="sr-only" role="status" aria-live="polite" aria-atomic="true"></div>',
    '<div data-announcer="alert" class="sr-only" role="alert" aria-live="assertive" aria-atomic="true"></div>',
  ].join('');
  const statusbar = [
    '<footer class="statusbar">',
    '<span id="busy-dot" class="busy-dot hidden"></span>',
    '<progress id="operation-meter" aria-label="Operation progress" max="1" hidden></progress>',
    '<span id="operation-percent" hidden></span>',
    '<span id="status-message" class="message" role="status" aria-live="polite" aria-atomic="true">Ready</span>',
    action('cancel', 'Cancel', 'small hidden', 'id="cancel-job"'),
    action('logs', 'Activity', 'quiet small'),
    '</footer>',
  ].join('');

  document.querySelector('#app').innerHTML = [
    '<a class="skip-link" href="#content">Skip to content</a>',
    '<div class="shell">',
    sidebar,
    '<section class="workspace">',
    topbar,
    announcers,
    '<div id="update-notice" hidden></div>',
    '<main id="content" class="content" tabindex="-1" aria-labelledby="page-title"></main>',
    '</section>',
    statusbar,
    '</div>',
  ].join('');
  updateLocation();
  render();
}

/** Each page's view function. */
const pageViews = {
  library: () => libraryView(),
  import: () => importView(),
  saves: () => savesView(),
  media: () => mediaView(),
  drive: () => driveView(),
  settings: () => settingsView(),
  updates: () => updatesView(),
};

/**
 * Redraws the current page. Keeps keyboard focus where it was if that
 * element still exists; on a new page, focuses its heading.
 */
function render() {
  const content = $('#content');
  const focus = captureFocus(content);
  const pageChanged = state.renderedPage !== state.page;
  updateLocation();
  renderUpdateNotice();

  // The Add discs page belongs to the Library in the sidebar.
  const navPage = state.page === 'import' ? 'library' : state.page;
  document.querySelectorAll('[data-page]').forEach((button) => {
    const current = button.dataset.page === navPage;
    button.classList.toggle('active', current);
    if (current) button.setAttribute('aria-current', 'page');
    else button.removeAttribute('aria-current');
  });

  // Scrolled tables are rebuilt too; on the same page they keep their place.
  const scrolled = [...content.querySelectorAll('.table-wrap')].map((wrap) => wrap.scrollTop);
  const pageScroll = content.scrollTop;
  content.innerHTML = (pageViews[state.page] || pageViews.library)();
  if (!pageChanged) {
    content.querySelectorAll('.table-wrap').forEach((wrap, index) => {
      wrap.scrollTop = scrolled[index] || 0;
    });
    content.scrollTop = pageScroll;
  }
  loadCovers();
  resizeSelectionBar();
  if (pageChanged || (focus && !restoreFocus(focus, content))) focusHeading(content);
  state.renderedPage = state.page;
}

/** A page's placeholder when it has nothing to show yet. */
function empty(title, note, buttons = '') {
  return [
    '<div class="empty"><div class="empty-mark">◉</div>',
    `<h2>${escapeHtml(title)}</h2><p>${escapeHtml(note)}</p>`,
    `<div class="row empty-actions">${buttons}</div>`,
    '</div>',
  ].join('');
}
// ---- The library page ----

/** The buttons for the selected games (in the selection bar). */
function selectionButtonsView() {
  const count = state.selected.size;
  const onDrive = !!state.browseRoot;
  const someNotMovable = state.groups.some(
    (group) => state.selected.has(group.key) && !group.movable,
  );
  return [
    action('game-details', 'Game details', 'quiet', count !== 1 ? 'disabled' : ''),
    action('remove-titles', 'Remove…', 'danger'),
    // The rest only apply to the PC library.
    onDrive ? '' : action('game-info-bulk', 'Edit information…', 'quiet'),
    onDrive ? '' : action('media-selected', 'Prepare missing media', 'quiet'),
    onDrive
      ? ''
      : action('move-games', 'Move category…', 'quiet', someNotMovable ? 'disabled' : ''),
    onDrive ? '' : action('copy-open', 'Copy selected to USB…', 'primary'),
  ].join('');
}

/** The media filter's choices. */
const mediaFilters = [
  ['', 'All media'],
  ['cover', 'Missing covers'],
  ['preview', 'Missing previews'],
  ['either', 'Missing covers or previews'],
];

/** The game information filter's choices. */
const informationFilters = [
  ['', 'All information'],
  ['any', 'Missing any information'],
  ['description', 'Missing descriptions'],
  ['released', 'Missing release dates / years'],
  ['genre', 'Missing genres'],
  ['developer', 'Missing developers'],
  ['publisher', 'Missing publishers'],
  ['players', 'Missing player counts'],
  ['edited', 'Manually edited'],
];

/** The library page: heading, search and filters, the games, and the selection bar. */
function libraryView() {
  const visible = visibleGames(state.groups, state.filter);
  const count = state.selected.size;
  return (
    viewSwitch() +
    libraryHeading(visible) +
    browserSummary() +
    libraryToolbar(visible) +
    libraryContent(visible) +
    (count ? selectionBar(visible) : '')
  );
}

/** The library heading: how many games are shown, and the page's buttons. */
function libraryHeading(visible) {
  const total = state.groups.length;
  const shown =
    visible.length === total
      ? quantity(total, 'game')
      : visible.length + ' of ' + total + ' games shown';
  const note = `${shown} · ${quantity(state.records.length, 'disc')} in library`;
  const noGames = !state.groups.length ? 'disabled' : '';

  const buttons = state.browseRoot
    ? action(
        'drive-media-update',
        'Update media & information…',
        'primary',
        !state.groups.length || !state.prefs.library ? 'disabled' : '',
      ) + action('browse-drive-setup', 'Drive setup…', 'small')
    : action('information-tools', 'Metadata…', 'quiet small', noGames) +
      action('information-refresh', 'Get game information…', 'quiet small', noGames) +
      action('add-discs', '+ Add discs', 'orange', !state.prefs.library ? 'disabled' : '');

  return heading(state.browseRoot ? 'On your USB / SD drive' : 'Your PC library', note, buttons);
}

/** The search box, filters, selection buttons and category chips. */
function libraryToolbar(visible) {
  const filter = state.filter;
  const regions = [...new Set(state.groups.map((group) => group.region).filter(Boolean))].sort();
  const anyFilter = Object.entries(filter).some(([key, value]) => key !== 'searchScope' && value);
  const usedCategories = categories.filter((category) =>
    state.groups.some((group) => group.category === category),
  );

  const search = [
    '<div class="library-search-controls">',
    `<input type="search" id="library-search" aria-label="Find a game" placeholder="Find a game…" value="${escapeHtml(filter.search)}">`,
    '<select id="library-search-scope" aria-label="Search in">',
    option('title', 'Titles only', filter.searchScope !== 'information'),
    option('information', 'Titles & information', filter.searchScope === 'information'),
    '</select>',
    '<select id="library-region" aria-label="Region">',
    option('', 'All regions', !filter.region),
    regions.map((region) => option(region, region, filter.region === region)).join(''),
    '</select>',
    '<select id="library-media" aria-label="Missing artwork">',
    mediaFilters
      .map(([value, label]) => option(value, label, (filter.media || '') === value))
      .join(''),
    '</select>',
    '<select id="library-information" aria-label="Game information filter">',
    informationFilters
      .map(([value, label]) => option(value, label, (filter.information || '') === value))
      .join(''),
    '</select>',
    '</div>',
  ].join('');
  const buttons = [
    '<div class="row">',
    anyFilter ? action('filters-clear', 'Clear filters', 'quiet small') : '',
    action('select-shown', 'Select all shown', 'small', !visible.length ? 'disabled' : ''),
    state.selected.size ? action('clear-selection', 'Clear selection', 'quiet small') : '',
    '</div>',
  ].join('');
  const chips = ['', ...usedCategories]
    .map((category) => {
      const active = filter.category === category;
      return action(
        'filter-category',
        escapeHtml(category || 'All'),
        `chip${active ? ' active' : ''}`,
        `data-category="${escapeHtml(category)}" aria-pressed="${active}"`,
      );
    })
    .join('');

  return [
    `<div class="row spread toolbar library-toolbar">${search}${buttons}</div>`,
    '\n ',
    `<div class="row filters">${chips}</div>`,
  ].join('');
}

/** The games, or a message saying why there are none to show. */
function libraryContent(visible) {
  if (state.browserError)
    return empty(
      'Could not read this library',
      state.browserError,
      action('library-refresh', 'Try again', 'primary'),
    );
  if (!state.browseRoot && !state.prefs.library)
    return empty(
      'A home for your 3DO library',
      'Choose the folder containing your games, or create a staging library to import discs into.',
      action('choose-library', 'Choose library folder', 'primary'),
    );
  if (!state.groups.length)
    return state.browseRoot
      ? empty(
          'No games on this drive',
          'Choose your PC library and copy selected games to this drive.',
        )
      : empty(
          'Your library is ready for games',
          'Add ISO or CUE files to identify and organize your collection. Your original files stay in place.',
          action('add-discs', 'Add discs', 'primary'),
        );
  if (!visible.length)
    return empty(
      'No matching games',
      'Try another search, category, region or information filter.',
      action('filters-clear', 'Clear filters', 'primary'),
    );

  // The grid is one Tab stop: only the focused game is in the tab order,
  // and the arrow keys move between games (see moveGameFocus).
  const focusedGame =
    visible.find((group) => group.key === state.focusedGame)?.key || visible[0]?.key;
  const cards = visible.map((group, index) => gameCard(group, index, focusedGame)).join('');
  return [
    '<p class="note" id="game-keyboard">Arrow keys move between games; Space selects.</p>',
    '<div class="grid" role="group" aria-label="Games" aria-describedby="game-keyboard">',
    cards,
    '</div>',
  ].join('');
}

/** One game in the grid: cover, title, details and where it is. */
function gameCard(group, index, focusedGame) {
  const selected = state.selected.has(group.key);
  const cover = group.cover
    ? `<img data-cover="${escapeHtml(group.cover)}" alt="" loading="lazy">`
    : '<span class="placeholder" aria-hidden="true">◉</span>';
  const details =
    escapeHtml([group.region, group.genre.split(';')[0]].filter(Boolean).join(' · ')) +
    (group.discs.length > 1 ? ` · ${quantity(group.discs.length, 'disc')}` : '') +
    (group.edited ? ' · Edited' : '');

  return [
    `<button class="game${selected ? ' selected' : ''}" data-game="${escapeHtml(group.key)}"`,
    ` tabindex="${group.key === focusedGame ? 0 : -1}" aria-label="${escapeHtml(group.title)}"`,
    ` aria-describedby="game-detail-${index} game-status-${index}" aria-pressed="${selected}"`,
    ` title="${escapeHtml(group.title + '\n' + group.key)}">`,
    `<div class="cover">${cover}<span class="selection-check" aria-hidden="true">${selected ? '✓' : ''}</span></div>`,
    `<div class="game-title">${escapeHtml(group.title)}</div>`,
    `<div class="game-detail" id="game-detail-${index}">${details}</div>`,
    `<div class="game-detail game-status" id="game-status-${index}">${gameStatus(group)}</div>`,
    '</button>',
  ].join('');
}

/** A game's status line: on a drive, its size and media; in the library, whether it is on the drive. */
function gameStatus(group) {
  if (state.browseRoot) {
    const cover = group.missingCover ? 'Cover missing' : 'Cover available';
    const preview = group.missingPreview ? 'Preview missing' : 'Preview available';
    return `${bytes(group.bytes)} · ${cover} · ${preview}`;
  }
  if (!group.presenceKnown) return '';
  if (group.presentDiscs === group.discs.length) return 'Already on drive';
  if (group.presentDiscs) return `${group.presentDiscs} of ${group.discs.length} discs on drive`;
  return 'Not on drive';
}

/** The bar at the bottom while games are selected. */
function selectionBar(visible) {
  const count = state.selected.size;
  const visibleKeys = new Set(visible.map((group) => group.key));
  const hiddenSelected = state.groups.filter(
    (group) => state.selected.has(group.key) && !visibleKeys.has(group.key),
  ).length;
  const discs = quantity(selectedDiscs(state.groups, state.selected).length, 'disc');
  const hidden = hiddenSelected ? ' · ' + hiddenSelected + ' selected outside this view' : '';
  return [
    '<div class="selection-bar row spread" id="selection-actions">',
    `<div><strong>${count} game${count === 1 ? '' : 's'} selected</strong><small> · ${discs}${hidden}</small></div>`,
    action('selection-actions', 'Actions…', 'selection-more'),
    `<div class="row selection-buttons">${selectionButtonsView()}</div>`,
    '</div>',
  ].join('');
}

let coverObserver;
let selectionObserver;

/**
 * Keeps room at the bottom of the page for the selection bar (and a fixed
 * status bar), so they never cover the last games.
 */
function resizeSelectionBar() {
  selectionObserver?.disconnect();
  const content = $('#content');
  const bar = $('.selection-bar');
  if (!content) return;

  const size = () => {
    const height = bar ? Math.ceil(bar.getBoundingClientRect().height) : 0;
    content.style.setProperty('--selection-space', height ? `${height + 24}px` : '0px');
    const footer = $('.statusbar');
    const fixedFooter =
      footer && getComputedStyle(footer).position === 'fixed'
        ? footer.getBoundingClientRect().height
        : 0;
    document.documentElement.style.setProperty(
      '--fixed-ui-space',
      `${height + fixedFooter + 16}px`,
    );
  };
  size();
  if (bar) {
    selectionObserver = new ResizeObserver(size);
    selectionObserver.observe(bar);
  }
}

/**
 * Loads cover thumbnails as their games scroll into view (or come within
 * 200 pixels), from the cache or the backend.
 */
function loadCovers() {
  coverObserver?.disconnect();
  const content = $('#content');
  const scrollsItself = getComputedStyle(content).overflowY !== 'visible';

  coverObserver = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        coverObserver.unobserve(entry.target);
        const img = entry.target;
        const root = state.browser?.root || state.libraryRoot;
        const token = state.browseRoot ? state.browser?.identity?.token : null;
        const name = state.browseRoot ? img.dataset.cover : `.ohmy3do/covers/${img.dataset.cover}`;
        const key = `${root}/${token || ''}/${name}`;
        if (state.coverCache.has(key)) {
          img.src = state.coverCache.get(key);
          continue;
        }
        coverQueue.push({ img, key, name, root, token, generation: state.coverGeneration });
        pumpCovers();
      }
    },
    { root: scrollsItself ? content : null, rootMargin: '200px' },
  );
  document.querySelectorAll('img[data-cover]').forEach((img) => coverObserver.observe(img));
}

// Covers waiting to load, and how many are loading now.
let coverRunning = 0;
const coverQueue = [];

/** The most covers loaded at once. */
const MAX_COVER_LOADS = 3;

/**
 * Starts loading queued covers, a few at a time. A cover from before the
 * library was reloaded (an older `coverGeneration`) is not kept; one that
 * fails to load is removed, leaving the placeholder look.
 */
function pumpCovers() {
  while (coverRunning < MAX_COVER_LOADS && coverQueue.length) {
    const item = coverQueue.shift();
    if (!item.img.isConnected) continue;
    coverRunning++;
    invoke('cover', { name: item.name, browserRoot: item.root, token: item.token })
      .then((src) => {
        if (item.generation === state.coverGeneration) {
          state.coverCache.set(item.key, src);
          if (item.img.isConnected) item.img.src = src;
        }
      })
      .catch(() => {
        item.img.remove();
      })
      .finally(() => {
        coverRunning--;
        pumpCovers();
      });
  }
}

/**
 * Starts updating the browsed drive's media and information from the PC
 * library: for the selected games (or all), matched to library discs by
 * path. Shows what will happen before anything is changed.
 */
async function driveMediaUpdate() {
  if (!state.browseRoot || !state.browser) throw new Error('Refresh the drive browser first.');
  const source = await call('library');
  const wantedPaths = state.selected.size
    ? selectedDiscs(state.groups, state.selected)
    : state.records.map((record) => record.path);
  const wanted = new Set(wantedPaths.map((path) => path.toLowerCase()));
  const discs = source.records
    .map((record) => record.path)
    .filter((path) => wanted.has(path.toLowerCase()));
  state.driveUpdate = {
    root: state.browser.root,
    token: state.browser.identity.token,
    source: state.prefs.library,
    discs,
  };

  const skipped = [...wanted].filter((path) => !discs.some((disc) => disc.toLowerCase() === path));
  const skippedNotice = skipped.length
    ? [
        '<details class="notice warning">',
        `<summary>${quantity(skipped.length, 'disc')} have no matching PC library entry and will be skipped</summary>`,
        skipped.map((path) => `<p class="path">${pathLabel(path)}</p>`).join(''),
        '</details>',
      ].join('')
    : '';
  const body = [
    `<p>Drive: ${pathLabel(state.driveUpdate.root)}<br>Source: ${pathLabel(state.prefs.library)}</p>`,
    `<p>${quantity(discs.length, 'disc')} matched by file path`,
    state.selected.size ? ' from the selected games' : ' across this drive',
    '. Existing PC artwork, previews, descriptions and metadata will be transferred with console support files. ',
    'Game disc files will stay in place.</p>',
    skippedNotice,
    '<p class="note">Edit game information or assign replacement artwork in the PC library first. ',
    'The option below finds missing media and descriptions using your configured sources; it keeps existing information.</p>',
    checkbox(
      'drive-update-prepare',
      'Find missing media and descriptions in the PC library first',
      false,
    ),
  ].join('');
  dialog(
    'Update drive media & information',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
      action('drive-media-review', 'Review update', 'primary', discs.length ? '' : 'disabled'),
  );
}

/**
 * Reviews the drive media update: checks the drive is the same one,
 * optionally fills in missing media first, then reviews the copy (support
 * files only; game files must already be there).
 */
async function reviewDriveMedia() {
  const update = state.driveUpdate;
  if (!update || update.source !== state.prefs.library)
    throw new Error('PC library changed. Start the update again.');
  const drive = await call('drive_inspect', { root: update.root });
  if (drive.token !== update.token) throw new Error('Drive changed. Refresh and start again.');
  if ($('#drive-update-prepare').checked) await call('media_selected', { discs: update.discs });

  const plan = await call('copy_preview', {
    destination: update.root,
    discs: update.discs,
    verify: false,
    existing_only: true,
    drive_token: update.token,
  });
  update.plan = plan;

  const changes =
    plan.files
      .filter((file) => file.status !== 'unchanged')
      .map((file) => `${escapeHtml(file.status)} ${pathLabel(file.path)}`)
      .join('\n') || 'No changed files.';
  const body = [
    `<p>Destination: ${pathLabel(update.root)}</p>`,
    '<p>Game disc files will not be copied or replaced. ',
    'Media, information and console support will be updated; existing settings and saves are preserved.</p>',
    storageView(plan.storage),
    `<details><summary>File changes</summary><div class="progress-log">${changes}</div></details>`,
  ].join('');
  dialog(
    'Review drive media update',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
      action('drive-media-apply', 'Update drive', 'primary', plan.storage.fits ? '' : 'disabled'),
  );
}

/**
 * The line under the heading: for a drive, its game size, free space,
 * name and warnings; for the PC library, the copy destination.
 */
function browserSummary() {
  const browser = state.browser;
  if (state.browseRoot && browser) {
    const space =
      browser.available !== null
        ? ' · ' + bytes(browser.available) + ' free of ' + bytes(browser.capacity)
        : '';
    const name = browser.identity?.identity
      ? escapeHtml(browser.identity.identity.name) + ' · '
      : '';
    const warnings = browser.warnings.map((warning) => `<p>${escapeHtml(warning)}</p>`).join('');
    return [
      `<div class="notice"><strong>Drive browser</strong> · ${bytes(browser.game_bytes)} of games${space}<br>`,
      `${name}Copy destination: ${pathLabel(state.prefs.destination || 'Not selected')}${warnings}`,
      '</div>',
    ].join('');
  }
  if (!state.prefs.destination) return '';

  const availability =
    browser?.destination_available === false
      ? ' · Drive unavailable — connect it and refresh.'
      : ' · “Already on drive” compares disc paths and sizes, including CUE tracks.';
  return `<p class="note">Copy destination: ${pathLabel(state.prefs.destination)} ${availability} Refresh after changing drives.</p>`;
}

/**
 * Loads the PC library or the browsed drive, restores that library's saved
 * filters and selection, and draws it.
 */
async function loadLibrary() {
  state.coverGeneration = (state.coverGeneration || 0) + 1;
  state.coverCache.clear();
  coverQueue.length = 0;
  if (!state.browseRoot && !state.prefs.library) {
    state.browserError = '';
    state.records = [];
    state.groups = [];
    state.libraryRoot = null;
    state.browser = null;
    render();
    return;
  }

  let result;
  try {
    result = await call(
      state.browseRoot ? 'drive_browser' : 'library',
      state.browseRoot ? { root: state.browseRoot } : {},
    );
  } catch (error) {
    state.records = [];
    state.groups = [];
    state.selected.clear();
    state.browser = null;
    state.browserError = String(error);
    state.libraryRoot = null;
    render();
    throw error;
  }

  state.browserError = '';
  state.browser = result;
  state.records = result.records || [];
  categories.splice(0, categories.length, ...result.categories);
  state.groups = groupGames(state.records);

  // A drive's view is remembered by its identity as well as its path.
  const key = state.browseRoot
    ? `drive:${result.identity?.token || 'unnamed'}:${result.root}`
    : result.root;
  const current =
    state.libraryRoot === key ? { ...state.filter, selected: [...state.selected] } : null;
  const view = restoreLibraryView(
    current || state.prefs.library_views?.[key],
    state.groups,
    categories,
  );
  state.libraryRoot = key;
  state.selected = new Set(view.selected);
  state.filter = {
    search: view.search,
    category: view.category,
    region: view.region,
    media: view.media,
    information: view.information,
    searchScope: view.searchScope,
  };
  state.anchor = null;
  rememberLibraryView();
  render();
  setStatus(`${state.groups.length} games ${state.browseRoot ? 'on drive' : 'in the PC library'}`);
}

// ---- Dialogs ----

/**
 * Opens (or redraws) the dialog `id` with a title, body HTML and footer
 * buttons. Redrawing keeps focus on the same control. Closing returns focus
 * to whatever opened it; Escape closes it unless an operation is running.
 */
function dialog(title, body, footer = '', id = 'main-dialog') {
  const previous = document.getElementById(id);
  const focus = previous ? captureFocus(previous) : null;
  const opener = previous?.returnFocus || captureFocus(document);
  previous?.remove();

  const element = document.createElement('dialog');
  element.id = id;
  element.returnFocus = opener;
  element.setAttribute('aria-labelledby', id + '-title');
  const close = action(
    'close-dialog',
    '×',
    'icon quiet',
    `data-dialog="${id}" aria-label="Close ${escapeHtml(title)}"`,
  );
  element.innerHTML = [
    `<div class="dialog-head"><h2 id="${id}-title" tabindex="-1">${escapeHtml(title)}</h2>${close}</div>`,
    '<div class="sr-only" data-announcer="polite" role="status" aria-live="polite" aria-atomic="true"></div>',
    '<div class="sr-only" data-announcer="alert" role="alert" aria-live="assertive" aria-atomic="true"></div>',
    `<div class="dialog-body">${body}</div>`,
    `<div class="dialog-foot">${footer}</div>`,
  ].join('');
  document.body.append(element);

  element.addEventListener('close', () => {
    element.remove();
    const front = [...document.querySelectorAll('dialog[open]')].at(-1);
    if (!restoreFocus(element.returnFocus, front || document)) focusHeading(front || $('#content'));
  });
  element.addEventListener('cancel', (event) => {
    event.preventDefault();
    if (element.dataset.operationBusy !== 'true')
      perform(() => handlers['close-dialog']({ dataset: { dialog: id } }));
  });

  element.showModal();
  // A new dialog focuses its chosen control, its safe answer, or its heading.
  if (!restoreFocus(focus, element)) {
    const initial = element.querySelector(
      '[autofocus],[data-answer="cancel"],[data-dirty="cancel"]',
    );
    if (!focusElement(initial)) focusHeading(element);
  }
  dialogProgress.attach(element);
  return element;
}

/** Closes a dialog, unless an operation in it is still running. */
function closeDialog(id = 'main-dialog') {
  const element = document.getElementById(id);
  if (element?.dataset.operationBusy !== 'true') element?.close();
}

/** Asks a yes/no question; resolves true for yes. */
function confirmAction(title, message, yes = 'Continue', danger = false) {
  return new Promise((resolve) => {
    const buttons =
      '<button data-answer="cancel">Cancel</button>' +
      `<button class="${danger ? 'danger' : 'primary'}" data-answer="yes">${escapeHtml(yes)}</button>`;
    const question = dialog(title, `<p>${escapeHtml(message)}</p>`, buttons, 'confirm-dialog');
    question.addEventListener('click', (event) => {
      const answer = event.target.closest('[data-answer]');
      if (answer) {
        resolve(answer.dataset.answer === 'yes');
        question.close();
      }
    });
    question.addEventListener('cancel', () => resolve(false));
    question.addEventListener('close', () => resolve(false));
  });
}

/**
 * Before leaving an edited save backup: asks to save or discard the edits.
 * Resolves true to go on (saved, discarded, or nothing to save).
 */
async function resolveDirty() {
  if (!state.backup?.dirty && !state.hexDirty) return true;
  return new Promise((resolve) => {
    const question = dialog(
      'Unsaved save edits',
      '<p>Save your edited backup before continuing?</p>',
      '<button data-dirty="cancel">Cancel</button>' +
        '<button data-dirty="discard">Discard changes</button>' +
        '<button data-dirty="save" class="primary">Save changes</button>',
      'dirty-dialog',
    );
    question.addEventListener('click', (event) => {
      const answer = event.target.closest('[data-dirty]');
      if (!answer) return;
      perform(async () => {
        if (answer.dataset.dirty === 'save') {
          if (!(await saveBackup())) return;
          resolve(true);
        } else if (answer.dataset.dirty === 'discard') {
          await call('backup_discard');
          state.backup = null;
          state.entry = null;
          state.hexDirty = false;
          resolve(true);
        } else resolve(false);
        question.close();
      });
    });
    question.addEventListener('cancel', () => resolve(false));
    question.addEventListener('close', () => resolve(false));
  });
}

// ---- Copying games to a drive ----

/** Opens the copy dialog for the selected games. */
function copyDialog() {
  const chosen = state.groups.filter((group) => state.selected.has(group.key));
  state.copyDiscs = chosen.flatMap((group) => group.discs);
  state.copyRoot = state.prefs.library;
  state.copy = null;
  renderCopyDialog(chosen);
}

/** Draws the copy dialog, with the review once there is one (`state.copy`). */
function renderCopyDialog(chosen = state.groups.filter((group) => state.selected.has(group.key))) {
  const copy = state.copy;
  const review = copy
    ? [
        '<div class="card section">',
        `<p><strong>Destination: ${escapeHtml(copy.drive?.identity?.name || 'Unnamed drive')}</strong> · ${pathLabel(copy.destination)}</p>`,
        storageView(copy.storage),
        copySummary(copy),
        '<details><summary>Show file details</summary><div class="progress-log">',
        copy.files
          .map((file) => `${escapeHtml(file.status.padEnd(10))} ${pathLabel(file.path)}`)
          .join('\n'),
        '</div></details>',
        '</div>',
      ].join('')
    : '';
  const body = [
    `<p><strong>${quantity(chosen.length, 'game')}</strong> · ${quantity(state.copyDiscs.length, 'disc')}. `,
    'Covers, previews and menu support are included.</p>',
    '<details><summary>Selected games</summary>',
    chosen.map((group) => `<div>${escapeHtml(group.title)}</div>`).join(''),
    '</details>',
    '<label class="field">USB destination<div class="row">',
    `<input class="grow" id="copy-destination" value="${escapeHtml(state.prefs.destination)}" placeholder="Choose the drive root">`,
    action('copy-destination', 'Browse…', '', 'aria-label="Choose copy destination"'),
    '</div></label>',
    '<div class="notice">Games, settings, favorites and saves already on this drive are preserved.</div>',
    checkbox('copy-verify', 'Verify existing disc contents (slower)', state.copyVerify),
    review,
  ].join('');

  dialog(
    'Copy games to USB',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
      action(
        copy ? 'copy-apply' : 'copy-preview',
        copy ? 'Copy to USB' : 'Review copy',
        'primary',
        copy && !copy.storage.fits ? 'disabled' : '',
      ),
  );
}

/**
 * The recovery copies dialog: save backups the app kept before replacing
 * files, which the person can choose to delete. Nothing is chosen
 * automatically.
 */
function recoveryDialog() {
  const review = state.cleanup;
  if (!review) return;
  const selected = review.copies.filter((copy) => state.cleanupSelected.has(copy.name));
  const selectedBytes = selected.reduce((total, copy) => total + copy.bytes, 0);

  const copyRow = (copy) => {
    const checked = state.cleanupSelected.has(copy.name) ? 'checked' : '';
    const badge = copy.newest
      ? ' <span class="badge">Newest copy</span>'
      : copy.keep
        ? ' <span class="badge">Newest valid copy</span>'
        : '';
    const when = escapeHtml(new Date(copy.modified * 1000).toLocaleString());
    const validity = copy.valid ? 'Valid archive' : 'Invalid archive — inspect before discarding';
    return [
      `<label class="card"><input type="checkbox" data-recovery-copy="${escapeHtml(copy.name)}" ${checked}>`,
      `<span><strong>${escapeHtml(copy.original)}</strong>${badge}<br>`,
      `<small>${escapeHtml(copy.name)}<br>${bytes(copy.bytes)} · ${when} · ${validity}</small></span>`,
      '</label>',
    ].join('');
  };
  const list = review.copies.length
    ? `<div class="stack">${review.copies.map(copyRow).join('')}</div>`
    : '<p>No app-created save recovery copies were found in this folder.</p>';
  const count = review.copies.length;
  const body = [
    `<p>${pathLabel(review.directory)}</p>`,
    `<p>${count} app-created save recovery ${count === 1 ? 'copy' : 'copies'} · <strong>${bytes(review.bytes)}</strong> total. `,
    'Current .3ds slots, recovery.3ds/recovery2.3ds and unrelated .bak files are excluded.</p>',
    '<div class="row">',
    action('recovery-older', 'Select older copies (keep newest)', 'small'),
    action('recovery-clear', 'Clear selection', 'quiet small'),
    '</div>',
    `<p class="note">${selected.length} selected · ${bytes(selectedBytes)} to reclaim. Nothing is selected automatically.</p>`,
    list,
  ].join('');

  dialog(
    'Recovery copies',
    body,
    action('close-dialog', 'Close', '', 'data-dialog="main-dialog"') +
      action(
        'recovery-delete',
        'Delete selected copies',
        'danger',
        selected.length ? '' : 'disabled',
      ),
  );
}

/** A copy review's counts: discs, files, folders to move, and files by status. */
function copySummary(plan) {
  const count = (status) => plan.files.filter((file) => file.status === status).length;
  // Each status, with its wording for one file and for several.
  const statuses = [
    ['copy', 'new file', 'new files'],
    ['update', 'file to update', 'files to update'],
    ['preserve', 'existing file kept', 'existing files kept'],
    ['unchanged', 'file already up to date', 'files already up to date'],
    ['remap', 'reference file to adjust', 'reference files to adjust'],
    ['remove', 'old file to remove', 'old files to remove'],
  ];
  const items = statuses
    .filter(([status]) => count(status))
    .map(
      ([status, one, many]) =>
        `${count(status).toLocaleString('en-US')} ${count(status) === 1 ? one : many}`,
    );
  const folders = plan.folders_to_move
    ? ' · ' + quantity(plan.folders_to_move, 'folder') + ' to move'
    : '';
  return [
    '<div class="copy-summary">',
    `<p>${quantity(plan.games, 'disc')} · ${quantity(plan.files.length, 'file')}${folders}</p>`,
    `<ul>${items.map((text) => `<li>${escapeHtml(text)}</li>`).join('')}</ul>`,
    '</div>',
  ].join('');
}

/** A copy's size: what will be transferred, the breakdown, and whether it fits. */
function storageView(size) {
  const shortfall = size.fits
    ? ''
    : `<div class="notice error">Not enough room: ${bytes(size.shortfall)} more free space is needed. Select fewer games or choose another drive.</div>`;
  return [
    '<div class="storage-estimate"><h3>Transfer size</h3>',
    `<p class="transfer-size"><strong>${bytes(size.to_copy)}</strong> to transfer</p>`,
    '<details><summary>Size breakdown</summary><div class="row">',
    `<span>Games <strong>${bytes(size.games)}</strong></span>`,
    `<span>Covers <strong>${bytes(size.covers)}</strong></span>`,
    `<span>Previews <strong>${bytes(size.previews)}</strong></span>`,
    `<span>Menu & support <strong>${bytes(size.support)}</strong></span>`,
    '</div>',
    `<p class="note">Selected files total ${bytes(size.total)}, including files already on the drive.</p>`,
    '</details>',
    `<p><strong>${bytes(size.available)}</strong> free now · <strong>${bytes(size.remaining)}</strong> estimated free after copying.</p>`,
    shortfall,
    `<p class="note">Estimate includes ${bytes(size.reserve)} reserved for filesystem allocation and temporary/support files. `,
    'Free space is checked again before copying.</p>',
    '</div>',
  ].join('');
}

/** The last drive check's results, if they are for the chosen drive. */
function readinessView() {
  const report = state.readiness;
  if (!report || report.requested_root !== state.prefs.destination) return '';

  const verdict = report.ready
    ? report.warnings
      ? 'Ready with warnings'
      : 'Drive checks passed'
    : 'Drive needs attention';
  const checkCard = (check) => {
    const badge = check.status === 'error' ? 'error' : check.status === 'warning' ? 'warning' : '';
    const status = check.status === 'ok' ? 'Passed' : check.status;
    const fix = check.fix ? `<p>${escapeHtml(check.fix)}</p>` : '';
    return [
      `<div class="card"><strong><span class="badge ${badge}">${escapeHtml(status)}</span> ${escapeHtml(check.name)}</strong>`,
      `<p class="note">${escapeHtml(check.detail)}</p>${fix}</div>`,
    ].join('');
  };
  return [
    '<div id="readiness-results" class="section">',
    `<div class="notice ${report.errors ? 'error' : ''}"><strong>${verdict}</strong> · ${report.errors} errors · ${report.warnings} warnings</div>`,
    `<p class="note">${pathLabel(report.root)} · Snapshot from the last check. `,
    'This does not test console booting or perform a full game checksum scan.</p>',
    `<div class="stack">${report.checks.map(checkCard).join('')}</div>`,
    '</div>',
  ].join('');
}

/** Shows how the open backup differs from the backup at `path`. */
function comparisonDialog(result, path) {
  const rows = result.rows;
  const identical =
    !result.added && !result.removed && !result.changed
      ? '<p class="section">The archives contain identical saves, filenames and file types.</p>'
      : '';
  const changed = rows
    .filter((row) => row.state !== 'Unchanged')
    .map(comparisonRow)
    .join('');
  const unchanged = result.unchanged
    ? [
        `<details class="section"><summary>Show ${result.unchanged} unchanged saves</summary>`,
        rows
          .filter((row) => row.state === 'Unchanged')
          .map(comparisonRow)
          .join(''),
        '</details>',
      ].join('')
    : '';
  const body = [
    `<p>Reference: ${pathLabel(path)}<br>Current: ${pathLabel(state.backup.path || 'Unsaved draft')}</p>`,
    '<p>Changes below describe the current draft relative to the reference. Comparing does not change either archive.</p>',
    '<div class="row">',
    `<span class="badge">${result.added} added</span>`,
    `<span class="badge">${result.removed} removed</span>`,
    `<span class="badge warning">${result.changed} changed</span>`,
    `<span class="badge">${result.unchanged} unchanged</span>`,
    '</div>',
    identical,
    changed,
    unchanged,
  ].join('');
  dialog(
    'Compare backup archives',
    body,
    action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
  );
}

/** One save in a comparison, with its changed bytes (offset, before → after). */
function comparisonRow(row) {
  const size = (count) => (count === null ? 'Absent' : bytes(count));
  const hexByte = (value) => (value === null ? '--' : value.toString(16).padStart(2, '0'));
  const name = row.game !== row.name ? ` · ${escapeHtml(row.name)}` : '';
  const notes =
    (row.type_changed ? ' · File type changed' : '') +
    (row.name_changed ? ' · Filename changed' : '');
  const open =
    row.after_index !== null
      ? action(
          'compare-edit',
          'Open current save in hex editor',
          'small',
          `data-index="${row.after_index}"`,
        )
      : '<p class="note">This save is absent from the current draft.</p>';
  const changes = row.changes.length
    ? 'Offset   Reference → Current\n' +
      row.changes
        .map(
          ([offset, before, after]) =>
            `${offset.toString(16).padStart(4, '0')}     ${hexByte(before)} → ${hexByte(after)}`,
        )
        .join('\n')
    : 'No byte differences.';
  const truncated = row.changed_bytes > 512 ? '\nFirst 512 differences shown.' : '';
  return [
    '<details class="section"><summary>',
    `<span class="badge ${row.state === 'Changed' ? 'warning' : ''}">${escapeHtml(row.state)}</span> `,
    `${escapeHtml(row.game)}${name} · ${row.changed_bytes} changed bytes</summary>`,
    `<p class="note">Reference: ${size(row.before_size)} → Current: ${size(row.after_size)}${notes}</p>`,
    open,
    `<pre class="progress-log">${changes}${truncated}</pre>`,
    '</details>',
  ].join('');
}

// ---- Adding discs ----

/** Whether an import entry can be imported (identified, with a destination). */
function importReady(entry) {
  return !entry.blocked && !!entry.category && !!entry.target;
}

/** The Add discs page: the chosen sources, then the scanned import to review. */
function importView() {
  const sources = state.paths.length;
  const sourceList = sources
    ? `<details><summary>Source files and folders</summary><div class="progress-log">${state.paths.map(pathLabel).join('\n')}</div></details>`
    : '<p class="note">Choose ISO/CUE files or a folder. You can also drop files onto this window.</p>';
  const sourcesCard = [
    '<div class="card"><div class="row spread">',
    `<strong>${sources} source${sources === 1 ? '' : 's'} selected</strong>`,
    '<div class="row">',
    checkbox('quick-scan', 'Fast name-only scan', state.prefs.quick),
    action('scan', 'Scan discs', 'primary', !sources ? 'disabled' : ''),
    action('clear-sources', 'Clear', 'quiet'),
    '</div></div>',
    sourceList,
    '</div>',
  ].join('');

  return (
    heading(
      'Add discs',
      'Identify and copy discs into your game library. Original files stay in place.',
      action('import-files', 'Add ISO / CUE…') + action('import-folder', 'Add folder…'),
    ) +
    sourcesCard +
    (state.plan ? importReview(state.plan.entries || []) : '')
  );
}

/** The scanned import: one row per disc, with its identification, category and destination. */
function importReview(entries) {
  const selectedCount = entries.filter((entry) => entry.selected).length;
  const readyCount = entries.filter(importReady).length;
  const allSelected = readyCount > 0 && selectedCount >= readyCount;
  const selectAll = `<input type="checkbox" data-import-all aria-label="Select all discs" ${allSelected ? 'checked' : ''} ${!readyCount ? 'disabled' : ''}>`;
  const row = (entry, index) => {
    const title = escapeHtml(entry.title);
    const badge = entry.blocked ? 'error' : entry.confidence === 'sha1' ? '' : 'warning';
    const categoryChoices = categories
      .map((category) => option(category, category, entry.category === category))
      .join('');
    return [
      '<tr>',
      `<td><input type="checkbox" data-import-check="${index}" aria-label="Import ${title}" ${entry.selected ? 'checked' : ''} ${!importReady(entry) ? 'disabled' : ''}></td>`,
      `<td><strong>${title}</strong><p class="note">${escapeHtml(entry.reason)}</p><span class="badge ${badge}">${escapeHtml(entry.confidence)}</span></td>`,
      `<td><select data-import-category="${index}" aria-label="Category for ${title}">`,
      `<option value="" ${entry.category ? '' : 'selected'} disabled>Choose a category…</option>${categoryChoices}</select></td>`,
      `<td class="path">${pathLabel(entry.target)}</td>`,
      `<td>${action('import-resolve', 'Identify…', 'small', `data-index="${index}" aria-label="Identify ${title}"`)}</td>`,
      '</tr>',
    ].join('');
  };
  return [
    '<div class="row spread section"><h2>Review import</h2>',
    action(
      'import-apply',
      `Import ${selectedCount} selected`,
      'primary',
      !selectedCount ? 'disabled' : '',
    ),
    '</div>',
    '<div class="table-wrap" tabindex="0" role="region" aria-label="Scrollable table"><table>',
    `<thead><tr><th>${selectAll}</th><th>Game / identification</th><th>Category</th><th>Destination</th><th></th></tr></thead>`,
    `<tbody>${entries.map(row).join('')}</tbody>`,
    '</table></div>',
  ].join('');
}

// ---- The Save Manager page ----

/** The Save Manager: slot search, the open backup and its saves, and recovery copies. */
function savesView() {
  const slotSearch = [
    '<div class="card"><div class="row spread"><h3>Find saves across slots</h3><div class="row">',
    action('slots-usb', 'Use USB'),
    action('slots-library', 'Use library'),
    action('slots-folder', 'Choose folder…'),
    action('slots-refresh', 'Refresh', 'small'),
    '</div></div>',
    '<div class="row">',
    `<input class="grow" id="slots-directory" aria-label="Save backups folder" placeholder="Drive/.ohmy3do/saves" value="${escapeHtml(state.slotDirectory || '')}">`,
    `<input id="slot-search" aria-label="Search backup slots" type="search" placeholder="Game, filename, label or notes…" value="${escapeHtml(state.slotQuery)}">`,
    '</div>',
    `<div id="slot-results">${slotResults()}</div>`,
    '</div>',
  ].join('');
  const backup = state.backup
    ? backupView(state.backup)
    : empty(
        'Your console saves, in one place',
        'Open a .3ds backup, search the slots on your drive, or create a draft and combine individual saves from other backups.',
      );
  const recovery = [
    '<div class="card section"><h3>Recovery copies</h3>',
    '<p>Review app-created recovery copies and remove selected older copies to reclaim space.</p>',
    action('recovery-open', 'Manage recovery copies…'),
    '</div>',
  ].join('');

  return (
    heading(
      'Save Manager',
      'Search slots, edit individual saves and assemble backup archives.',
      action('backup-open', 'Open backup…') + action('backup-new', 'New backup', 'primary'),
    ) +
    slotSearch +
    backup +
    recovery
  );
}

/** The open backup: its name and buttons, its saves, and the chosen save's editor. */
function backupView(backup) {
  const unsaved =
    backup.dirty || state.hexDirty ? ' <span class="badge warning">Unsaved changes</span>' : '';
  // Slot files (slotNN.3ds) can be named and have notes.
  const slotButtons = backup.path
    ? action('slot-label', 'Name slot…', 'quiet') +
      (/^slot[0-9]+\.3ds$/i.test(basename(backup.path))
        ? action('slot-notes', 'Notes…', 'quiet')
        : '')
    : '';
  const header = [
    '<div class="row spread section"><div>',
    `<h2>${escapeHtml(basename(backup.path) || 'New backup')}${unsaved}</h2>`,
    `<p class="note">${backup.entries.length} saves · ${bytes(backup.bytes)} of 64 KB · ${pathLabel(backup.path || 'Not saved yet')}</p>`,
    '</div><div class="row">',
    action('backup-save', 'Save', 'primary'),
    action('backup-save-as', 'Save as…'),
    action('backup-merge', 'Add saves from…'),
    action('backup-compare', 'Compare…'),
    slotButtons,
    '</div></div>',
    '<div class="notice">Saving writes the entire destination backup slot. ',
    'Use “Add saves from…” to merge individual saves before saving. Existing archives get a recovery copy.</div>',
  ].join('');
  const saves = backup.entries.length
    ? backup.entries
        .map((save, index) => {
          const active = state.entryIndex === index;
          return [
            `<button class="entry-button${active ? ' active' : ''}" data-entry="${index}" aria-pressed="${active}">`,
            `<strong>${escapeHtml(save.known || save.name)}</strong>`,
            `<small>${escapeHtml(save.name)} · ${bytes(save.size)}</small></button>`,
          ].join('');
        })
        .join('')
    : '<p class="note">This draft is empty. Add saves from another backup to build a slot.</p>';
  const editor = state.entry
    ? saveEditorView(state.entry)
    : '<p>Select a save to inspect or edit it.</p>';
  return `${header}<div class="split"><div class="card entry-list">${saves}</div><div class="card">${editor}</div></div>`;
}

/** The chosen save: its game-specific editor (if its format is known) and the hex editor. */
function saveEditorView(entry) {
  const profile = entry.profile;
  const cheats = profile
    ? [
        `<div class="notice"><strong>${escapeHtml(profile.game)}</strong><p class="note">${escapeHtml(profile.notes)}</p>`,
        '<div class="form-grid">',
        profile.fields
          .map(
            (field, index) =>
              `<label class="field">${escapeHtml(field.label)}<input type="number" data-cheat-field="${index}" min="${field.min}" max="${field.max}" value="${entry.values[index] ?? ''}"></label>`,
          )
          .join(''),
        '</div><div class="row section">',
        profile.presets
          .map((preset, index) =>
            action('cheat-preset', escapeHtml(preset.label), 'small', `data-index="${index}"`),
          )
          .join(''),
        '</div></div>',
      ].join('')
    : '<p class="note">No verified game-specific editor is available for this save. Raw hex editing remains available.</p>';
  // Printable ASCII as itself, everything else as a dot.
  const ascii = entry.data
    .map((code) => (code >= 32 && code <= 126 ? String.fromCharCode(code) : '·'))
    .join('');
  return [
    `<div class="row spread"><h3>${escapeHtml(entry.name)}</h3>${action('backup-remove', 'Remove from draft', 'danger small')}</div>`,
    cheats,
    '<div class="row section">',
    action('backup-export-raw', 'Export raw…', 'small'),
    action('backup-export-one', 'Export this save…', 'small'),
    action('backup-import-raw', 'Import raw bytes…', 'small'),
    '</div>',
    `<details ${!profile ? 'open' : ''}><summary>Raw hex editor · offsets in bytes</summary>`,
    `<div class="row spread"><small>Offset 0000 · ${entry.data.length} bytes</small>${action('hex-apply', 'Apply hex edits', 'small')}</div>`,
    `<textarea id="hex-editor" class="hex" spellcheck="false" aria-label="Save data in hexadecimal">${escapeHtml(entry.hex)}</textarea>`,
    `<div class="ascii">${escapeHtml(ascii)}</div>`,
    '<p class="note">Two hex digits per byte. Apply edits to the draft, then Save to write the archive. ',
    'Game-specific controls also maintain the game’s checksum.</p>',
    '</details>',
  ].join('');
}

/** The slots matching the search, as a table. */
function slotResults() {
  const slots = state.slots.filter((slot) => slotMatches(slot, state.slotQuery));
  if (!slots.length)
    return state.slots.length ? '<p class="note">No matching occupied slots.</p>' : '';

  const row = (slot) => {
    const invalid = !slot.valid ? ' <span class="badge error">Invalid</span>' : '';
    const notes = slot.notes
      ? `<small class="note">${escapeHtml(slot.notes.slice(0, 160))}${slot.notes.length > 160 ? '…' : ''}</small>`
      : '';
    const open = action(
      'slot-open',
      'Open',
      'small',
      `data-path="${escapeHtml(slot.path)}" aria-label="Open ${escapeHtml(slot.name)}" ${!slot.valid ? 'disabled' : ''}`,
    );
    return `<tr><td>${escapeHtml(slot.name)}${invalid}</td><td>${escapeHtml(slot.games)}${notes}</td><td>${open}</td></tr>`;
  };
  return [
    `<details open><summary>${slots.length} occupied slots</summary>`,
    '<div class="table-wrap" tabindex="0" role="region" aria-label="Scrollable table"><table>',
    '<thead><tr><th>Slot</th><th>Games / saves</th><th></th></tr></thead>',
    `<tbody>${slots.map(row).join('')}</tbody>`,
    '</table></div></details>',
  ].join('');
}

/** Applies typed hex edits to the open save, if there are any. */
async function commitHex() {
  if (!state.hexDirty) return;
  state.backup = await call('backup_edit', {
    index: state.entryIndex,
    hex: $('#hex-editor')?.value ?? state.entry.hex,
  });
  state.hexDirty = false;
  state.entry = await call('backup_entry', { index: state.entryIndex });
}

/** Opens a backup (after dealing with unsaved edits) and shows its first save. */
async function openBackup(path) {
  if (!(await resolveDirty())) return;
  state.backup = await call('backup_open', { path });
  state.hexDirty = false;
  state.entryIndex = state.backup.entries.length ? 0 : null;
  state.entry = state.entryIndex === null ? null : await call('backup_entry', { index: 0 });
  state.page = 'saves';
  render();
}

/**
 * Saves the open backup: to where it came from, or (`saveAs`, or a new
 * backup) to a chosen file. Asks first, since the whole slot is replaced.
 * Returns whether it was saved.
 */
async function saveBackup(saveAs = false) {
  await commitHex();
  let path = !saveAs && state.backup?.path;
  if (!path) path = await pick('archive_save', state.slotDirectory || state.prefs.destination);
  if (!path) return false;

  const overwrite = await confirmAction(
    'Save backup archive',
    `Write the complete archive (${state.backup.entries.length} saves) to ${displayPath(path)}? ` +
      'Any existing destination slot is replaced as a whole, with a recovery backup kept.',
    'Save archive',
  );
  if (!overwrite) return false;

  const result = await call('backup_save', { path, overwrite: true });
  state.backup = result.backup;
  const recovery = result.recovery ? `. Recovery copy: ${displayPath(result.recovery)}` : '';
  toast(result.catalog_warning || `Backup saved and verified${recovery}`);
  render();
  return true;
}

// ---- The Artwork & previews page ----

/** Media sources, assigning media to one game, and the video tools. */
function mediaView() {
  const game = state.groups.find((group) => group.discs.includes(state.mediaDisc));
  const sources = [
    '<div class="card"><h3>Media sources</h3>',
    '<div class="row"><label class="field grow">EmuMovies / media folder',
    `<input id="media-source" value="${escapeHtml(state.prefs.emumovies)}" placeholder="Optional local artwork and video folder"></label>`,
    action('media-folder', 'Browse…', '', 'aria-label="Choose media source folder"'),
    '</div>',
    '<p class="note">Media is matched to game titles and converted for the console. ',
    'Prepare missing media also finds missing descriptions and creates their console files.</p>',
    '<div class="row">',
    action(
      'media-prepare',
      'Prepare missing media',
      'primary',
      !state.groups.length ? 'disabled' : '',
    ),
    action('media-rebuild', 'Rebuild existing media…'),
    '</div></div>',
  ].join('');
  const chosenGame = game
    ? [
        '<div class="row section"><div class="grow">',
        `<strong>${escapeHtml(game.title)}</strong><p class="note">${escapeHtml(game.discs.join('\n'))}</p>`,
        '</div><div class="row">',
        action('media-cover', 'Choose cover…'),
        action('media-preview', 'Choose preview video…'),
        '</div></div>',
      ].join('')
    : '';
  const assign = [
    '<div class="card"><h3>Assign media to a game</h3>',
    '<select id="media-game" aria-label="Game to assign media" class="grow">',
    option('', 'Choose a game', !state.mediaDisc),
    state.groups
      .map((group) => option(group.discs[0], group.title, group.discs.includes(state.mediaDisc)))
      .join(''),
    '</select>',
    chosenGame,
    '</div>',
  ].join('');
  const videoTools = [
    '<div class="card"><h3>Video tools</h3><div class="row">',
    `<input id="media-video-tools" aria-label="Video tools folder" class="grow" value="${escapeHtml(state.prefs.video_tools)}" placeholder="Folder containing FFmpeg and ffprobe">`,
    action('video-folder', 'Browse…', '', 'aria-label="Choose video tools folder"'),
    action('video-tools', 'Download tools', '', state.prefs.offline ? 'disabled' : ''),
    '</div>',
    '<p class="note">FFmpeg is downloaded separately and verified against its published checksum.</p>',
    '</div>',
  ].join('');

  return (
    heading(
      'Artwork & previews',
      'Prepare covers, video previews and game descriptions for the console menu.',
    ) +
    sources +
    assign +
    videoTools
  );
}

// ---- Console settings ----

/** The console menu's on/off settings, and their labels. */
const consoleSwitches = [
  ['usb_covers', 'Show covers'],
  ['video_previews', 'Play video previews'],
  ['preview_sound', 'Preview sound'],
  ['remember_position', 'Remember menu position'],
  ['recent_games', 'Show recent games'],
  ['large_titles', 'Large titles'],
];

/** The folder workaround's choices (see console_config.rs). */
const folderWorkarounds = [
  ['0', 'Automatic'],
  ['1', 'Always'],
  ['2', 'Off'],
];

/** The console settings form; its controls' ids start with `prefix`. */
function settingsForm(settings, prefix = 'config') {
  const switches = consoleSwitches
    .map(([key, label]) => checkbox(`${prefix}-${key}`, label, settings[key]))
    .join('');
  const workarounds = folderWorkarounds
    .map(([value, label]) => option(value, label, String(settings.folder_workaround) === value))
    .join('');
  return [
    '<div class="form-grid">',
    switches,
    `<label class="field">Screen margin<input type="number" min="0" max="12" id="${prefix}-screen_margin" value="${settings.screen_margin}"></label>`,
    `<label class="field">Folder workaround<select id="${prefix}-folder_workaround">${workarounds}</select></label>`,
    '</div>',
  ].join('');
}

/**
 * Reads the console settings form (`prefix` as in settingsForm). Settings
 * not on the form keep their loaded (or default) values.
 */
function readSettings(prefix = 'config') {
  const loaded = prefix === 'config' ? state.config?.settings : {};
  const settings = { ...defaults, ...loaded };
  for (const key of Object.keys(settings)) {
    const control = document.getElementById(`${prefix}-${key}`);
    if (control)
      settings[key] = control.type === 'checkbox' ? control.checked : Number(control.value);
  }
  return settings;
}

// ---- The Drive setup page ----

// Removing Oh My 3DO from a drive: review what would go, then confirm. When
// the drive holds save backups, the person picks a PC folder for them or
// explicitly chooses to delete them.
async function reviewDriveCleanup() {
  const root = $('#drive-destination').value || state.prefs.destination;
  if (!root) throw new Error('Choose the USB / SD drive root first.');
  const plan = await call('drive_cleanup_review', { root });
  const fileList = plan.files
    .map((file) => `<li>${pathLabel(file.path)} · ${bytes(file.bytes)}</li>`)
    .join('');
  const kept = plan.kept.length
    ? `<p>Kept because they belong to something else: ${plan.kept.map(escapeHtml).join(', ')}.</p>`
    : '';
  const savesNote = plan.saves.length
    ? `<div class="notice">This drive holds ${quantity(plan.saves.length, 'save backup file')}. Copy them to a folder on the PC first, or remove them with everything else.</div>`
    : '';
  const buttons = plan.saves.length
    ? action('drive-cleanup-skip-saves', 'Remove without keeping saves', 'danger') +
      action('drive-cleanup-keep-saves', 'Copy saves to PC, then remove…', 'primary')
    : action('drive-cleanup-apply', 'Remove menu files', 'danger');
  dialog(
    'Stop using Oh My 3DO on this drive',
    `<p>Permanently remove ${quantity(plan.files.length, 'file')} (${bytes(plan.bytes)}) from ${pathLabel(plan.root)}?</p>
     <p>Games and every other file stay where they are.</p>${kept}${savesNote}
     <div class="notice">This cannot be undone. To use the menu again later, set up the drive again.</div>
     <details class="section"><summary>Review files</summary><ul>${fileList}</ul></details>`,
    action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') + buttons,
  );
}

/**
 * Carries out the reviewed drive cleanup (`choice`: where to keep the
 * saves, or to skip them), then reloads and reports what was removed.
 */
async function applyDriveCleanup(choice) {
  const result = await call('drive_cleanup', choice);
  if (result.backup_closed) {
    state.backup = null;
    state.entry = null;
    state.entryIndex = null;
    state.hexDirty = false;
  }
  state.readiness = null;
  state.copy = null;
  state.coverCache.clear();
  closeDialog();
  await perform(loadLibrary);

  const cleaned = result.cleaned;
  const saved = cleaned.saves_copied_to ? ` Save backups are in ${cleaned.saves_copied_to}.` : '';
  const warnings = cleaned.warnings.length ? ' ' + cleaned.warnings.join(' ') : '';
  toast(
    `${quantity(cleaned.files, 'file')} removed.${saved}${warnings}`,
    cleaned.warnings.length > 0,
  );
}

/** Drive setup: installing the menu, repairing, checking, and removing it. */
function driveView() {
  const developer = state.prefs.developer_mode;
  const install = [
    '<div class="card">',
    '<label class="field">USB / SD destination<div class="row">',
    `<input class="grow" id="drive-destination" value="${escapeHtml(state.prefs.destination)}" placeholder="Choose the drive root">`,
    action('drive-folder', 'Browse…', '', 'aria-label="Choose USB or SD destination"'),
    '</div></label>',
    `<div class="row">${action('drive-name', 'Name / identify drive…')}</div>`,
    '<div class="notice">Existing games, media, settings and save backups are preserved. Missing support files are created automatically.</div>',
    '<div class="row">',
    action(
      'drive-library',
      'Use library boot.iso',
      'primary',
      !state.prefs.library ? 'disabled' : '',
    ),
    action('drive-iso', 'Choose boot.iso…'),
    action(
      'drive-download',
      developer ? 'Install local menu' : 'Download released menu',
      '',
      state.prefs.offline && !developer ? 'disabled' : '',
    ),
    '</div>',
    `<details><summary>Initial menu settings</summary>${settingsForm(defaults, 'drive')}`,
    '<p class="note">Used only when creating new settings.</p></details>',
    '</div>',
  ].join('');
  const repair = [
    '<div class="card"><h3>Repair a drive</h3>',
    '<p>Already have boot.iso? Create missing settings, save folders and search support using the installed image.</p>',
    action('drive-repair', 'Repair installed support'),
    '</div>',
  ].join('');
  const readiness = [
    '<div class="card"><h3>Drive readiness</h3>',
    '<p>Check the installed menu, settings, game indexes, navigation helpers, saves and media before ejecting. ',
    'This check does not change any files.</p>',
    '<div class="row">',
    action('drive-check', 'Check drive', 'primary'),
    action('drive-eject', 'Safely eject drive'),
    action('support-export', 'Export support report…'),
    '</div>',
    '<p class="note">Support reports include drive paths, readiness results, console diagnostics and recent desktop activity. ',
    'They exclude game and save contents.</p>',
    readinessView(),
    '</div>',
  ].join('');
  const cleanup = [
    '<div class="card"><h3>Stop using Oh My 3DO on this drive</h3>',
    '<p>Remove the menu, its settings, media, descriptions and save backups from the drive. ',
    'Your games stay where they are. You can copy the save backups to the PC first.</p>',
    action('drive-cleanup', 'Review cleanup…', 'danger'),
    '</div>',
  ].join('');

  return (
    heading(
      'Set up your drive',
      'Install or repair the menu and Save Manager support without copying games.',
    ) +
    install +
    repair +
    readiness +
    cleanup +
    '<p class="note">To add games, select them in Library and choose Copy selected to USB. Install this drive in the console with the power off.</p>'
  );
}

// ---- The Settings & tools page ----

/** Desktop preferences, console settings, categories, library tools and more. */
function settingsView() {
  const prefs = state.prefs;
  const themeName = (mode) =>
    mode === 'system' ? 'System (follow device theme)' : mode[0].toUpperCase() + mode.slice(1);
  const desktop = [
    '<div class="card"><h3>Desktop preferences</h3><div class="stack">',
    '<label class="field">Appearance<select id="pref-theme">',
    ['system', 'light', 'dark']
      .map((mode) => option(mode, themeName(mode), (prefs.theme || 'system') === mode))
      .join(''),
    '</select></label>',
    '<label class="field">Text size<select id="pref-text-scale">',
    [100, 125, 150, 175, 200]
      .map((size) => option(size, `${size}%`, textScale(prefs.text_scale) === size))
      .join(''),
    '</select></label>',
    checkbox('pref-offline', 'Offline mode — use cached catalog and local media', prefs.offline),
    checkbox('pref-software', 'Software graphics (restart required)', prefs.software_rendering),
    checkbox('pref-updates', 'Check for updates at startup', !prefs.disable_update_checks),
    checkbox(
      'pref-update-notices',
      'Notify when updates are available',
      !prefs.hide_update_notifications,
    ),
    '</div></div>',
  ].join('');
  const configRoot = state.config?.root || prefs.destination || prefs.library;
  const consoleSettings = [
    '<div class="card"><h3>Console menu settings</h3><div class="row">',
    `<input id="config-root" aria-label="Console settings library or drive root" class="grow" value="${escapeHtml(configRoot)}" placeholder="Library or drive root">`,
    action('config-folder', 'Browse…', '', 'aria-label="Choose console settings location"'),
    action('config-load', 'Load settings'),
    '</div>',
    state.config
      ? `<div class="section">${settingsForm(state.config.settings)}</div>${action('config-save', 'Save console settings', 'primary')}`
      : '<p class="note">Load ohmy3do.ini to edit settings for the console menu.</p>',
    '</div>',
  ].join('');
  const cards = [
    '<div class="card"><h3>Categories</h3>',
    '<p>Add or rename category folders, choose import destinations, and control which categories appear on the console. ',
    'The desktop always shows hidden categories.</p>',
    action('categories-open', 'Manage categories…'),
    '</div>',
    '<div class="card"><h3>Library tools</h3><div class="row">',
    action('indexes', 'Rebuild search indexes'),
    action('library-review', 'Create library review'),
    action('undo', 'Undo last change', 'danger'),
    '</div></div>',
    '<div class="card"><h3>Cheat database</h3>',
    '<p>Edit the known save layouts, field addresses and presets shared by the desktop and console.</p>',
    action('cheats-open', 'Open database editor…'),
    '</div>',
    '<div class="card"><h3>Set up a PC staging library</h3>',
    '<p>Choose the library folder at the top of the window, then install its menu support before importing discs.</p>',
    action(
      'staging-setup',
      prefs.developer_mode ? 'Install local menu into library…' : 'Download menu into library…',
    ),
    '</div>',
  ].join('');

  return (
    heading('Settings & tools', 'Desktop preferences, console settings and library maintenance.') +
    desktop +
    developerSettingsView() +
    consoleSettings +
    cards
  );
}

/** Opens the categories editor for a library or drive. */
async function openCategories(root) {
  const result = await call('categories_read', { root });
  state.categoryRoot = result.root;
  state.categoryOriginal = result.config;
  state.categoryDraft = structuredClone(result.config);
  categoriesDialog();
}

/** The most categories a library can have. */
const MAX_CATEGORIES = 32;

/** The categories editor: names, console visibility, and import destinations. */
function categoriesDialog() {
  const config = state.categoryDraft;
  const rows = config.categories
    .map((category, index) =>
      [
        '<div class="row">',
        `<input class="grow" data-category-name="${index}" value="${escapeHtml(category.name)}" maxlength="48" aria-label="Category name">`,
        `<label><input type="checkbox" data-category-hidden="${index}" ${category.hidden ? 'checked' : ''}>Hide on console</label>`,
        action('category-remove', 'Remove', 'danger small', `data-index="${index}"`),
        '</div>',
      ].join(''),
    )
    .join('');
  const destinations = Object.entries(config.defaults)
    .map(([role, id]) => {
      const choices = config.categories
        .map((category) => option(category.id, category.name, category.id === id))
        .join('');
      return `<label class="field">${escapeHtml(role)}<select data-category-role="${escapeHtml(role)}">${choices}</select></label>`;
    })
    .join('');
  const body = [
    '<div class="row spread">',
    `<p>${pathLabel(state.categoryRoot)}</p>`,
    action('categories-folder', 'Choose library / drive…', 'small'),
    '</div>',
    '<p>Hidden categories stay available here. Saving renames their folders and updates library references; ',
    'Undo restores the previous state. Remove a category only after moving its games elsewhere.</p>',
    `<div class="stack">${rows}</div>`,
    '<div class="section">',
    action(
      'category-add',
      'Add category',
      'small',
      config.categories.length >= MAX_CATEGORIES ? 'disabled' : '',
    ),
    '</div>',
    '<h3>Import destinations</h3><p>Choose where each detected disc type is imported.</p>',
    `<div class="form-grid">${destinations}</div>`,
  ].join('');

  dialog(
    'Library categories',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="categories-dialog"') +
      action('categories-save', 'Save categories', 'primary'),
    'categories-dialog',
  );
}

// ---- The Updates page ----

/** The available updates, one card each, with their release notes. */
function updateCards() {
  const updates = state.updates;
  if (!updates)
    return state.prefs?.developer_mode
      ? '<p>Check the local build directory for desktop and menu updates.</p>'
      : '<p>Check GitHub for desktop and menu updates.</p>';

  const notes = (release) =>
    [
      `<details><summary>${release.local ? 'Local build' : 'Release notes'} — ${escapeHtml(release.tag)}</summary>`,
      `<pre class="progress-log">${escapeHtml(release.notes)}</pre></details>`,
    ].join('');
  const cards = [];

  if (updates.desktop && !state.pendingUpdate) {
    const source = updates.local
      ? 'Read the verified local package'
      : 'Download the verified release';
    const buttons = state.pendingUpdate
      ? '<p class="notice">Update downloaded and verified. It will install when you close the app.</p>' +
        action('self-update-apply', 'Restart now', 'primary') +
        action('self-update-cancel', 'Cancel scheduled update')
      : action('self-update', 'Update and restart', 'primary') +
        action('self-update-exit', 'Update on exit');
    cards.push(
      [
        `<div class="card update-card"><h3>Desktop app <span class="badge">${escapeHtml(updates.desktop_release.tag)}</span></h3>`,
        `<p>${source}, install it and restart automatically. Your settings and game library are preserved.</p>`,
        buttons,
        notes(updates.desktop_release),
        '</div>',
      ].join(''),
    );
  }

  for (const target of updates.targets) {
    const difference = target.installed
      ? updates.local
        ? 'The installed menu differs from this local build.'
        : 'The menu image differs from this release.'
      : 'No menu image is installed here.';
    cards.push(
      [
        `<div class="card update-card"><h3>${target.installed ? 'Boot ISO update' : 'Install boot ISO'} `,
        `<span class="badge">${escapeHtml(updates.menu_release.tag)}</span></h3>`,
        `<p>${pathLabel(target.root)}</p>`,
        `<p class="note">${difference} Games and saves are preserved.</p>`,
        action(
          'boot-update',
          'Update boot ISO',
          'primary',
          `data-root="${escapeHtml(target.root)}"`,
        ),
        notes(updates.menu_release),
        '</div>',
      ].join(''),
    );
  }

  const warnings = updates.warnings.length
    ? `<details><summary>Drive check details</summary><p>${updates.warnings.map(escapeHtml).join('<br>')}</p></details>`
    : '';
  return (
    (cards.join('') || '<div class="notice">Everything checked is up to date.</div>') + warnings
  );
}

/** The Updates page. */
function updatesView() {
  const prefs = state.prefs;
  const source = prefs.developer_mode
    ? `Developer mode — local builds from ${prefs.developer_updates_directory || 'no directory selected'}`
    : 'Verified downloads from the Oh My 3DO GitHub releases.';
  return (
    heading(
      'Updates',
      source,
      action(
        'check-updates',
        'Check for updates',
        'primary',
        prefs.offline && !prefs.developer_mode ? 'disabled' : '',
      ),
    ) +
    installedVersionsView() +
    updateResultView() +
    pendingUpdateView() +
    updateCards()
  );
}

/** Installs the offered menu on a drive, after checking the drive and asking. */
async function installBootUpdate(button) {
  const drive = await inspectWriteDrive(button.dataset.root);
  const build = state.updates.local ? 'local build ' : '';
  const confirmed = await confirmAction(
    'Update boot ISO',
    `Replace boot.iso on ${drive.identity?.name || 'unnamed drive'} at ${displayPath(drive.root)} ` +
      `with ${build}${state.updates.menu_release.tag}? Games, saves and settings are preserved.`,
    'Update boot ISO',
  );
  if (!confirmed) return;

  await call('boot_update', { root: button.dataset.root, drive_token: drive.token });
  closeDialog();
  state.page = 'updates';
  render();
  await checkUpdates();
  toast('Boot ISO updated and verified.');
}

/**
 * Checks for updates. An `automatic` check (at startup) only records a
 * failure in Activity, and stays quiet when everything is up to date.
 */
async function checkUpdates(automatic = false) {
  try {
    state.updates = await call('updates_check', { root: versionRoot() });
    await refreshInstalledVersions();
    renderUpdateNotice();
    if (state.page === 'updates') render();
    if (!automatic && !state.updates.desktop && !state.updates.targets.length)
      toast('Everything checked is up to date.');
  } catch (error) {
    if (!automatic) throw error;
    recordActivity(`Automatic update check failed: ${error}`);
  }
}

/** The library or drive whose installed menu version is shown. */
function versionRoot() {
  return (
    state.versionTarget || state.browseRoot || state.prefs.destination || state.prefs.library || ''
  );
}

/** Finds the installed menu version on the chosen library or drive. */
async function refreshInstalledVersions() {
  const root = versionRoot();
  state.installedMenu = null;
  if (!root) return;
  try {
    state.installedMenu = await call('installed_versions', { root });
  } catch (error) {
    state.installedMenu = { root, error: String(error) };
  }
}

/** The installed desktop and menu versions, with a chooser of libraries and drives. */
function installedVersionsView() {
  const menu = state.installedMenu;
  const prefs = state.prefs;
  const roots = [
    ...new Set(
      [
        versionRoot(),
        prefs.library,
        ...(prefs.libraries || []),
        prefs.destination,
        ...(prefs.browser_drives || []),
      ].filter(Boolean),
    ),
  ];
  let label;
  if (!versionRoot()) label = 'Choose a library or drive';
  else if (menu?.error) label = 'Drive unavailable';
  else if (!menu) label = 'Not checked';
  else if (!menu.installed) label = 'Not installed';
  else if (menu.version) label = `v${menu.version}`;
  else label = 'Version unknown — check for updates to identify it';

  return [
    '<div class="card installed-versions"><h3>Installed versions</h3>',
    `<p><strong>Desktop app</strong> · v${escapeHtml(state.version)}</p>`,
    '<label class="field">Menu on library / drive<select id="version-root">',
    roots.map((root) => option(root, displayPath(root), root === versionRoot())).join(''),
    '</select></label>',
    `<p><strong>Console menu</strong> · ${escapeHtml(label)}</p>`,
    menu?.error ? `<p class="note">${escapeHtml(menu.error)}</p>` : '',
    action(
      'versions-refresh',
      'Refresh installed versions',
      'small',
      !versionRoot() ? 'disabled' : '',
    ),
    '</div>',
  ].join('');
}

/** How the last desktop update went, if the app has just been updated. */
function updateResultView() {
  const report = state.updateReport;
  if (!report) return '';
  let outcome;
  if (report.success) outcome = 'The updated app passed its startup check.';
  else if (report.previous_intact)
    outcome = 'The previous version is intact. You can continue using it and try again later.';
  else outcome = 'Recovery could not be confirmed. Keep the update recovery files.';
  return [
    `<div class="card ${report.success ? '' : 'update-failure'}">`,
    `<h3>${report.success ? 'Last desktop update completed' : 'Last desktop update failed'}</h3>`,
    `<p>${outcome}</p>`,
    `<details ${report.success ? '' : 'open'}><summary>Update details</summary>`,
    `<pre class="progress-log">${escapeHtml(report.detail)}</pre>`,
    `<p class="note">Log: ${pathLabel(report.log)}</p></details>`,
    '</div>',
  ].join('');
}

/** A downloaded update waiting to install when the app closes. */
function pendingUpdateView() {
  if (!state.pendingUpdate) return '';
  return [
    `<div class="card"><h3>Desktop v${escapeHtml(state.pendingUpdate.version)} is ready</h3>`,
    '<p>It will install when you close the app. A hidden startup check runs afterward; the app stays closed.</p>',
    action('self-update-apply', 'Restart now', 'primary'),
    action('self-update-cancel', 'Cancel scheduled update'),
    '</div>',
  ].join('');
}

/**
 * The banner above every page: a failed update, an update waiting for
 * exit, or available updates (until dismissed, or if notices are off).
 */
function updateNotice() {
  const report = state.updateReport;
  if (report && !report.success && !state.reportDismissed) {
    const recovery = report.previous_intact
      ? 'The previous version is intact.'
      : 'Recovery needs attention.';
    return `<strong>The last desktop update failed.</strong> ${recovery} ${action('updates-review', 'View details', 'small')}${action('update-notice-dismiss', 'Dismiss', 'quiet small')}`;
  }
  if (state.pendingUpdate)
    return `<strong>Desktop v${escapeHtml(state.pendingUpdate.version)} is ready to install on exit.</strong> ${action('updates-review', 'Review', 'small')}`;
  if (state.prefs.hide_update_notifications) return '';

  const updates = state.updates;
  if (!updates || (!updates.desktop && !updates.targets.length)) return '';
  // A dismissed notice stays hidden until something different is offered.
  const key = JSON.stringify([
    updates.desktop && updates.desktop_release.tag,
    updates.targets.map((target) => target.root),
    updates.menu_release?.tag,
  ]);
  if (state.noticeDismissed === key) return '';

  let label;
  if (updates.desktop && updates.targets.length)
    label = 'Desktop and boot ISO updates are available.';
  else if (updates.desktop) label = 'A desktop update is available.';
  else label = 'A boot ISO update is available.';
  return `<strong>${updates.local ? 'Local build: ' : ''}${label}</strong> ${action('updates-review', 'Review updates', 'small')}${action('update-notice-dismiss', 'Dismiss', 'quiet small')}`;
}

/** Shows the update banner, and the number of updates on the Updates button. */
function renderUpdateNotice() {
  const panel = $('#update-notice');
  if (!panel) return;
  const html = updateNotice();
  panel.innerHTML = html;
  panel.hidden = !html;

  const nav = document.querySelector('[data-page="updates"]');
  const count = state.prefs.hide_update_notifications
    ? 0
    : Number(Boolean(state.updates?.desktop)) + Number(Boolean(state.updates?.targets.length));
  if (nav) {
    if (count) nav.dataset.count = String(count);
    else delete nav.dataset.count;
    nav.setAttribute('aria-label', count ? `Updates, ${count} available` : 'Updates');
  }
}

/**
 * Downloads the offered desktop update, after asking. Installs it now
 * (closing and restarting the app, once open edits are dealt with) or, with
 * `onExit`, when the app closes.
 */
async function prepareDesktopUpdate(onExit) {
  if (!onExit && !(await prepareClose())) return;
  const release = state.updates.desktop_release;
  let question;
  if (state.updates.local)
    question =
      `Install local ${release.tag} from ${displayPath(state.prefs.developer_updates_directory)}` +
      `${onExit ? ' when you close the app' : ' and restart'}? Local builds can replace the same or a newer version.`;
  else if (onExit)
    question = `Download ${release.tag} now and install it when you close Oh My 3DO? You can keep working until then.`;
  else
    question = `Install ${release.tag} and restart Oh My 3DO? Your settings and library stay in place.`;
  const confirmed = await confirmAction(
    onExit ? 'Update on exit' : 'Update and restart',
    question,
    onExit ? 'Download update' : 'Update and restart',
  );
  if (!confirmed) return;

  const result = await call('self_update', { on_exit: onExit });
  if (onExit) {
    state.pendingUpdate = result;
    closeDialog();
    render();
    toast('Update downloaded and verified. It will install when you close the app.');
  }
}

// ---- Button handlers ----
//
// What each `data-action` button does (see the click listener below). A
// handler gets the clicked button, and may return false to mean "nothing
// was done".

const handlers = {
  // -- Drive media updates (from the drive view) --
  'drive-media-update': () => driveMediaUpdate(),
  'drive-media-review': () => reviewDriveMedia(),
  'drive-media-apply': async () => {
    const update = state.driveUpdate;
    if (!update?.plan || update.source !== state.prefs.library)
      throw new Error('Review the update again.');
    await call('copy', { destination: update.root, drive_token: update.token });
    state.driveUpdate = null;
    closeDialog();
    state.coverCache.clear();
    await loadLibrary();
    toast('Drive media, descriptions and information updated.');
  },

  // -- Libraries and drives --
  'library-refresh': async () => {
    state.coverCache.clear();
    await refreshDriveStatus();
    await loadLibrary();
  },
  'view-pc': () => switchLibrary(''),
  'view-drive': async () => {
    if (!state.prefs.destination) throw new Error('Choose a USB / SD drive first.');
    await switchLibrary(state.prefs.destination);
  },
  'add-drive': async () => {
    const folder = await pick('folder', state.prefs.destination);
    if (!folder) return false;
    state.prefs.destination = folder;
    await preferences();
    await refreshDriveStatus();
    if (state.browseRoot) await switchLibrary(folder);
    else await loadLibrary();
    return true;
  },
  'manage-add': async (button) => {
    const kind = button.dataset.kind;
    closeDialog();
    await handlers[kind === 'drives' ? 'add-drive' : 'choose-library']();
    await manageRoots(kind);
  },
  // Takes a library or drive off the saved list (nothing on it changes).
  'forget-root': async (button) => {
    const { kind, root } = button.dataset;
    const prefs = state.prefs;
    if (kind === 'drives') {
      prefs.browser_drives = (prefs.browser_drives || []).filter((drive) => drive !== root);
      if (prefs.destination === root) prefs.destination = prefs.browser_drives[0] || '';
      await preferences();
      if (state.browseRoot === root) await switchLibrary(prefs.destination || '');
    } else {
      if (prefs.library === root && !(await resolveDirty())) return;
      const active = prefs.library === root;
      if (active) {
        rememberLibraryView();
        await flushLibraryView();
      }
      prefs.libraries = (prefs.libraries || []).filter((library) => library !== root);
      if (active) {
        prefs.library = prefs.libraries[0] || '';
        state.coverCache.clear();
      }
      await preferences();
      if (active && !state.browseRoot) await loadLibrary();
    }
    await manageRoots(kind);
  },
  'browse-drive-setup': async () => {
    await switchLibrary('');
    state.page = 'drive';
    render();
  },
  'choose-library': async () => {
    if (!(await resolveDirty())) return;
    const folder = await pick('folder', state.prefs.library);
    if (folder) {
      rememberLibraryView();
      await flushLibraryView();
      state.browseRoot = '';
      state.prefs.library = folder;
      state.coverCache.clear();
      await preferences();
      await loadLibrary();
    }
  },

  // -- Selecting and filtering games --
  'add-discs': () => {
    state.page = 'import';
    render();
  },
  'select-shown': () => {
    for (const group of visibleGames(state.groups, state.filter)) state.selected.add(group.key);
    rememberLibraryView();
    render();
  },
  'clear-selection': () => {
    state.selected.clear();
    rememberLibraryView();
    render();
  },
  'filters-clear': () => {
    state.filter = {
      ...state.filter,
      search: '',
      category: '',
      region: '',
      media: '',
      information: '',
    };
    state.anchor = null;
    rememberLibraryView();
    render();
  },
  'filter-category': (button) => {
    state.filter.category = button.dataset.category;
    state.anchor = null;
    rememberLibraryView();
    render();
  },

  // -- Removing and moving games --
  // Reviews removing the selected games, from the library or the browsed drive.
  'remove-titles': async () => {
    const discs = selectedDiscs(state.groups, state.selected);
    if (!discs.length) return;
    const titles = state.groups
      .filter((group) => state.selected.has(group.key))
      .map((group) => group.title);
    const onDrive = state.browseRoot
      ? { browser_root: state.browseRoot, drive_token: state.browser?.identity?.token }
      : {};
    const plan = await call('titles_remove_review', { discs, ...onDrive });
    state.removal = plan;

    const body = [
      `<p>Permanently delete ${quantity(titles.length, 'title')} (${quantity(plan.discs.length, 'disc')}) from ${pathLabel(plan.root)}?</p>`,
      `<ul>${titles.map((title) => `<li>${escapeHtml(title)}</li>`).join('')}</ul>`,
      `<p>${quantity(plan.files.length, 'file')} · ${bytes(plan.bytes)} to remove, including associated media that no remaining title uses.</p>`,
      '<div class="notice">This cannot be undone. Saves, unrelated files and copies in other libraries are kept.</div>',
      '<details class="section"><summary>Review files</summary><ul>',
      plan.files.map((file) => `<li>${pathLabel(file.path)} · ${bytes(file.bytes)}</li>`).join(''),
      '</ul></details>',
    ].join('');
    dialog(
      'Remove selected titles',
      body,
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('remove-titles-apply', 'Permanently remove', 'danger'),
    );
  },
  // Carries out the reviewed removal. If it fails, the button offers a new review.
  'remove-titles-apply': async () => {
    let result;
    try {
      result = await call('titles_remove');
    } catch (error) {
      const button = document.querySelector('[data-action=remove-titles-apply]');
      if (button) {
        button.dataset.action = 'remove-titles';
        button.textContent = 'Review again';
      }
      throw error;
    }
    state.removal = null;
    state.selected.clear();
    state.coverCache.clear();
    closeDialog();
    await perform(loadLibrary);
    const warnings = result.warnings.length ? ' ' + result.warnings.join(' ') : '';
    toast(`${quantity(result.discs, 'disc')} removed.${warnings}`, result.warnings.length > 0);
  },
  'move-games': async () => {
    const choices = categories.map((category) => option(category, category, false)).join('');
    dialog(
      'Move selected games',
      `<p>Move ${state.selected.size} complete game folders and update their metadata, media links and menu lists.</p>` +
        `<label class="field">Category<select id="move-category">${choices}</select></label>`,
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('move-apply', 'Move folders', 'primary'),
    );
  },
  'move-apply': async () => {
    const category = $('#move-category').value;
    await call('move', { discs: selectedDiscs(state.groups, state.selected), category });
    closeDialog();
    await loadLibrary();
    toast('Game folders moved. Undo is available in Settings & tools.');
  },

  // -- Copying games to a drive --
  'copy-open': () => copyDialog(),
  'copy-destination': async () => {
    const folder = await pick('folder', state.prefs.destination);
    if (folder) {
      state.prefs.destination = folder;
      state.copy = null;
      await preferences();
      renderCopyDialog();
    }
  },
  'copy-preview': async () => {
    if (state.copyRoot !== state.prefs.library)
      throw new Error('Library changed. Select the games again.');
    state.prefs.destination = $('#copy-destination').value;
    state.copyVerify = $('#copy-verify').checked;
    await preferences();
    state.copy = await call('copy_preview', {
      discs: state.copyDiscs,
      destination: state.prefs.destination,
      verify: state.copyVerify,
    });
    renderCopyDialog();
  },
  // Copies, if the library and destination are still the reviewed ones.
  'copy-apply': async () => {
    const changed =
      state.copyRoot !== state.prefs.library ||
      $('#copy-destination').value !== state.prefs.destination;
    if (changed) {
      state.copy = null;
      throw new Error('Folders changed. Review the copy again.');
    }
    await call('copy', {
      destination: state.prefs.destination,
      drive_token: state.copy.drive.token,
    });
    state.copy = null;
    await perform(loadLibrary);
    dialog(
      'USB copy complete',
      '<p>The reviewed files were copied and verified. Existing games, settings and saves were preserved.</p>' +
        `<p>${pathLabel(state.prefs.destination)}</p>`,
      action('close-dialog', 'Close', '', 'data-dialog="main-dialog"') +
        action(
          'drive-eject',
          'Safely eject drive',
          'primary',
          `data-root="${escapeHtml(state.prefs.destination)}"`,
        ),
    );
  },

  // -- Adding discs --
  'import-files': async () => {
    const paths = await pick('discs');
    if (paths) {
      state.paths = [...new Set([...state.paths, ...paths])];
      state.plan = null;
      render();
    }
  },
  'import-folder': async () => {
    const path = await pick('folder');
    if (path) {
      state.paths = [...new Set([...state.paths, path])];
      state.plan = null;
      render();
    }
  },
  'clear-sources': () => {
    state.paths = [];
    state.plan = null;
    render();
  },
  scan: async () => {
    await preferences();
    state.plan = await call('scan', { paths: state.paths });
    render();
    toast('Scan complete. Review identification and selection before importing.');
  },
  'import-apply': async () => {
    const count = state.plan.entries.filter((entry) => entry.selected).length;
    const confirmed = await confirmAction(
      'Import selected discs',
      `Copy ${count} discs into ${state.prefs.library}?`,
      'Import discs',
    );
    if (!confirmed) return;
    await call('import');
    state.paths = [];
    state.plan = null;
    state.page = 'library';
    await loadLibrary();
    toast('Discs imported and search indexes updated.');
  },
  // Lets the person choose a disc's catalog release and category, or
  // import it under a title of their own (unverified).
  'import-resolve': (button) => {
    const index = Number(button.dataset.index);
    const entry = state.plan.entries[index];
    const releases = state.plan.catalog.games
      .map((game, gameIndex) => option(gameIndex, game.release, entry.game === gameIndex))
      .join('');
    const categoryChoices = categories
      .map((category) => option(category, category, category === entry.category))
      .join('');
    const body = [
      `<p>${pathLabel(entry.source)}</p>`,
      '<label class="field">Catalog release<select id="resolve-game">',
      `<option value="">Choose a release…</option>${releases}</select></label>`,
      `<div class="row section"><label class="field grow">Category<select id="resolve-category">${categoryChoices}</select></label></div>`,
      '<details><summary>Import an unverified title</summary>',
      field('Title', 'resolve-title', entry.title),
      field('Region', 'resolve-region', entry.region || 'Unknown'),
      '<p class="note">This accepts your title explicitly without asserting checksum identification.</p>',
      action('import-unverified', 'Accept unverified title', '', 'data-index="' + index + '"'),
      '</details>',
    ].join('');
    dialog(
      'Identify disc',
      body,
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('import-identify', 'Use selected release', 'primary', `data-index="${index}"`),
    );
  },
  'import-identify': async (button) => {
    const value = $('#resolve-game').value;
    if (value === '') throw new Error('Choose a catalog release first.');
    state.plan = await call('edit_import', {
      index: Number(button.dataset.index),
      game: Number(value),
      category: $('#resolve-category').value,
      selected: true,
    });
    closeDialog();
    render();
  },
  'import-unverified': async (button) => {
    state.plan = await call('edit_import', {
      index: Number(button.dataset.index),
      unverified: true,
      title: $('#resolve-title').value,
      region: $('#resolve-region').value,
      category: $('#resolve-category').value,
    });
    closeDialog();
    render();
  },

  // -- Save backups --
  'backup-open': async () => {
    const path = await pick('archive', state.slotDirectory);
    if (path) await openBackup(path);
  },
  'backup-new': async () => {
    if (!(await resolveDirty())) return;
    state.backup = await call('backup_new');
    state.entry = null;
    state.entryIndex = null;
    state.hexDirty = false;
    render();
  },
  'backup-save': () => saveBackup(),
  'backup-save-as': () => saveBackup(true),
  'hex-apply': async () => {
    await commitHex();
    render();
    toast('Hex changes applied to the draft. Save to write the archive.');
  },
  'cheat-preset': async (button) => {
    await commitHex();
    state.backup = await call('backup_edit', {
      index: state.entryIndex,
      preset: Number(button.dataset.index),
    });
    state.entry = await call('backup_entry', { index: state.entryIndex });
    render();
  },
  'backup-remove': async () => {
    const confirmed = await confirmAction(
      'Remove save from draft',
      `Remove ${state.entry.name} from this backup draft?`,
      'Remove save',
      true,
    );
    if (!confirmed) return;
    state.backup = await call('backup_remove', { indices: [state.entryIndex] });
    state.entry = null;
    state.entryIndex = null;
    state.hexDirty = false;
    render();
  },
  'backup-export-raw': () => exportSave(true),
  'backup-export-one': () => exportSave(false),
  'backup-import-raw': async () => {
    await commitHex();
    const path = await pick('raw');
    if (!path) return;
    const confirmed = await confirmAction(
      'Replace save bytes',
      `Replace the raw bytes of ${state.entry.name} in this draft? Its filename and file type stay the same.`,
      'Import bytes',
    );
    if (!confirmed) return;
    state.backup = await call('backup_import_raw', { index: state.entryIndex, path });
    state.entry = await call('backup_entry', { index: state.entryIndex });
    render();
  },
  // Lets the person pick saves from another backup to add to this one.
  'backup-merge': async () => {
    await commitHex();
    const path = await pick('archive', state.slotDirectory);
    if (!path) return;
    const saves = await call('backup_inspect', { path });
    state.mergePath = path;
    const choices = saves
      .map((save) =>
        [
          `<label><input type="checkbox" data-merge-index="${save.index}">`,
          `<span><strong>${escapeHtml(save.known || save.name)}</strong>`,
          `<small> · ${escapeHtml(save.name)} · ${bytes(save.size)}</small></span></label>`,
        ].join(''),
      )
      .join('');
    dialog(
      'Add individual saves',
      '<p>Choose saves to add to this draft. Matching filenames replace that save only; unrelated saves stay in the draft.</p>' +
        `<div class="stack">${choices}</div>`,
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('backup-merge-apply', 'Add selected saves', 'primary'),
    );
  },
  'backup-merge-apply': async () => {
    const indices = [...document.querySelectorAll('[data-merge-index]:checked')].map((input) =>
      Number(input.dataset.mergeIndex),
    );
    if (!indices.length) throw new Error('Choose at least one save.');
    state.backup = await call('backup_merge', { path: state.mergePath, indices });
    closeDialog();
    state.entry = null;
    state.entryIndex = null;
    render();
  },
  'backup-compare': async () => {
    await commitHex();
    const path = await pick('archive', state.slotDirectory);
    if (!path) return;
    const result = await call('backup_compare', { path });
    comparisonDialog(result, path);
  },
  // From a comparison: opens that save in the hex editor.
  'compare-edit': async (button) => {
    state.entryIndex = Number(button.dataset.index);
    state.entry = await call('backup_entry', { index: state.entryIndex });
    closeDialog();
    render();
    const hex = $('#hex-editor');
    if (hex) {
      hex.closest('details').open = true;
      hex.focus();
      hex.scrollIntoView({ block: 'center' });
    }
  },

  // -- Save slots --
  'slots-usb': async () => {
    if (!state.prefs.destination) throw new Error('Choose a USB destination in Drive setup first.');
    state.slotDirectory = state.prefs.destination.replace(/[\\/]$/, '') + '/.ohmy3do/saves';
    await refreshSlots();
  },
  'slots-library': async () => {
    if (!state.prefs.library) throw new Error('Choose a library folder first.');
    state.slotDirectory = state.prefs.library.replace(/[\\/]$/, '') + '/.ohmy3do/saves';
    await refreshSlots();
  },
  'slots-folder': async () => {
    const folder = await pick('folder', state.slotDirectory);
    if (folder) {
      state.slotDirectory = folder;
      await refreshSlots();
    }
  },
  'slots-refresh': async () => {
    state.slotDirectory = $('#slots-directory').value;
    await refreshSlots();
  },
  'slot-open': (button) => openBackup(button.dataset.path),
  'slot-label': () => {
    if (!state.backup.path) throw new Error('Save this draft into a numbered slot first.');
    const found = state.slots.find((slot) => slot.path === state.backup.path);
    dialog(
      'Name backup slot',
      field('Slot label (24 ASCII characters)', 'slot-label-text', found?.label || ''),
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('slot-label-save', 'Save label', 'primary'),
    );
  },
  'slot-label-save': async () => {
    state.slots = await call('slot_label', {
      path: state.backup.path,
      label: $('#slot-label-text').value,
    });
    closeDialog();
    render();
    toast('Slot name saved. Archive contents were preserved.');
  },
  'slot-notes': async () => {
    if (!state.backup?.path) throw new Error('Save the draft into a numbered slot first.');
    const result = await call('slot_notes_read', { path: state.backup.path });
    state.notesOriginal = result.notes;
    dialog(
      'Slot notes',
      '<p>Notes stay beside the archive and do not change console save data.</p>' +
        '<label class="field">Notes (up to 4096 UTF-8 bytes)' +
        `<textarea id="slot-notes-text" rows="7" aria-label="Slot notes">${escapeHtml(result.notes || '')}</textarea></label>`,
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('slot-notes-save', 'Save notes', 'primary'),
    );
  },
  'slot-notes-save': async () => {
    await call('slot_notes_save', {
      path: state.backup.path,
      notes: $('#slot-notes-text').value,
      original: state.notesOriginal,
    });
    closeDialog();
    if (state.slotDirectory) await refreshSlots();
    toast('Slot notes saved. Archive contents were preserved.');
  },

  // -- Game details and information --
  'game-details': () => showGameDetails(),
  'game-info-edit': (button) => openInformationEditor(button.dataset.gameKey),
  'game-info-bulk': () => openBulkInformationEditor(),
  // Saves the information editor. Editing several games at once asks first.
  'game-info-save': async () => {
    const edit = state.informationEditor;
    const patch = informationPatch();
    if (!hasInformationChanges()) {
      state.informationEditor = null;
      closeDialog('game-info-dialog');
      return;
    }
    if (edit.bulk) {
      const fieldNames = Object.keys(patch)
        .map((key) => informationFields.find(([field]) => field === key)[1])
        .join(', ');
      const confirmed = await confirmAction(
        'Apply shared information',
        `Apply ${fieldNames} to ${edit.gameCount} selected games (${quantity(edit.records.length, 'disc')})? ` +
          'Unchecked fields are kept. Blank applied values will clear those fields.',
        'Save information',
      );
      if (!confirmed) return;
    }
    await call('game_info_save', {
      original: edit.records,
      patch,
      restore_fields: [...(edit.restoreFields || [])],
      restore_token: edit.restorePreview?.token,
    });
    state.informationEditor = null;
    closeDialog('game-info-dialog');
    closeDialog();
    await loadLibrary();
    if (!edit.bulk) showGameDetails(edit.key);
    toast('Game information saved. Copy the games to update your console drive.');
  },
  'information-tools': () => informationTools(),
  'information-export': (button) => exportInformation(button.dataset.scope === 'selected'),
  'information-import-preview': () => previewInformationImport(),
  'information-import-save': () => saveInformationImport(),
  'information-matches': () => openInformationMatches(),
  'information-match-game': (button) => openInformationMatch(button.dataset.key),
  'information-match-save': () => saveInformationMatch(),
  'information-backups': () => showInformationBackups(),
  'information-backup-preview': async (button) => {
    const preview = await call('game_info_backup_preview', { name: button.dataset.name });
    showInformationImport(preview, button.dataset.name);
  },
  'game-info-restore': (button) => previewInformationRestore(button.dataset.field),
  'game-info-stage-restore': () => stageInformationRestore(),
  'information-refresh': async () => {
    const confirmed = await confirmAction(
      'Get game information',
      'Match library titles to descriptions and metadata from LaunchBox. ' +
        'The downloaded information stays available offline. Unmatched games keep their existing information.',
      'Get information',
    );
    if (!confirmed) return;
    await call('game_information');
    await loadLibrary();
    toast('Game information updated. Copy selected games to transfer it to the console.');
  },

  // -- Covers and previews --
  'media-selected': async () => {
    const discs = selectedDiscs(state.groups, state.selected);
    if (!discs.length) throw new Error('Select games first.');
    const confirmed = await confirmAction(
      'Prepare selected media',
      `Prepare missing covers, previews and descriptions for ${state.selected.size} selected games ` +
        'using your configured media folder and download preferences?',
      'Prepare media',
    );
    if (!confirmed) return;
    await call('media_selected', { discs });
    await loadLibrary();
    toast('Missing media and available descriptions prepared for the selected games.');
  },

  // -- Naming drives --
  'drive-name': async () => {
    const root = $('#drive-destination').value;
    if (!root) throw new Error('Choose a USB / SD drive first.');
    state.driveInspection = await call('drive_inspect', { root });
    const drive = state.driveInspection;
    const mismatch = drive.mismatch
      ? `<p class="notice warning">A different drive was previously remembered at this location: ${escapeHtml(drive.expected.name)}. Check the drive before remembering this one.</p>`
      : '';
    const identity = drive.identity
      ? 'Drive ID: ' + escapeHtml(drive.identity.id)
      : 'This drive has no identity yet. A small identity file will be created on it.';
    dialog(
      'Name and identify drive',
      `<p>${pathLabel(drive.root)}</p>${mismatch}<p>${identity}</p>` +
        field('Friendly name', 'drive-friendly-name', drive.identity?.name || 'Console SD'),
      action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
        action('drive-name-save', 'Remember this drive', 'primary'),
    );
  },
  'drive-name-save': async () => {
    const drive = state.driveInspection;
    const result = await call('drive_remember', {
      root: drive.root,
      name: $('#drive-friendly-name').value,
      token: drive.token,
    });
    state.prefs = result.prefs;
    closeDialog();
    render();
    toast('Drive remembered as ' + result.identity.name);
  },

  // -- The Artwork & previews page --
  'media-folder': async () => {
    const path = await pick('folder', state.prefs.emumovies);
    if (path) {
      state.prefs.emumovies = path;
      await preferences();
      render();
    }
  },
  'media-prepare': async () => {
    state.prefs.emumovies = $('#media-source').value;
    state.prefs.video_tools = $('#media-video-tools').value;
    await preferences();
    await call('media_update');
    await loadLibrary();
    toast('Missing media and available descriptions prepared.');
  },
  'media-rebuild': async () => {
    const confirmed = await confirmAction(
      'Rebuild media',
      'Reconvert existing media from its available source files? This can take a while.',
      'Rebuild media',
    );
    if (!confirmed) return;
    await call('media_rebuild');
    await loadLibrary();
    toast('Media rebuilt.');
  },
  'media-cover': async () => {
    const cover = await pick('image');
    if (cover) {
      await call('media_update', { disc: state.mediaDisc, cover });
      await loadLibrary();
      toast('Cover prepared.');
    }
  },
  'media-preview': async () => {
    const preview = await pick('video');
    if (preview) {
      await call('media_update', { disc: state.mediaDisc, preview });
      await loadLibrary();
      toast('Preview prepared.');
    }
  },
  'video-folder': async () => {
    const path = await pick('folder', state.prefs.video_tools);
    if (path) {
      state.prefs.video_tools = path;
      await preferences();
      render();
    }
  },
  'video-tools': async () => {
    state.prefs = await call('video_tools');
    render();
    toast('FFmpeg and ffprobe installed and verified.');
  },

  // -- Drive setup, checks and support --
  'support-export': async () => {
    const root = $('#drive-destination').value;
    if (!root) throw new Error('Choose a USB / SD destination first.');
    const target = await pick('support_save');
    if (!target) return;
    await call('support_export', { root, target, activity: state.logs });
    toast('Support report exported and verified. It includes drive paths and diagnostic text.');
  },

  // -- Recovery copies --
  'recovery-open': async () => {
    const driveSaves = state.prefs.destination
      ? `${state.prefs.destination.replace(/[\\/]$/, '')}/.ohmy3do/saves`
      : state.prefs.library;
    const directory = await pick('folder', state.slotDirectory || driveSaves);
    if (!directory) return;
    state.cleanup = await call('recovery_scan', { directory });
    state.cleanupSelected.clear();
    recoveryDialog();
  },
  // Selects every copy except the newest (and newest valid) of each save.
  'recovery-older': () => {
    const older = state.cleanup.copies.filter((copy) => !copy.keep).map((copy) => copy.name);
    state.cleanupSelected = new Set(older);
    recoveryDialog();
  },
  'recovery-clear': () => {
    state.cleanupSelected.clear();
    recoveryDialog();
  },
  'recovery-delete': async () => {
    const selected = [...state.cleanupSelected];
    if (!selected.length) return;
    const total = state.cleanup.copies
      .filter((copy) => state.cleanupSelected.has(copy.name))
      .reduce((sum, copy) => sum + copy.bytes, 0);
    const copies = selected.length === 1 ? 'copy' : 'copies';
    const confirmed = await confirmAction(
      'Delete recovery copies',
      `Permanently delete ${selected.length} selected recovery ${copies} (${bytes(total)})? ` +
        'This cannot be undone. Current slots and console recovery archives are kept.',
      'Delete selected copies',
      true,
    );
    if (!confirmed) return;

    const removed = await call('recovery_remove', { selected });
    const directory = state.cleanup.directory;
    const removedCopies = removed.names.length === 1 ? 'copy' : 'copies';
    const errors = removed.errors.length ? ' ' + removed.errors.join(' ') : '';
    const summary = `${removed.names.length} recovery ${removedCopies} removed; ${bytes(removed.bytes)} reclaimed.${errors}`;
    state.cleanupSelected.clear();
    // Show the folder again as it is now.
    try {
      state.cleanup = await call('recovery_scan', { directory });
      recoveryDialog();
      toast(summary, removed.errors.length > 0);
    } catch (error) {
      state.cleanup = null;
      closeDialog();
      toast(`${summary} Refresh failed: ${error}`, true);
    }
  },

  // -- Ejecting and checking drives --
  // Ejects a drive. Cover loading stops first, since it reads from the drive.
  // If the library or open backup was on it, they are closed.
  'drive-eject': async (button) => {
    if (!(await resolveDirty())) return;
    const root = button.dataset.root || $('#drive-destination')?.value || state.prefs.destination;
    if (!root) throw new Error('Choose the USB / SD drive root first.');
    await flushLibraryView();
    coverObserver?.disconnect();
    coverQueue.length = 0;

    let result;
    try {
      result = await call('drive_eject', { root });
    } catch (error) {
      loadCovers();
      throw error;
    }

    if (result.backup_closed) {
      state.backup = null;
      state.entry = null;
      state.entryIndex = null;
      state.hexDirty = false;
    }
    if (result.library_closed || state.browser?.root === result.root) {
      state.browser = null;
      state.browseRoot = '';
      state.records = [];
      state.groups = [];
      state.selected.clear();
      state.libraryRoot = null;
    }
    state.coverCache.clear();
    state.readiness = null;
    state.copy = null;
    closeDialog();
    if (result.library_closed) {
      render();
    } else {
      await perform(loadLibrary);
    }
    await perform(refreshDriveStatus);
    toast(result.message);
  },
  'drive-check': async () => {
    const root = $('#drive-destination').value;
    if (!root) throw new Error('Choose a USB / SD destination first.');
    state.prefs.destination = root;
    await preferences();
    const report = await call('drive_check', { root });
    state.readiness = { ...report, requested_root: root };
    render();
  },
  'drive-folder': async () => {
    const folder = await pick('folder', state.prefs.destination);
    if (folder) {
      state.prefs.destination = folder;
      await preferences();
      render();
    }
  },

  // -- Installing the menu --
  'drive-library': () => setupDrive(`${state.prefs.library.replace(/[\\/]$/, '')}/boot.iso`),
  'drive-iso': async () => {
    const path = await pick('iso', state.prefs.library);
    if (path) await setupDrive(path);
  },
  'drive-download': () => setupDrive(null, true),
  'drive-cleanup': reviewDriveCleanup,
  'drive-cleanup-keep-saves': async () => {
    const folder = await pick('folder');
    if (folder) await applyDriveCleanup({ saves_to: folder });
  },
  'drive-cleanup-skip-saves': () => applyDriveCleanup({ skip_saves: true }),
  'drive-cleanup-apply': () => applyDriveCleanup({}),
  // Repairs a drive with the boot.iso already on it.
  'drive-repair': () => {
    const root = $('#drive-destination').value || state.prefs.destination;
    return setupDrive(`${root.replace(/[\\/]$/, '')}/boot.iso`);
  },
  'staging-setup': async () => {
    if (!state.prefs.library) throw new Error('Choose a library folder first.');
    const source = state.prefs.developer_mode
      ? 'Install local'
      : 'Download and install the released';
    const confirmed = await confirmAction(
      'Set up staging library',
      `${source} menu support in ${state.prefs.library}?`,
      'Install menu support',
    );
    if (!confirmed) return;
    await call('drive_setup', {
      destination: state.prefs.library,
      release: true,
      settings: defaults,
    });
    toast('Staging library prepared. Add discs from Library.');
  },

  // -- Categories --
  'categories-open': async () => {
    const root = state.prefs.library || state.prefs.destination || (await pick('folder'));
    if (root) await openCategories(root);
  },
  'categories-folder': async () => {
    const edited = JSON.stringify(state.categoryDraft) !== JSON.stringify(state.categoryOriginal);
    if (edited) {
      const confirmed = await confirmAction(
        'Discard category edits?',
        'Open another library or drive without saving these category changes?',
        'Discard changes',
      );
      if (!confirmed) return;
    }
    const root = await pick('folder', state.categoryRoot);
    if (root) await openCategories(root);
  },
  'category-add': () => {
    state.categoryDraft.categories.push({
      id: 'custom-' + Date.now().toString(36),
      name: 'New category',
      hidden: false,
    });
    categoriesDialog();
  },
  // Removes a category; disc types imported into it go to the first one instead.
  'category-remove': (button) => {
    const list = state.categoryDraft.categories;
    if (list.length === 1) throw new Error('Keep at least one category.');
    const index = Number(button.dataset.index);
    const [removed] = list.splice(index, 1);
    const destinations = state.categoryDraft.defaults;
    for (const role of Object.keys(destinations))
      if (destinations[role] === removed.id) destinations[role] = list[0].id;
    categoriesDialog();
  },
  'categories-save': async () => {
    if (!(await resolveDirty())) return;
    const confirmed = await confirmAction(
      'Save categories',
      'Apply category names, visibility and import destinations? Renamed folders and references will be updated.',
      'Save categories',
    );
    if (!confirmed) return;
    await call('categories_save', {
      root: state.categoryRoot,
      original: state.categoryOriginal,
      config: state.categoryDraft,
    });
    $('#categories-dialog').close();
    state.plan = null;
    state.copy = null;
    state.coverCache.clear();
    await loadLibrary();
    toast('Categories saved. Undo is available in Settings & tools.');
  },

  // -- Console settings and library tools --
  'config-folder': async () => {
    const folder = await pick('folder', $('#config-root').value);
    if (folder) $('#config-root').value = folder;
  },
  'config-load': async () => {
    state.config = await call('config_load', { root: $('#config-root').value });
    render();
  },
  'config-save': async () => {
    await call('config_save', { settings: readSettings() });
    toast('Console settings saved and verified.');
  },
  indexes: async () => {
    await call('indexes');
    toast('Search, metadata and save-name indexes rebuilt.');
  },
  'library-review': async () => {
    const report = await call('review');
    dialog(
      'Library review',
      `<p>The review was written to:</p><code>${pathLabel(report.path)}</code>`,
      action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
    );
  },
  undo: async () => {
    const confirmed = await confirmAction(
      'Undo last change',
      'Restore the files from the last operation’s undo journal?',
      'Undo change',
      true,
    );
    if (!confirmed) return;
    await call('undo');
    await loadLibrary();
    toast('Last change undone.');
  },

  // -- The cheat database editor --
  'cheats-open': async () => {
    const database = await call('cheats_load');
    state.cheats = JSON.stringify(database, null, 2);
    state.cheatsDirty = false;
    cheatDialog();
  },
  'cheats-load': async () => {
    if (state.cheatsDirty) {
      const confirmed = await confirmAction(
        'Discard database edits',
        'Discard unsaved cheat database edits?',
        'Discard edits',
        true,
      );
      if (!confirmed) return;
    }
    const path = await pick('json');
    if (path) {
      state.cheats = JSON.stringify(await call('cheats_load', { path }), null, 2);
      state.cheatsDirty = false;
      cheatDialog();
    }
  },
  // Adds the official database's new profiles to the one being edited.
  'cheats-download': async () => {
    if (state.cheatsDirty)
      throw new Error('Save the current database edits before merging updates.');
    state.cheats = JSON.stringify(await call('cheats_download'), null, 2);
    state.cheatsDirty = true;
    cheatDialog();
  },
  'cheats-save': async () => {
    const json = $('#cheats-json').value;
    const path = await pick('json_save', state.prefs.library);
    if (path) {
      await call('cheats_save', { json, path });
      state.cheats = json;
      state.cheatsDirty = false;
      toast('Database validated and saved.');
    }
  },
  'cheats-install': async () => {
    const json = $('#cheats-json').value;
    const root = await pick('folder', state.prefs.destination || state.prefs.library);
    if (root) {
      await call('cheats_install', { root, json });
      state.cheatsDirty = false;
      toast('Validated database installed for the console.');
    }
  },

  // -- Updates --
  'developer-folder': async () => {
    const folder = await pick('folder', state.prefs.developer_updates_directory);
    if (folder) {
      state.prefs.developer_updates_directory = folder;
      await developerPreferences();
    }
  },
  // On narrow windows, the selection buttons open in a dialog.
  'selection-actions': () => {
    if (state.selected.size)
      dialog(
        'Selected games',
        `<p>${quantity(state.selected.size, 'game')} selected</p><div class="row">${selectionButtonsView()}</div>`,
        action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
      );
  },
  'check-updates': () => checkUpdates(),
  'self-update': () => prepareDesktopUpdate(false),
  'self-update-exit': () => prepareDesktopUpdate(true),
  'self-update-apply': async () => {
    if (!(await prepareClose())) return;
    await call('self_update_apply');
  },
  'self-update-cancel': async () => {
    await call('self_update_cancel');
    state.pendingUpdate = null;
    render();
    toast('Scheduled update canceled. The installed app is unchanged.');
  },
  'updates-review': async () => {
    state.page = 'updates';
    await refreshInstalledVersions();
    render();
  },
  'versions-refresh': async () => {
    await refreshInstalledVersions();
    render();
  },
  // Hides the banner until something different is offered (see updateNotice).
  'update-notice-dismiss': () => {
    state.reportDismissed = true;
    const updates = state.updates;
    if (updates)
      state.noticeDismissed = JSON.stringify([
        updates.desktop && updates.desktop_release.tag,
        updates.targets.map((target) => target.root),
        updates.menu_release?.tag,
      ]);
    renderUpdateNotice();
  },
  'boot-update': installBootUpdate,

  // -- Operations, Activity and dialogs --
  // Asks the running operation to stop; it finishes its current step first.
  cancel: async () => {
    for (const button of document.querySelectorAll('[data-action="cancel"]')) {
      button.disabled = true;
      button.textContent = 'Canceling…';
    }
    await invoke('cancel');
    if (state.busy) setStatus('Cancel requested. Finishing the current step…');
  },
  logs: () =>
    dialog(
      'Activity',
      `<div class="progress-log">${state.logs.map(escapeHtml).join('\n') || 'No operations yet.'}</div>`,
      action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
    ),
  // Closes a dialog, first asking about unsaved edits in the editors. A
  // dialog whose operation is running stays open.
  'close-dialog': async (button) => {
    const id = button.dataset.dialog || 'main-dialog';
    if (document.getElementById(id)?.dataset.operationBusy === 'true') return;
    if (id === 'game-info-dialog') {
      if (state.busy) return;
      if (hasInformationChanges()) {
        const confirmed = await confirmAction(
          'Unsaved game information',
          'Discard your unsaved information edits?',
          'Discard edits',
          true,
        );
        if (!confirmed) return;
      }
      state.informationEditor = null;
    }
    if (id === 'cheats-dialog' && state.cheatsDirty) {
      const confirmed = await confirmAction(
        'Discard database edits',
        'Discard unsaved cheat database edits?',
        'Discard edits',
        true,
      );
      if (!confirmed) return;
      state.cheatsDirty = false;
    }
    closeDialog(id);
  },
};

/** Exports the open save: its raw bytes, or as a one-save archive. */
async function exportSave(raw) {
  await commitHex();
  const path = await pick(raw ? 'raw_save' : 'archive_save');
  if (!path) return;
  const confirmed = await confirmAction(
    'Export selected save',
    `Write ${state.entry.name} to ${displayPath(path)}? An existing export will get a recovery copy.`,
    'Export save',
  );
  if (!confirmed) return;
  await call('backup_export', { index: state.entryIndex, path, raw, overwrite: true });
  toast('Selected save exported and verified.');
}

/** Lists the save slots in the chosen folder. */
async function refreshSlots() {
  if (!state.slotDirectory) throw new Error('Choose the .ohmy3do/saves folder first.');
  state.slots = await call('slots', { directory: state.slotDirectory });
  render();
}

/**
 * Installs the menu on the drive in Drive setup: from a boot.iso (`source`)
 * or the latest release (`release`), with the initial settings from the
 * form. Checks the drive and asks first.
 */
async function setupDrive(source, release = false) {
  state.prefs.destination = $('#drive-destination').value;
  if (!state.prefs.destination) throw new Error('Choose a USB / SD destination first.');
  const settings = readSettings('drive');
  await preferences();
  const drive = await inspectWriteDrive(state.prefs.destination);
  const confirmed = await confirmAction(
    'Prepare drive',
    `Prepare ${drive.identity?.name || 'unnamed drive'} at ${displayPath(drive.root)}? Existing games and saves are preserved.`,
    'Prepare drive',
  );
  if (!confirmed) return;
  await call('drive_setup', {
    destination: state.prefs.destination,
    source,
    release,
    settings,
    drive_token: drive.token,
  });
  toast('Drive prepared and verified. You can use Save Manager without installing games.');
}

// ---- The cheat database editor ----
//
// The editor works on `state.cheats`, the database as JSON text. The form
// edits a parsed copy (`databaseDraft`) and writes it back (`writeDraft`);
// the Advanced JSON box edits the text directly.

/** The database being edited, parsed. */
function databaseDraft() {
  return JSON.parse(state.cheats);
}

/** Stores an edited database (and shows it in the JSON box). */
function writeDraft(database) {
  state.cheats = JSON.stringify(database, null, 2);
  state.cheatsDirty = true;
  const textarea = $('#cheats-json');
  if (textarea) textarea.value = state.cheats;
}

/** A field's columns in the editor: [key, value shown]. */
function cheatFieldCells(field) {
  return [
    ['label', field.label],
    ['offset', '0x' + field.offset.toString(16)],
    ['width', field.width ?? 4],
    ['min', field.min],
    ['max', field.max],
    ['mask', '0x' + (field.mask ?? 4294967295).toString(16)],
  ];
}

/** The chosen profile's form: names, layout, fields and presets. */
function cheatProfileEditor(profile) {
  const textInput = (label, key) =>
    `<label class="field">${label}<input data-profile-key="${key}" value="${escapeHtml(profile[key] || '')}"></label>`;
  const details = [
    '<div class="form-grid">',
    `<label class="field">Game<input data-profile-key="game" value="${escapeHtml(profile.game)}"></label>`,
    `<label class="field">Save filename<input data-profile-key="filename" value="${escapeHtml(profile.filename)}"></label>`,
    '</div>',
    '<details><summary>Layout, source and checksum</summary><div class="form-grid">',
    ['id', 'encoding', 'source', 'notes'].map((key) => textInput(escapeHtml(key), key)).join(''),
    `<label class="field">Save size (bytes)<input data-profile-key="size" type="number" value="${profile.size}"></label>`,
    '</div>',
    '<p class="note">Signature and checksum algorithms are preserved. ',
    'Edit their exact structure in Advanced JSON when defining a new format.</p>',
    '</details>',
  ].join('');

  const fieldRows = profile.fields
    .map((field, index) => {
      const cells = cheatFieldCells(field)
        .map(
          ([key, value]) =>
            `<td><input data-db-field="${index}" data-key="${key}" value="${escapeHtml(value)}" aria-label="${escapeHtml(key)}"></td>`,
        )
        .join('');
      const remove = action(
        'cheats-remove-field',
        '×',
        'danger small',
        `data-index="${index}" aria-label="Remove field"`,
      );
      return `<tr>${cells}<td>${remove}</td></tr>`;
    })
    .join('');
  const fields = [
    `<div class="row spread section"><h3>Fields</h3>${action('cheats-add-field', 'Add field', 'small')}</div>`,
    '<div class="table-wrap cheat-fields"><table>',
    '<thead><tr><th>Name</th><th>Offset</th><th>Bytes</th><th>Minimum</th><th>Maximum</th><th>Mask</th><th></th></tr></thead>',
    `<tbody>${fieldRows}</tbody>`,
    '</table></div>',
  ].join('');

  const presetRows = profile.presets
    .map((preset, index) => {
      const fieldChoices = profile.fields
        .map((field, fieldIndex) => option(fieldIndex, field.label, preset.field === fieldIndex))
        .join('');
      return [
        '<div class="row">',
        `<input class="grow" data-db-preset="${index}" data-key="label" value="${escapeHtml(preset.label)}" aria-label="Preset name">`,
        `<select data-db-preset="${index}" data-key="field" aria-label="Preset field">${fieldChoices}</select>`,
        `<input class="preset-value" type="number" data-db-preset="${index}" data-key="value" value="${preset.value}" aria-label="Preset value">`,
        action(
          'cheats-remove-preset',
          '×',
          'danger small',
          `data-index="${index}" aria-label="Remove preset"`,
        ),
        '</div>',
      ].join('');
    })
    .join('');
  const presets = [
    `<div class="row spread section"><h3>Presets</h3>${action('cheats-add-preset', 'Add preset', 'small')}</div>`,
    `<div class="stack">${presetRows || '<small>No presets yet.</small>'}</div>`,
  ].join('');

  return details + fields + presets;
}

/** Draws the cheat database editor for the chosen profile. */
function cheatDialog() {
  const database = databaseDraft();
  const lastProfile = Math.max(0, database.profiles.length - 1);
  state.cheatProfile = Math.min(state.cheatProfile || 0, lastProfile);
  const profile = database.profiles[state.cheatProfile];

  const profileChoices = database.profiles
    .map((each, index) => option(index, each.game, index === state.cheatProfile))
    .join('');
  const body = [
    '<p>Edit known save formats and presets. ',
    'These definitions do not change your save archives until you apply a cheat in Save Manager.</p>',
    '<div class="row">',
    action('cheats-load', 'Open JSON…'),
    action(
      'cheats-download',
      'Merge published profiles',
      '',
      state.prefs.offline ? 'disabled' : '',
    ),
    action('cheats-validate', 'Validate'),
    '</div>',
    '<div class="row section">',
    `<select id="cheat-profile" class="grow" aria-label="Game profile">${profileChoices}</select>`,
    action('cheats-new-profile', 'Add profile…', 'small'),
    action('cheats-remove-profile', 'Remove profile', 'danger small', !profile ? 'disabled' : ''),
    '</div>',
    profile ? cheatProfileEditor(profile) : '',
    '<details class="section"><summary>Advanced JSON — signatures, checksums and multi-field presets</summary>',
    `<textarea id="cheats-json" class="json-editor" spellcheck="false" aria-label="Cheat database JSON">${escapeHtml(state.cheats)}</textarea>`,
    action('cheats-apply-json', 'Apply JSON to editor', 'small'),
    '</details>',
  ].join('');

  dialog(
    'Save cheat database',
    body,
    action('close-dialog', 'Close', '', 'data-dialog="cheats-dialog"') +
      action('cheats-use', 'Use in Save Manager') +
      action('cheats-save', 'Save JSON…') +
      action('cheats-install', 'Install to drive…', 'primary'),
    'cheats-dialog',
  );
}

// The cheat editor's own buttons.
Object.assign(handlers, {
  'cheats-validate': async () => {
    await call('cheats_validate', { json: $('#cheats-json').value });
    toast('Database structure, addresses, ranges and checksums validated.');
  },
  // Uses the edited definitions in the Save Manager for this session.
  'cheats-use': async () => {
    await call('cheats_activate', { json: $('#cheats-json').value });
    if (state.entryIndex !== null && state.backup) {
      state.entry = await call('backup_entry', { index: state.entryIndex });
      render();
    }
    toast('Validated definitions are active in Save Manager for this session.');
  },
  'cheats-apply-json': () => {
    const database = JSON.parse($('#cheats-json').value);
    if (!Array.isArray(database.profiles)) throw new Error('Database needs a profiles array.');
    state.cheats = JSON.stringify(database, null, 2);
    state.cheatsDirty = true;
    cheatDialog();
  },
  // Adds a placeholder profile to fill in.
  'cheats-new-profile': () => {
    const database = databaseDraft();
    database.profiles.push({
      id: 'custom-' + Date.now(),
      game: 'New game',
      filename: 'SaveName',
      size: 4,
      encoding: 'be',
      source: 'https://example.com/save-format',
      notes: 'Replace with a verified save layout before use.',
      signature: { offset: 0, width: 1, value: 0 },
      checksum: { algorithm: 'none', offset: 4, seed: 0 },
      fields: [{ label: 'Value', offset: 1, width: 1, min: 0, max: 255, mask: 255 }],
      presets: [],
    });
    state.cheatProfile = database.profiles.length - 1;
    writeDraft(database);
    cheatDialog();
  },
  'cheats-remove-profile': async () => {
    const confirmed = await confirmAction(
      'Remove game profile',
      'Remove this profile from the draft database?',
      'Remove profile',
      true,
    );
    if (!confirmed) return;
    const database = databaseDraft();
    database.profiles.splice(state.cheatProfile, 1);
    state.cheatProfile = 0;
    writeDraft(database);
    cheatDialog();
  },
  'cheats-add-field': () => {
    const database = databaseDraft();
    database.profiles[state.cheatProfile].fields.push({
      label: 'New field',
      offset: 0,
      width: 1,
      min: 0,
      max: 255,
      mask: 255,
    });
    writeDraft(database);
    cheatDialog();
  },
  // Removes a field, the presets that use it, and renumbers the rest.
  'cheats-remove-field': (button) => {
    const database = databaseDraft();
    const profile = database.profiles[state.cheatProfile];
    const index = Number(button.dataset.index);
    profile.fields.splice(index, 1);
    profile.presets = profile.presets.filter(
      (preset) =>
        preset.field !== index &&
        !(preset.additional || []).some((change) => change.field === index),
    );
    for (const preset of profile.presets) {
      if (preset.field > index) preset.field--;
      for (const change of preset.additional || []) if (change.field > index) change.field--;
    }
    writeDraft(database);
    cheatDialog();
  },
  'cheats-add-preset': () => {
    const database = databaseDraft();
    const profile = database.profiles[state.cheatProfile];
    if (!profile.fields.length) throw new Error('Add a field first.');
    profile.presets.push({ label: 'New preset', field: 0, value: profile.fields[0].min });
    writeDraft(database);
    cheatDialog();
  },
  'cheats-remove-preset': (button) => {
    const database = databaseDraft();
    database.profiles[state.cheatProfile].presets.splice(Number(button.dataset.index), 1);
    writeDraft(database);
    cheatDialog();
  },
});

// ---- Clicks, typing and changes ----

/** Buttons that edit or copy games, which only work in the PC library. */
const libraryOnlyActions = [
  'add-discs',
  'copy-open',
  'move-games',
  'move-apply',
  'game-info-edit',
  'game-info-bulk',
  'game-info-save',
  'information-refresh',
  'information-tools',
  'media-selected',
];

// Clicks: a `data-action` button runs its handler; a sidebar button opens
// its page; a game toggles its selection (Shift+click selects a range); a
// save in the open backup is shown.
document.addEventListener('click', (event) => {
  const button = event.target.closest('[data-action]');
  if (button) {
    event.preventDefault();
    const handler = handlers[button.dataset.action];
    if (handler)
      perform(() => {
        if (state.browseRoot && libraryOnlyActions.includes(button.dataset.action))
          throw new Error('Switch to your PC library to edit or copy games.');
        return handler(button);
      });
    return;
  }

  const nav = event.target.closest('[data-page]');
  if (nav) {
    perform(async () => {
      if (state.hexDirty) await commitHex();
      if (nav.dataset.page === 'updates')
        state.versionTarget = state.browseRoot || state.prefs.destination || state.prefs.library;
      // Other pages work on the PC library, so leave the drive view.
      if (state.browseRoot && nav.dataset.page !== 'library') {
        await switchLibrary('');
      }
      state.page = nav.dataset.page;
      if (state.page === 'updates') await refreshInstalledVersions();
      render();
    });
    return;
  }

  const tile = event.target.closest('[data-game]');
  if (tile) {
    const visible = visibleGames(state.groups, state.filter);
    const index = visible.findIndex((group) => group.key === tile.dataset.game);
    if (event.shiftKey && state.anchor !== null) {
      const first = Math.min(index, state.anchor);
      const last = Math.max(index, state.anchor);
      for (const group of visible.slice(first, last + 1)) state.selected.add(group.key);
    } else if (!state.selected.delete(tile.dataset.game)) state.selected.add(tile.dataset.game);
    state.anchor = index;
    state.focusedGame = tile.dataset.game;
    rememberLibraryView();
    render();
    announce(`${state.selected.size} games selected`);
    return;
  }

  const entry = event.target.closest('[data-entry]');
  if (entry)
    perform(async () => {
      await commitHex();
      state.entryIndex = Number(entry.dataset.entry);
      state.entry = await call('backup_entry', { index: state.entryIndex });
      render();
    });
});

// Typing: searches, editors and fields that update as they change.
document.addEventListener('input', (event) => {
  const input = event.target;

  // The game information dialogs.
  if (input.id === 'information-match-query' || input.id === 'information-match-all')
    renderInformationMatchList();
  if (input.id === 'information-choice-query') renderInformationChoices();
  if (input.dataset.infoApply !== undefined) {
    // In the bulk editor, a field is only edited (and applied) when checked.
    const field = $(`#game-info-${input.dataset.infoApply}`);
    if (field) field.disabled = !input.checked;
    markInformationRestores();
  }
  if (input.dataset.infoImport !== undefined) updateInformationImportButton();
  if (input.id.startsWith('game-info-') && state.informationEditor) {
    // Typing over a field cancels restoring its downloaded value.
    state.informationEditor.restoreFields?.delete(input.id.slice('game-info-'.length));
    markInformationRestores();
  }

  // Searching the library redraws it, keeping the cursor in the search box.
  if (input.id === 'library-search') {
    const cursor = input.selectionStart;
    state.filter.search = input.value;
    rememberLibraryView();
    render();
    $('#library-search').focus();
    $('#library-search').setSelectionRange(cursor, cursor);
  }
  if (input.id === 'slot-search') {
    state.slotQuery = input.value;
    $('#slot-results').innerHTML = slotResults();
  }

  if (input.dataset.categoryName !== undefined)
    state.categoryDraft.categories[Number(input.dataset.categoryName)].name = input.value;
  // A different drive makes the last drive check stale.
  if (input.id === 'drive-destination') {
    state.readiness = null;
    $('#readiness-results')?.remove();
  }
  if (input.dataset.recoveryCopy !== undefined) {
    if (input.checked) state.cleanupSelected.add(input.dataset.recoveryCopy);
    else state.cleanupSelected.delete(input.dataset.recoveryCopy);
    recoveryDialog();
  }
  if (input.id === 'hex-editor') state.hexDirty = true;
  if (input.id === 'cheats-json') {
    state.cheats = input.value;
    state.cheatsDirty = true;
  }

  // Changing the copy destination makes the copy review stale: the Copy
  // button is disabled and a Review button offered again.
  if (input.id === 'copy-destination' && state.copy) {
    state.copy = null;
    const apply = document.querySelector('[data-action="copy-apply"]');
    if (apply) {
      apply.disabled = true;
      apply.textContent = 'Destination changed — review again';
    }
    const body = input.closest('.dialog-body');
    if (!document.querySelector('[data-action="copy-preview"]')) {
      const review = document.createElement('button');
      review.dataset.action = 'copy-preview';
      review.textContent = 'Review copy';
      body.append(review);
    }
  }
});

/** The library's filter selects, and the filter each sets. */
const filterSelects = {
  'library-search-scope': 'searchScope',
  'library-information': 'information',
  'library-media': 'media',
  'library-region': 'region',
};

/** Choosing a PC library in the top bar (or adding or managing them). */
async function choosePcLibrary(select) {
  const value = select.value;
  updateLocation();
  if (value === 'add') {
    await handlers['choose-library']();
    return;
  }
  if (value === 'manage') {
    await manageRoots('libraries');
    return;
  }
  if (!value || value === state.prefs.library || !(await resolveDirty())) return;
  rememberLibraryView();
  await flushLibraryView();
  state.prefs.library = value;
  state.coverCache.clear();
  await preferences();
  if (!state.browseRoot) await loadLibrary();
}

/** Choosing a drive in the top bar (or adding or managing them). */
async function chooseDrive(select) {
  const value = select.value;
  updateLocation();
  if (value === 'add') {
    await handlers['add-drive']();
    return;
  }
  if (value === 'manage') {
    await manageRoots('drives');
    return;
  }
  if (!value || value === state.prefs.destination) return;
  state.prefs.destination = value;
  await preferences();
  if (state.browseRoot) await switchLibrary(value);
  else await loadLibrary();
}

/** Applies a change made in the cheat database editor's form. */
function changeCheatDatabase(control) {
  const database = databaseDraft();
  const profile = database.profiles[state.cheatProfile];
  // Names are text; everything else is a number.
  const value = (key) => (key === 'label' ? control.value : Number(control.value));
  if (control.dataset.profileKey) {
    const key = control.dataset.profileKey;
    profile[key] = key === 'size' ? Number(control.value) : control.value;
  }
  if (control.dataset.dbField !== undefined)
    profile.fields[Number(control.dataset.dbField)][control.dataset.key] = value(
      control.dataset.key,
    );
  if (control.dataset.dbPreset !== undefined)
    profile.presets[Number(control.dataset.dbPreset)][control.dataset.key] = value(
      control.dataset.key,
    );
  writeDraft(database);
}

// Changes: choosers, filters, preferences, and form fields that save as
// soon as they change.
document.addEventListener('change', (event) => {
  const control = event.target;
  perform(async () => {
    if (control.id === 'pc-library') return choosePcLibrary(control);
    if (control.id === 'usb-drive') return chooseDrive(control);

    // Dialog editors.
    if (control.id === 'information-choice') renderInformationMatchPreview();
    if (control.dataset.categoryHidden !== undefined)
      state.categoryDraft.categories[Number(control.dataset.categoryHidden)].hidden =
        control.checked;
    if (control.dataset.categoryRole !== undefined)
      state.categoryDraft.defaults[control.dataset.categoryRole] = control.value;
    if (control.id === 'cheat-profile') {
      state.cheatProfile = Number(control.value);
      cheatDialog();
    }
    const cheatControl =
      control.dataset.profileKey ||
      control.dataset.dbField !== undefined ||
      control.dataset.dbPreset !== undefined;
    if (cheatControl) changeCheatDatabase(control);

    // Library filters.
    const filter = filterSelects[control.id];
    if (filter) {
      state.filter[filter] = control.value;
      state.anchor = null;
      rememberLibraryView();
      render();
    }

    // Preferences.
    if (control.id === 'quick-scan') {
      state.prefs.quick = control.checked;
      await preferences();
    }
    if (control.id === 'pref-developer') {
      state.prefs.developer_mode = control.checked;
      await developerPreferences();
    }
    if (control.id === 'developer-directory') {
      state.prefs.developer_updates_directory = control.value.trim();
      await developerPreferences();
    }
    if (control.id === 'pref-text-scale') {
      state.prefs.text_scale = textScale(control.value);
      applyTheme();
      await preferences();
    }
    if (control.id === 'pref-theme') {
      state.prefs.theme = control.value;
      applyTheme();
      await preferences();
    }
    if (control.id === 'pref-software') {
      state.prefs.software_rendering = control.checked;
      await preferences();
    }
    if (control.id === 'pref-offline') {
      state.prefs.offline = control.checked;
      await preferences();
    }
    if (control.id === 'version-root') {
      state.versionTarget = control.value;
      await refreshInstalledVersions();
      render();
    }
    if (control.id === 'pref-update-notices') {
      state.prefs.hide_update_notifications = !control.checked;
      await preferences();
      renderUpdateNotice();
    }
    if (control.id === 'pref-updates') {
      state.prefs.disable_update_checks = !control.checked;
      await preferences();
    }

    // Pages' own fields.
    if (control.id === 'media-game') {
      state.mediaDisc = control.value;
      render();
    }
    if (control.dataset.importAll !== undefined) {
      state.plan = await call('select_import', { selected: control.checked });
      render();
    }
    if (control.dataset.importCheck !== undefined) {
      state.plan = await call('edit_import', {
        index: Number(control.dataset.importCheck),
        selected: control.checked,
      });
      render();
    }
    if (control.dataset.importCategory !== undefined && control.value) {
      state.plan = await call('edit_import', {
        index: Number(control.dataset.importCategory),
        category: control.value,
      });
      render();
    }
    if (control.dataset.cheatField !== undefined) {
      await commitHex();
      state.backup = await call('backup_edit', {
        index: state.entryIndex,
        field: Number(control.dataset.cheatField),
        value: Number(control.value),
      });
      state.entry = await call('backup_entry', { index: state.entryIndex });
      render();
    }
    // Verifying or not changes the review, so it is redone.
    if (control.id === 'copy-verify' && state.copy) {
      state.copy = null;
      renderCopyDialog();
    }
  });
});

// The focused game is the grid's one Tab stop.
document.addEventListener('focusin', (event) => {
  const game = event.target.closest('[data-game]');
  if (game) {
    state.focusedGame = game.dataset.game;
    const previous = document.querySelector('[data-game][tabindex="0"]');
    if (previous && previous !== game) previous.tabIndex = -1;
    game.tabIndex = 0;
  }
});

// Keys: Tab stays inside an open dialog; arrow keys move between games;
// in the library, Ctrl+A selects all shown and Escape clears the selection.
document.addEventListener('keydown', (event) => {
  if (trapDialogTab(event)) return;
  const plainKey = !event.altKey && !event.ctrlKey && !event.metaKey;
  if (event.target.matches('[data-game]') && plainKey && moveGameFocus(event.target, event.key)) {
    event.preventDefault();
    return;
  }
  if (event.target.matches('input,textarea,select')) return;

  const dialogOpen = !!document.querySelector('dialog[open]');
  const selectAll = (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'a';
  if (selectAll && state.page === 'library' && !dialogOpen) {
    event.preventDefault();
    handlers['select-shown']();
  }
  if (event.key === 'Escape' && !dialogOpen && state.page === 'library') {
    state.selected.clear();
    render();
  }
});

// ---- Closing and starting ----

/**
 * Before the app closes: refuses while an operation runs, asks about
 * unsaved edits, and saves the library view. Returns true if it may close.
 */
async function prepareClose() {
  if (state.busy) {
    toast('Wait for the current operation or cancel it before closing.');
    return false;
  }
  if (hasInformationChanges()) {
    const confirmed = await confirmAction(
      'Unsaved game information',
      'Discard your information edits and close the app?',
      'Discard and close',
      true,
    );
    if (!confirmed) return;
  }
  if (!(await resolveDirty())) return;
  if (state.cheatsDirty) {
    const confirmed = await confirmAction(
      'Unsaved cheat database',
      'Discard unsaved database edits and close the app?',
      'Discard and close',
      true,
    );
    if (!confirmed) return;
  }
  await flushLibraryView();
  return true;
}

async function closeApp() {
  if (await prepareClose()) await call('quit');
}

/** Controls that stay usable while an operation runs. */
const alwaysUsableActions = ['cancel', 'logs', 'close-dialog'];

/**
 * While an operation runs (`busy`), disables every control except Cancel,
 * Activity and Close, remembering which were already disabled. Afterwards,
 * puts them back and returns focus to where it was.
 */
let busyFocus = null;
function showBusy(busy) {
  if (busy && !state.busy) busyFocus = document.activeElement;
  state.busy = busy;
  document.body.classList.toggle('busy-ui', busy);
  $('#content')?.setAttribute('aria-busy', String(busy));
  if (!busy) {
    const cancel = $('#cancel-job');
    cancel.disabled = false;
    cancel.textContent = 'Cancel';
  }

  for (const control of document.querySelectorAll('button,input,select,textarea')) {
    if (alwaysUsableActions.includes(control.dataset.action)) continue;
    if (busy) {
      control.dataset.busyLock = control.disabled ? '1' : '0';
      control.disabled = true;
    } else if (control.dataset.busyLock !== undefined) {
      control.disabled = control.dataset.busyLock === '1';
      delete control.dataset.busyLock;
    }
    if (control.closest('dialog')?.dataset.operationBusy === 'true') control.disabled = true;
  }

  if (!busy && busyFocus?.isConnected && !busyFocus.disabled) {
    // Only within the front dialog, if one is open.
    const dialogs = [...document.querySelectorAll('dialog[open]')];
    if (!dialogs.length || busyFocus.closest('dialog') === dialogs.at(-1))
      busyFocus.focus({ preventScroll: true });
    busyFocus = null;
  }
  $('#busy-dot').classList.toggle('hidden', !busy);
  $('#cancel-job').classList.toggle('hidden', !busy);
}

/** The pages `--tab N` opens (1 to 5). */
const tabPages = ['library', 'import', 'media', 'drive', 'settings'];

/**
 * Starts the interface: loads the preferences, draws the window, listens
 * for the backend's events, loads the library, applies command-line
 * options, and checks for updates (unless turned off or offline).
 *
 * Options: `--guided-setup` opens Drive setup; `--tab N` opens a page;
 * `--save-slots <folder>` and `--save-backup <file>` open the Save Manager;
 * `--update-ready`, `--update-exit` and `--update-failed <log>` are used by
 * the updater.
 */
async function init() {
  const initial = await call('init');
  state.prefs = initial.prefs;
  rememberRoots();
  state.updateReport = initial.args.includes('--update-ready') ? null : initial.update_report;
  applyTheme();
  state.version = initial.version;
  categories.splice(0, categories.length, ...initial.categories);
  shell();

  await listen('operation-progress', ({ payload }) => dialogProgress.updateProgress(payload));
  await listen('progress', ({ payload }) => {
    recordActivity(payload);
    setStatus(String(payload));
  });
  await listen('busy', ({ payload }) => showBusy(payload));
  await listen('close-requested', () => perform(closeApp));
  // Discs dropped on the window are added to the import.
  await listen('tauri://drag-drop', ({ payload }) => {
    if (state.busy || state.browseRoot) return;
    state.paths = [...new Set([...state.paths, ...payload.paths])];
    state.plan = null;
    state.page = 'import';
    render();
  });

  const args = initial.args;
  const argument = (flag) => {
    const index = args.indexOf(flag);
    return index >= 0 ? args[index + 1] : null;
  };
  if (!args.includes('--update-exit')) await perform(loadLibrary);
  await perform(refreshDriveStatus);
  // A drive may have been plugged in or removed while the app was in the background.
  window.addEventListener('focus', () => {
    if (!state.busy) perform(refreshDriveStatus);
  });

  if (args.includes('--guided-setup')) state.page = 'drive';
  if (argument('--tab')) {
    const tab = Math.max(0, Math.min(4, Number(argument('--tab')) - 1));
    state.page = tabPages[tab];
  }
  if (argument('--save-slots')) {
    state.page = 'saves';
    state.slotDirectory = argument('--save-slots');
    await perform(refreshSlots);
  }
  if (argument('--save-backup')) await perform(() => openBackup(argument('--save-backup')));
  render();
  await call('ready');

  if (argument('--update-failed') && !state.updateReport)
    toast(
      `The desktop update failed. Keep the recovery files. Log: ${argument('--update-failed')}`,
      true,
    );
  const checksAllowed = !state.prefs.offline || state.prefs.developer_mode;
  if (!args.includes('--update-exit') && checksAllowed && !state.prefs.disable_update_checks)
    perform(() => checkUpdates(true));
}

init().catch((error) => {
  document.getElementById('app').textContent = `Oh My 3DO could not initialize: ${error}`;
});

/**
 * Checks the drive at `root` before writing to it: refuses if a different
 * drive is there than the one remembered at that location.
 */
async function inspectWriteDrive(root) {
  const drive = await call('drive_inspect', { root });
  if (drive.mismatch)
    throw new Error(
      `A different drive is mounted here. Expected ${drive.expected.name}. Check and remember this drive in Drive setup first.`,
    );
  return drive;
}

// ---- Game details and information ----

/** Where LaunchBox's information is downloaded from (shown as "LaunchBox"). */
const LAUNCHBOX_SOURCE = 'https://gamesdb.launchbox-app.com/Metadata.zip';

/** A game's details: its information, description and disc files. */
function showGameDetails(key = [...state.selected][0]) {
  const group = state.groups.find((each) => each.key === key);
  if (!group) return;
  const record = state.records.find((each) => group.discs.includes(each.path));
  const metadata = record?.metadata || {};
  const source = metadata.description_source || metadata.source;

  const details = [
    ['Region', group.region],
    ['Genre', metadata.genre],
    ['Developer', metadata.developer],
    ['Publisher', metadata.publisher],
    ['Released', metadata.release_date || metadata.year],
    ['Players', metadata.players],
  ]
    .filter(([, value]) => value)
    .map(
      ([label, value]) =>
        `<dt>${escapeHtml(label)}</dt><dd>${escapeHtml(value)}${informationDetailBadge(record, label)}</dd>`,
    )
    .join('');
  const description =
    metadata.description ||
    metadata.overview ||
    'No description available. Prepare missing media or edit this game’s information.';
  const sourceNote = source
    ? `<p class="note">Description source: ${escapeHtml(source === LAUNCHBOX_SOURCE ? 'LaunchBox' : source)}</p>`
    : '';
  const body = [
    `<dl>${details}</dl>`,
    informationDetailBadge(record, 'Description'),
    `<p class="game-description">${escapeHtml(description)}</p>`,
    '<details><summary>Disc files</summary>',
    group.discs.map((path) => `<p class="path">${pathLabel(path)}</p>`).join(''),
    '</details>',
    sourceNote,
  ].join('');

  dialog(
    group.title,
    body,
    action('close-dialog', 'Close', '', 'data-dialog="main-dialog"') +
      (state.browseRoot
        ? ''
        : action(
            'game-info-edit',
            'Edit information…',
            'primary',
            `data-game-key="${escapeHtml(group.key)}"`,
          )),
  );
}

/** The game information fields that can be edited, and their labels. */
const informationFields = [
  ['description', 'Description'],
  ['genre', 'Genre'],
  ['developer', 'Developer'],
  ['publisher', 'Publisher'],
  ['release_date', 'Release date'],
  ['year', 'Release year'],
  ['players', 'Players'],
];

/** A field's label, from its key. */
function informationFieldLabel(key) {
  return informationFields.find(([field]) => field === key)?.[1] || key;
}

/**
 * The changes in the open information editor, by field: fields that
 * differ from what was loaded (or, in the bulk editor, are checked to
 * apply). Fields being restored to their downloaded values are not included.
 */
function informationPatch() {
  if (!state.informationEditor) return {};
  const edit = state.informationEditor;
  const changed = ([key, value]) => {
    if (edit.restoreFields?.has(key) || value === undefined) return false;
    if (edit.bulk) return $(`[data-info-apply="${key}"]`)?.checked;
    return value !== edit.fields[key];
  };
  return Object.fromEntries(
    informationFields.map(([key]) => [key, $(`#game-info-${key}`)?.value]).filter(changed),
  );
}

/** Whether the information editor has unsaved changes (edits or restores). */
function hasInformationChanges() {
  return (
    Object.keys(informationPatch()).length > 0 || !!state.informationEditor?.restoreFields?.size
  );
}

/**
 * A field's label in the information editor, with its "edited" marker,
 * its "downloaded value staged" note, and its Restore downloaded button.
 */
function informationLabel(key, label) {
  const restore = action(
    'game-info-restore',
    'Restore downloaded',
    'quiet small',
    `data-field="${key}" aria-label="Restore downloaded ${escapeHtml(label.toLowerCase())}"`,
  );
  return [
    '<span class="row spread">',
    `<label for="game-info-${key}">${label} <small class="badge hidden" data-info-marker="${key}"></small>`,
    `<small data-info-restored="${key}" class="hidden"> · Downloaded value staged</small></label>`,
    restore,
    '</span>',
  ].join('');
}

/** The extra attributes of a number field (year and players). */
function informationNumberAttributes(key) {
  if (key !== 'year' && key !== 'players') return '';
  return `type="number" min="1" max="${key === 'year' ? 9999 : 64}"`;
}

/**
 * Asks before closing the information editor with Escape, like its Cancel
 * button (and not while it is saving).
 */
function confirmInformationEscape(editor) {
  editor.addEventListener('cancel', (event) => {
    event.preventDefault();
    if (!state.busy)
      perform(() => handlers['close-dialog']({ dataset: { dialog: 'game-info-dialog' } }));
  });
}

/** Opens the information editor for one game (all of its discs). */
async function openInformationEditor(key) {
  const group = state.groups.find((each) => each.key === key);
  if (!group) throw new Error('Game selection changed.');
  const view = await call('game_info_load', { discs: group.discs });
  state.informationEditor = { ...view, key, restoreFields: new Set() };

  const otherFields = informationFields
    .slice(1)
    .map(
      ([field, label]) =>
        `<div class="field">${informationLabel(field, label)}<input id="game-info-${field}" aria-label="${label}" ${informationNumberAttributes(field)} value="${escapeHtml(view.fields[field])}"></div>`,
    )
    .join('');
  const body = [
    `<p>Changed fields apply to all ${quantity(group.discs.length, 'disc')} in this game. `,
    'Your edits are kept when downloading information later.</p>',
    `<div class="row section">${action('game-info-restore', 'Restore downloaded information…', 'small', 'data-field="all"')}</div>`,
    `<div class="field">${informationLabel('description', 'Description')}`,
    `<textarea id="game-info-description" rows="7" aria-label="Game description">${escapeHtml(view.fields.description)}</textarea></div>`,
    `<div class="form-grid">${otherFields}</div>`,
    '<p class="note">Restore previews downloaded values before saving and lets future downloads update those fields again. ',
    'Fields not supplied by LaunchBox are kept. Copy the games to update your console drive. Undo is available in Settings & tools.</p>',
  ].join('');

  const editor = dialog(
    'Edit ' + group.title,
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="game-info-dialog"') +
      action('game-info-save', 'Save information', 'primary'),
    'game-info-dialog',
  );
  confirmInformationEscape(editor);
  markInformationRestores();
}

/**
 * Updates the information editor's markers: which fields are staged for
 * restore, and which are (or will be) manual edits, on all discs or some.
 */
function markInformationRestores() {
  const edit = state.informationEditor;
  if (!edit) return;
  const patch = informationPatch();
  document
    .querySelectorAll('[data-info-restored]')
    .forEach((note) =>
      note.classList.toggle('hidden', !edit.restoreFields?.has(note.dataset.infoRestored)),
    );
  document.querySelectorAll('[data-info-marker]').forEach((marker) => {
    const key = marker.dataset.infoMarker;
    const editedDiscs = edit.records.filter((record) =>
      Object.hasOwn(record.overrides || {}, key),
    ).length;
    let text = '';
    if (edit.restoreFields?.has(key)) text = '';
    else if (Object.hasOwn(patch, key)) text = 'Edited on save';
    else if (editedDiscs === edit.records.length) text = 'Edited';
    else if (editedDiscs) text = 'Some discs edited';
    marker.textContent = text;
    marker.classList.toggle('hidden', !text);
    marker.title = 'Manual edits are protected from downloaded updates.';
  });
}

/** A metadata value as text ('' for none). */
function informationValue(value) {
  return value === null || value === undefined ? '' : String(value);
}

/** An "Edited" badge for a detail whose value the person set by hand. */
function informationDetailBadge(record, label) {
  const keysByLabel = {
    Description: ['description'],
    Genre: ['genre'],
    Developer: ['developer'],
    Publisher: ['publisher'],
    Released: ['release_date', 'year'],
    Players: ['players'],
  };
  const keys = keysByLabel[label] || [];
  const edited = keys.some((key) => Object.hasOwn(record?.metadata_overrides || {}, key));
  return edited
    ? ' <span class="badge" title="Protected from downloaded updates">Edited</span>'
    : '';
}

/**
 * Opens the information editor for all selected games at once: each field
 * is applied only if checked. A field all discs share is filled in.
 */
async function openBulkInformationEditor() {
  const discs = selectedDiscs(state.groups, state.selected);
  if (!discs.length) throw new Error('Select games first.');
  const view = await call('game_info_load', { discs });
  const gameCount = state.selected.size;
  state.informationEditor = { ...view, bulk: true, gameCount, restoreFields: new Set() };

  const fields = informationFields
    .slice(1)
    .map(([key, label]) => {
      const values = view.records.map((record) => informationValue(record.metadata?.[key]));
      const same = new Set(values).size === 1;
      return [
        '<div class="field">',
        `<label class="row"><input type="checkbox" data-info-apply="${key}">Apply ${escapeHtml(label.toLowerCase())} `,
        `<small class="badge hidden" data-info-marker="${key}"></small></label>`,
        `<input id="game-info-${key}" aria-label="${label}" disabled ${informationNumberAttributes(key)}`,
        ` placeholder="${same ? '' : 'Mixed values — choose a shared value'}" value="${same ? escapeHtml(values[0]) : ''}">`,
        '</div>',
      ].join('');
    })
    .join('');
  const body = [
    `<p>Choose which shared fields to apply to ${gameCount} games (${quantity(discs.length, 'disc')}). `,
    'Unchecked fields and each game’s description are kept. Checking Apply with a blank value clears that field.</p>',
    `<div class="form-grid">${fields}</div>`,
    '<p class="note">Applied values become manual edits protected from future downloads. ',
    'You can undo the entire change in Settings & tools.</p>',
  ].join('');

  const editor = dialog(
    'Edit selected games',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="game-info-dialog"') +
      action('game-info-save', 'Save information', 'primary'),
    'game-info-dialog',
  );
  confirmInformationEscape(editor);
  markInformationRestores();
}

/** The Metadata tools dialog: matching, exporting and importing, and backups. */
function informationTools() {
  const selectedCount = state.selected.size;
  const body = [
    '<div class="metadata-tools stack">',
    '<section><h3>Match game information</h3>',
    '<p>Review unmatched games or change an existing LaunchBox match.</p>',
    action('information-matches', 'Review information matches…'),
    '</section>',
    '<section><h3>Back up custom edits</h3>',
    '<p>Export your edits, including cleared fields, or review an import before applying it.</p>',
    '<div class="row">',
    action('information-export', 'Export all…', '', 'data-scope="all"'),
    action(
      'information-export',
      `Export selected… (${selectedCount})`,
      '',
      `data-scope="selected" ${selectedCount ? '' : 'disabled'}`,
    ),
    action('information-import-preview', 'Import metadata…'),
    '</div></section>',
    '<section><h3>Automatic backups</h3>',
    '<p>Review the five most recent snapshots saved on this computer.</p>',
    action('information-backups', 'Automatic metadata backups…'),
    '</section>',
    '</div>',
    '<p class="note">Manual edits are protected from downloads. Metadata imports and match changes support Undo.</p>',
  ].join('');
  dialog('Metadata tools', body, action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'));
}

/** Exports the person's own information: for the selected games, or all. */
async function exportInformation(selected) {
  const discs = selected ? selectedDiscs(state.groups, state.selected) : [];
  if (selected && !discs.length) throw new Error('Select games first.');
  const target = await pick('metadata_save');
  if (!target) return;
  const result = await call('game_info_export', { target, discs });
  toast(`Exported custom metadata for ${quantity(result.count, 'disc')}.`);
}

/** Chooses a metadata file and reviews importing it. */
async function previewInformationImport() {
  const file = await pick('metadata');
  if (!file) return;
  const preview = await call('game_info_import_preview', { file });
  showInformationImport(preview, file);
}

/** A disc's current value of a field (a description may be in "overview"). */
function currentInformation(metadata, key) {
  return key === 'description' ? metadata?.description || metadata?.overview : metadata?.[key];
}

/**
 * Reviews an information import (from a file or an automatic backup): one
 * section per disc, each field's current and imported value. Discs whose
 * own edits would be replaced start unchecked.
 */
function showInformationImport(preview, file) {
  state.informationImport = preview;

  const discSection = (record, index) => {
    const conflicts = record.conflicts.length;
    const fieldRows = Object.entries(record.fields)
      .map(([key, value]) => {
        const edited = record.conflicts.includes(key)
          ? ' <span class="badge warning">Edited</span>'
          : '';
        const current =
          informationValue(currentInformation(record.original.metadata, key)) || '(Empty)';
        const imported = informationValue(value) || '(Empty — clears this field)';
        return [
          `<tr><td>${escapeHtml(informationFieldLabel(key))}${edited}</td>`,
          `<td><div class="game-description">${escapeHtml(current)}</div></td>`,
          `<td><div class="game-description">${escapeHtml(imported)}</div></td></tr>`,
        ].join('');
      })
      .join('');
    const from =
      record.path !== record.source_path ? ` · From ${pathLabel(record.source_path)}` : '';
    const differing = conflicts ? ' · ' + conflicts + ' manual edits differ' : '';
    return [
      '<section class="card"><label class="row">',
      `<input type="checkbox" data-info-import="${index}" ${conflicts ? '' : 'checked'}>`,
      `<strong>${escapeHtml(record.title)}</strong>`,
      conflicts ? '<span class="badge warning">Replaces manual edits</span>' : '',
      '</label>',
      `<p class="note path">${pathLabel(record.path)}<br>${escapeHtml(record.matching)}${from}</p>`,
      `<details ${index === 0 ? 'open' : ''}><summary>Review ${quantity(Object.keys(record.fields).length, 'field')}${differing}</summary>`,
      '<div class="table-wrap" tabindex="0" role="region" aria-label="Scrollable table">',
      '<table class="information-restore-table"><thead><tr><th>Field</th><th>Current</th><th>Imported</th></tr></thead>',
      `<tbody>${fieldRows}</tbody></table></div></details>`,
      '</section>',
    ].join('');
  };
  const skipped = preview.skipped.length
    ? [
        `<details class="section"><summary>${preview.skipped.length} skipped entries</summary>`,
        `<ul>${preview.skipped.map((text) => `<li>${escapeHtml(text)}</li>`).join('')}</ul></details>`,
      ].join('')
    : '';
  const body = [
    `<p>${pathLabel(file)}</p>`,
    `<p>Changes found for ${quantity(preview.records.length, 'disc')}. Select the entries to import, then review their fields below.</p>`,
    '<p class="note">Entries that replace different manual edits start unchecked. ',
    'Check them only if you want the imported values. Other fields are kept.</p>',
    `<div class="stack">${preview.records.map(discSection).join('')}</div>`,
    skipped,
    '<p class="note">Importing saves the checked fields as manual edits. Copy the affected games afterward to update your console drive. ',
    'Undo is available in Settings & tools.</p>',
  ].join('');

  dialog(
    'Review custom metadata import',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="main-dialog"') +
      action(
        'information-import-save',
        'Import checked entries',
        'primary',
        'id="information-import-save"',
      ),
  );
  updateInformationImportButton();
}

/** Labels the import button with how many discs are checked. */
function updateInformationImportButton() {
  const button = $('#information-import-save');
  const count = document.querySelectorAll('[data-info-import]:checked').length;
  if (button) {
    button.disabled = !count;
    button.textContent = count
      ? `Import ${count} ${count === 1 ? 'disc' : 'discs'}`
      : 'Select entries to import';
  }
}

/** Statuses of a disc that already has information matched. */
const matchedStatuses = ['Automatic match', 'Chosen match'];

/** Opens the review of which LaunchBox entry each game's information comes from. */
async function openInformationMatches() {
  state.informationMatches = await call('game_info_matches');
  const review = state.informationMatches;
  const unmatched = review.records.filter(
    (record) => !matchedStatuses.includes(review.status[record.path]),
  ).length;
  const body = [
    `<p>${quantity(unmatched, 'disc')} ${unmatched === 1 ? 'needs' : 'need'} a match. `,
    'Choose the correct LaunchBox title to preview and apply its information. Manual edits are kept. ',
    'Chosen matches are remembered for future information downloads and missing-media preparation.</p>',
    '<div class="row">',
    '<input id="information-match-query" type="search" placeholder="Find a library game…" aria-label="Find a game to match">',
    '<label><input type="checkbox" id="information-match-all"> Show matched games too</label>',
    '</div>',
    '<div id="information-match-list" class="stack section"></div>',
    '<p class="note">This changes information only. Disc identification, folder names, game files and media associations are kept. ',
    'Cached LaunchBox information works offline. Undo is available in Settings & tools.</p>',
  ].join('');
  dialog(
    'Review information matches',
    body,
    action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
  );
  renderInformationMatchList();
}

/** Lists the games to match: unmatched ones (or all), filtered by the search. */
function renderInformationMatchList() {
  const target = $('#information-match-list');
  const review = state.informationMatches;
  if (!target || !review) return;
  const query = ($('#information-match-query')?.value || '').trim().toLocaleLowerCase();
  const showAll = $('#information-match-all')?.checked;
  const games = groupGames(review.records).filter(
    (game) =>
      (!query || game.title.toLocaleLowerCase().includes(query)) &&
      (showAll || game.discs.some((path) => !matchedStatuses.includes(review.status[path]))),
  );

  target.innerHTML = games.length
    ? games
        .map((game) => {
          const statuses = [...new Set(game.discs.map((path) => review.status[path]))].join('; ');
          return [
            '<div class="card row spread"><div>',
            `<strong>${escapeHtml(game.title)}</strong>`,
            `<p class="note">${escapeHtml(game.region)} · ${escapeHtml(statuses)} · ${quantity(game.discs.length, 'disc')}</p>`,
            '</div>',
            action(
              'information-match-game',
              'Choose match…',
              'small',
              `data-key="${escapeHtml(game.key)}"`,
            ),
            '</div>',
          ].join('');
        })
        .join('')
    : '<p>No games match this view. Clear the search or enable Show matched games too.</p>';
}

/** Opens the chooser of LaunchBox entries for one game. */
function openInformationMatch(key) {
  const review = state.informationMatches;
  const game = groupGames(review.records).find((each) => each.key === key);
  if (!game) throw new Error('Review information matches again.');
  state.informationMatchGame = game;
  state.informationMatchChoice = '';
  const body = [
    `<p>Applies to ${quantity(game.discs.length, 'disc')}. `,
    'Check the description, release year and publisher before choosing a match.</p>',
    '<label class="field">Search LaunchBox titles',
    '<input id="information-choice-query" type="search" placeholder="Search the downloaded 3DO titles…"></label>',
    '<label class="field">LaunchBox title<select id="information-choice" size="8" aria-label="LaunchBox title"></select></label>',
    '<p class="note" id="information-choice-count"></p>',
    '<div id="information-match-preview"></div>',
  ].join('');
  dialog(
    'Choose information for ' + game.title,
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="information-match-dialog"') +
      action(
        'information-match-save',
        'Use this match',
        'primary',
        'id="information-match-save" disabled',
      ),
    'information-match-dialog',
  );
  renderInformationChoices();
}

/** Lists the LaunchBox entries matching the search. */
function renderInformationChoices() {
  const target = $('#information-choice');
  if (!target) return;
  const query = ($('#information-choice-query')?.value || '').trim().toLocaleLowerCase();
  const choices = state.informationMatches.choices.filter(
    (choice) => !query || String(choice.title).toLocaleLowerCase().includes(query),
  );
  const label = (choice) =>
    [choice.title, choice.year, choice.publisher, 'ID ' + choice.launchbox_id]
      .filter(Boolean)
      .join(' · ');
  target.innerHTML =
    option('', 'Choose a LaunchBox title', true) +
    choices.map((choice) => option(choice.launchbox_id, label(choice), false)).join('');
  state.informationMatchChoice = '';
  $('#information-choice-count').textContent = `${choices.length} matching LaunchBox titles.`;
  renderInformationMatchPreview();
}

/**
 * Previews the chosen LaunchBox entry: for each disc, each field's current
 * value and what it would become. Manual edits are kept.
 */
function renderInformationMatchPreview() {
  const review = state.informationMatches;
  const id = $('#information-choice')?.value || '';
  const chosen = review?.choices.find((choice) => choice.launchbox_id === id);
  state.informationMatchChoice = id;
  const target = $('#information-match-preview');
  const button = $('#information-match-save');
  if (button) button.disabled = !chosen;
  if (!target) return;
  if (!chosen) {
    target.innerHTML =
      '<p class="note">Choose a title to preview the changes. No files have been changed.</p>';
    return;
  }

  const game = state.informationMatchGame;
  const records = review.records.filter((record) => game.discs.includes(record.path));
  const fieldRow = (record, key, label) => {
    const current = currentInformation(record.metadata, key);
    const value = chosen[key];
    const manual = Object.hasOwn(record.metadata_overrides || {}, key);
    const supplied = value !== null && value !== undefined && String(value).trim() !== '';
    let downloaded;
    if (manual) downloaded = '<span class="badge">Manual edit kept</span>';
    else if (supplied) downloaded = escapeHtml(informationValue(value));
    else downloaded = 'Not supplied — current value kept';
    return [
      `<tr><td>${escapeHtml(label)}</td>`,
      `<td><div class="game-description">${escapeHtml(informationValue(current) || '(Empty)')}</div></td>`,
      `<td><div class="game-description">${downloaded}</div></td></tr>`,
    ].join('');
  };
  const discTable = (record, index) =>
    [
      `<details ${index === 0 ? 'open' : ''}><summary>${pathLabel(record.path)}</summary>`,
      '<div class="table-wrap" tabindex="0" role="region" aria-label="Scrollable table">',
      '<table class="information-restore-table"><thead><tr><th>Field</th><th>Current</th><th>Downloaded</th></tr></thead>',
      `<tbody>${informationFields.map(([key, label]) => fieldRow(record, key, label)).join('')}</tbody>`,
      '</table></div></details>',
    ].join('');
  const facts = [chosen.year, chosen.publisher, chosen.developer].filter(Boolean).join(' · ');

  target.innerHTML = [
    `<h3>${escapeHtml(chosen.title)}</h3>`,
    `<p class="note">${escapeHtml(facts)} · LaunchBox ID ${escapeHtml(id)}</p>`,
    records.map(discTable).join(''),
    '<p class="note">Using this match saves downloaded fields immediately and remembers the LaunchBox entry. ',
    'Manual edits remain protected; use Restore downloaded in the information editor to remove an override.</p>',
  ].join('');
}

/** Uses the chosen LaunchBox entry for the game. */
async function saveInformationMatch() {
  const review = state.informationMatches;
  const game = state.informationMatchGame;
  const id = state.informationMatchChoice;
  if (!review || !game || !id) throw new Error('Choose and preview a LaunchBox title first.');
  await call('game_info_choose_match', { token: review.token, discs: game.discs, id });
  state.informationMatches = null;
  closeDialog('information-match-dialog');
  closeDialog();
  await loadLibrary();
  toast(
    'Information match saved. Future downloads will use this entry. Copy the games to update your console drive.',
  );
}

/** Lists the automatic backups of the person's own information. */
async function showInformationBackups() {
  const result = await call('game_info_backups');
  const backups = result.backups.length
    ? result.backups
        .map((backup) =>
          [
            '<section class="card row spread"><div>',
            `<strong>${escapeHtml(new Date(backup.modified * 1000).toLocaleString())}</strong>`,
            `<p class="note">${quantity(backup.entries, 'disc')} · ${bytes(backup.bytes)}</p>`,
            '</div>',
            action(
              'information-backup-preview',
              'Review restore…',
              'small',
              `data-name="${escapeHtml(backup.name)}"`,
            ),
            '</section>',
          ].join(''),
        )
        .join('')
    : '<p>No snapshots yet. After you edit game information, a snapshot is saved before your next library change.</p>';
  const body = [
    '<p>The five newest distinct snapshots of your custom edits are kept for this library. ',
    'Snapshots are saved before library changes, including metadata edits and updates, imports, media preparation, category changes and Undo.</p>',
    '<p class="note">Snapshots stay on this computer and are independent of Undo. ',
    'Restoring opens the normal import review, where you choose entries and resolve conflicts. Only manual information fields are backed up.</p>',
    `<p class="path">${pathLabel(result.directory)}</p>`,
    `<div class="stack">${backups}</div>`,
  ].join('');
  dialog(
    'Automatic metadata backups',
    body,
    action('close-dialog', 'Close', '', 'data-dialog="main-dialog"'),
  );
}

/** Imports the checked discs of the reviewed information import. */
async function saveInformationImport() {
  const plan = state.informationImport;
  if (!plan) throw new Error('Review a metadata import first.');
  const discs = [...document.querySelectorAll('[data-info-import]:checked')].map(
    (input) => plan.records[Number(input.dataset.infoImport)].path,
  );
  if (!discs.length) throw new Error('Check at least one entry to import.');
  await call('game_info_import', { token: plan.token, discs });
  state.informationImport = null;
  closeDialog();
  await loadLibrary();
  toast(
    `Imported custom metadata for ${quantity(discs.length, 'disc')}. Copy the games to update your console drive.`,
  );
}

/**
 * Previews restoring downloaded values in the information editor, for one
 * field or `all`: each disc's current (or draft) value and the downloaded
 * one. Discs that agree are shown once.
 */
async function previewInformationRestore(field) {
  const edit = state.informationEditor;
  if (!edit) throw new Error('Open the information editor first.');
  if (!edit.restorePreview)
    edit.restorePreview = await call('game_info_restore_preview', { original: edit.records });
  const preview = edit.restorePreview;
  const keys = field === 'all' ? informationFields.map(([key]) => key) : [field];
  edit.restoreRequested = keys.filter((key) => preview.available.includes(key));
  const patch = informationPatch();

  const rows = keys
    .map((key) => {
      const label = informationFieldLabel(key);
      const values = preview.records.map((record, index) => {
        const loaded = edit.records[index]?.metadata || {};
        let current;
        if (edit.restoreFields.has(key)) current = informationValue(record.fields[key]);
        else if (Object.hasOwn(patch, key)) current = patch[key];
        else current = informationValue(currentInformation(loaded, key));
        return { path: record.path, current, downloaded: informationValue(record.fields[key]) };
      });
      const same =
        new Set(values.map((value) => JSON.stringify([value.current, value.downloaded]))).size ===
        1;
      return (same ? values.slice(0, 1) : values)
        .map((value) => {
          const path = !same ? `<small class="path">${pathLabel(value.path)}</small>` : '';
          const downloaded = preview.available.includes(key)
            ? escapeHtml(value.downloaded)
            : 'Not supplied for every disc — kept';
          return [
            `<tr><td>${escapeHtml(label)}${path}</td>`,
            `<td><div class="game-description">${escapeHtml(value.current || '(Empty)')}</div></td>`,
            `<td><div class="game-description">${downloaded}</div></td></tr>`,
          ].join('');
        })
        .join('');
    })
    .join('');
  const body = [
    '<p>Review the LaunchBox values below. Choose Use downloaded values, then Save information in the editor to finish. ',
    'Restored fields can receive future downloaded updates.</p>',
    '<div class="table-wrap" tabindex="0" role="region" aria-label="Scrollable table">',
    '<table class="information-restore-table"><thead><tr><th>Field</th><th>Current / draft</th><th>Downloaded</th></tr></thead>',
    `<tbody>${rows}</tbody></table></div>`,
    `<p class="note">Applies to ${quantity(edit.records.length, 'disc')}. Unavailable fields and your other edits are kept.</p>`,
  ].join('');

  dialog(
    'Restore downloaded information',
    body,
    action('close-dialog', 'Cancel', '', 'data-dialog="game-info-restore-dialog"') +
      action(
        'game-info-stage-restore',
        'Use downloaded values',
        'primary',
        !edit.restoreRequested.length ? 'disabled' : '',
      ),
    'game-info-restore-dialog',
  );
}

/**
 * Puts the previewed downloaded values into the editor's fields; they are
 * saved (and the manual edits removed) with Save information.
 */
function stageInformationRestore() {
  const edit = state.informationEditor;
  if (!edit?.restorePreview) return;
  for (const key of edit.restoreRequested) {
    const input = $(`#game-info-${key}`);
    if (input) {
      input.value = informationValue(edit.restorePreview.records[0].fields[key]);
      edit.restoreFields.add(key);
    }
  }
  markInformationRestores();
  closeDialog('game-info-restore-dialog');
}
