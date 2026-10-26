// The interface's pure logic: grouping disc records into games, filtering
// them, and formatting text. Nothing here touches the page, so it is tested
// directly (tests/ui-model.test.mjs).

/**
 * The library's categories, in display order. The app replaces the
 * contents with the library's own (configured) categories when it loads one.
 */
export const categories = [
  'Games',
  'Educational',
  'Adult',
  'Demos',
  'Samplers',
  'Multimedia',
  'Applications',
  'Bonus Discs',
];

/** Whether a metadata value is filled in. */
const isPresent = (value) => value !== null && value !== undefined && String(value).trim() !== '';

/**
 * Groups disc records into games, sorted by title.
 *
 * Discs in an organized library (Category/Region/Game/disc) are grouped by
 * their game folder, and can be moved to another category as a whole.
 * Others are grouped by the record's `group_key` (discs of one set), or
 * stand alone. A game's title drops any "(Disc N)".
 *
 * Each game collects what the library view needs: its discs and total
 * size, whether any disc lacks a cover, preview or each information field,
 * whether any was edited by hand, the text to search, and how many of its
 * discs are on the chosen drive (`presenceKnown` is false if any disc's
 * presence is unknown).
 */
export function groupGames(records) {
  const groups = new Map();
  for (const record of records) {
    const path = String(record.path ?? '');
    if (!path) continue;
    const parts = path.split('/');
    const movable = parts.length >= 4 && categories.includes(parts[0]);
    const key = movable ? parts.slice(0, 3).join('/') : String(record.group_key || path);

    if (!groups.has(key))
      groups.set(key, {
        key,
        title: String(record.title || parts.at(-1)).replace(/ \((?:disc|cd) \d+\)/gi, ''),
        category: movable ? parts[0] : record.category || 'Games',
        region: record.region || '',
        genre: record.metadata?.genre || '',
        publisher: record.metadata?.publisher || '',
        cover: record.cover,
        discs: [],
        missingCover: false,
        missingPreview: false,
        missingInformation: [],
        edited: false,
        informationSearch: [],
        bytes: 0,
        presentDiscs: 0,
        presenceKnown: true,
        movable,
      });
    const game = groups.get(key);

    game.bytes += Number(record.bytes || 0);
    game.presentDiscs += record.on_drive === true ? 1 : 0;
    game.presenceKnown &&= typeof record.on_drive === 'boolean';
    game.discs.push(path);
    game.cover ||= record.cover;
    game.missingCover ||= record.cover_missing ?? !record.cover;
    game.missingPreview ||= record.preview_missing ?? !record.preview;

    const metadata = record.metadata || {};
    const description = metadata.description || metadata.overview;
    game.informationSearch.push(
      [record.title, description, metadata.genre, metadata.developer, metadata.publisher]
        .filter(Boolean)
        .join(' ')
        .toLocaleLowerCase(),
    );
    const information = {
      description,
      genre: metadata.genre,
      developer: metadata.developer,
      publisher: metadata.publisher,
      released: metadata.release_date || metadata.year,
      players: metadata.players,
    };
    for (const [field, value] of Object.entries(information))
      if (!isPresent(value) && !game.missingInformation.includes(field))
        game.missingInformation.push(field);
    game.edited ||= Object.keys(record.metadata_overrides || {}).length > 0;
  }

  return [...groups.values()].sort(
    (a, b) => a.title.localeCompare(b.title) || a.key.localeCompare(b.key),
  );
}

/** Whether a game matches the search, in its title (or, by choice, its information). */
function matchesSearch(game, query, searchScope) {
  if (!query) return true;
  if (game.title.toLocaleLowerCase().includes(query)) return true;
  return (
    searchScope === 'information' && game.informationSearch?.some((text) => text.includes(query))
  );
}

/** Whether a game matches the missing media filter ('cover', 'preview' or 'either'). */
function matchesMedia(game, media) {
  if (!media) return true;
  if (media === 'cover') return game.missingCover;
  if (media === 'preview') return game.missingPreview;
  if (media === 'either') return game.missingCover || game.missingPreview;
  return false;
}

/**
 * Whether a game matches the information filter: 'edited' (edited by hand),
 * 'any' (missing any field), or a field it is missing.
 */
function matchesInformation(game, information) {
  if (!information) return true;
  if (information === 'edited') return game.edited;
  if (information === 'any') return game.missingInformation?.length;
  return game.missingInformation?.includes(information);
}

/** The games that pass every filter (empty filters let everything through). */
export function visibleGames(
  groups,
  {
    search = '',
    category = '',
    region = '',
    media = '',
    information = '',
    searchScope = 'title',
  } = {},
) {
  const query = search.toLocaleLowerCase();
  return groups.filter(
    (game) =>
      (!category || game.category === category) &&
      (!region || game.region === region) &&
      matchesSearch(game, query, searchScope) &&
      matchesMedia(game, media) &&
      matchesInformation(game, information),
  );
}

/** Every disc of the selected games (`selected`: a set of game keys). */
export function selectedDiscs(groups, selected) {
  return groups.filter((game) => selected.has(game.key)).flatMap((game) => game.discs);
}

/** Text made safe to put in HTML (also inside attribute values). */
export function escapeHtml(value) {
  const entities = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
  return String(value ?? '').replace(/[&<>"']/g, (character) => entities[character]);
}

/** A size in bytes as "512 B", "1.5 KiB", "3.2 MiB" or "1.25 GiB". */
export function formatBytes(count) {
  if (count < 1024) return `${count} B`;
  if (count < 1024 ** 2) return `${(count / 1024).toFixed(1)} KiB`;
  if (count < 1024 ** 3) return `${(count / 1024 ** 2).toFixed(1)} MiB`;
  return `${(count / 1024 ** 3).toFixed(2)} GiB`;
}

/**
 * Whether a save slot matches a search: every word must appear in its
 * name, games, search text or file names (ignoring case).
 */
export function slotMatches(slot, query) {
  const text = [slot.name, slot.games, slot.search, ...(slot.files || [])].join(' ').toLowerCase();
  return query
    .trim()
    .toLowerCase()
    .split(/\s+/)
    .every((word) => text.includes(word));
}

// Hide Windows extended-length syntax in labels; retain original paths for I/O.
// "\\?\C:\Games" is shown as "C:\Games", "\\?\UNC\server\share" as "\\server\share".
export function displayPath(value) {
  const path = String(value ?? '');
  if (/^\\\\\?\\[a-z]:\\/i.test(path)) return path.slice(4);
  if (/^\\\\\?\\UNC\\/i.test(path)) return '\\\\' + path.slice(8);
  return path;
}

/** The information filter's choices (besides "all"). */
const informationFilters = [
  'any',
  'description',
  'released',
  'genre',
  'developer',
  'publisher',
  'players',
  'edited',
];

/**
 * A library's saved view (selection, search and filters) made safe to use:
 * selected games that no longer exist, and filters for categories or
 * regions the library no longer has, are dropped. Views belong to one
 * canonical library root.
 */
export function restoreLibraryView(view, groups, categoryNames) {
  const keys = new Set(groups.map((game) => game.key));
  const regions = new Set(groups.map((game) => game.region));
  const selected = Array.isArray(view?.selected) ? view.selected : [];
  return {
    selected: [...new Set(selected)].filter((key) => keys.has(key)),
    search: typeof view?.search === 'string' ? view.search : '',
    searchScope: view?.searchScope === 'information' ? 'information' : 'title',
    category: categoryNames.includes(view?.category) ? view.category : '',
    region: regions.has(view?.region) ? view.region : '',
    media: ['cover', 'preview', 'either'].includes(view?.media) ? view.media : '',
    information: informationFilters.includes(view?.information) ? view.information : '',
  };
}
