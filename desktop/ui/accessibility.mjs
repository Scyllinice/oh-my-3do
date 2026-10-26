// Keyboard and screen reader support: keeping focus in place when a view is
// redrawn, keeping Tab inside dialogs, moving between games with the arrow
// keys, and announcing messages.

/** Controls that can take keyboard focus. */
const FOCUSABLE = 'button,input,select,textarea,a[href],summary,[tabindex]';

// Preserve keyboard position when a view replaces its controls. Keys describe
// the underlying game/field/action, not its index in a filtered list.
/**
 * Remembers which control inside `root` has focus, and how to find it again
 * after a redraw: its id, or else its tag and `data-` attributes (which name
 * the game, field or action). Also keeps a text field's cursor and scroll.
 * Returns null if nothing inside `root` has focus.
 */
export function captureFocus(root = document) {
  const node = root.ownerDocument?.activeElement || root.activeElement;
  if (!node || !root.contains(node) || node === root || node.tagName === 'BODY') return null;
  // Attributes that change while busy are not part of a control's identity.
  const attributes = [...node.attributes]
    .filter(
      (attribute) =>
        attribute.name.startsWith('data-') &&
        !['data-busy-lock', 'data-count'].includes(attribute.name),
    )
    .map((attribute) => [attribute.name, attribute.value]);
  let selection = null;
  if (typeof node.selectionStart === 'number')
    selection = [node.selectionStart, node.selectionEnd, node.selectionDirection];
  return { node, id: node.id, tag: node.tagName, attributes, selection, scrollTop: node.scrollTop };
}

/**
 * Focuses a control if it can take focus (connected, enabled, visible and
 * not inert). Returns whether it did.
 */
export function focusElement(node) {
  const usable =
    node?.isConnected && !node.disabled && node.getClientRects().length && !node.closest('[inert]');
  if (!usable) return false;
  node.focus({ preventScroll: true });
  return node.ownerDocument.activeElement === node;
}

/**
 * Focuses the control `captureFocus` remembered, or its replacement after a
 * redraw, and restores its cursor and scroll. Returns whether it could.
 */
export function restoreFocus(snapshot, root = document) {
  if (!snapshot) return false;
  let node = snapshot.node;
  if (!root.contains(node)) {
    const candidates = [...root.querySelectorAll(FOCUSABLE)];
    if (snapshot.id) {
      node = candidates.find((candidate) => candidate.id === snapshot.id);
    } else if (snapshot.attributes.length) {
      node = candidates.find(
        (candidate) =>
          candidate.tagName === snapshot.tag &&
          snapshot.attributes.every(([name, value]) => candidate.getAttribute(name) === value),
      );
    } else {
      node = null;
    }
  }
  if (!focusElement(node)) return false;
  if (snapshot.selection && typeof node.setSelectionRange === 'function')
    node.setSelectionRange(...snapshot.selection);
  node.scrollTop = snapshot.scrollTop;
  revealFocus(node);
  return true;
}

/** Scrolls a focused control (a game: its title) into view. */
function revealFocus(node) {
  (node.querySelector('.game-title') || node).scrollIntoView({
    block: 'nearest',
    inline: 'nearest',
  });
}

/**
 * Focuses a page's or dialog's heading, as when it opens, so screen readers
 * start there. A page scrolls back to its top.
 */
export function focusHeading(root) {
  const heading = root?.querySelector('h1,h2');
  if (!heading) return false;
  heading.tabIndex = -1;
  const focused = focusElement(heading);
  if (focused && root.tagName !== 'DIALOG') {
    root.scrollTop = 0;
    heading.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
  return focused;
}

/**
 * Keeps Tab and Shift+Tab inside the front dialog, wrapping from its last
 * control to its first and back. Returns true if it handled the key.
 */
export function trapDialogTab(event, document = globalThis.document) {
  // WebKitGTK reports Shift+Tab as an Unidentified key with the Tab code.
  if (event.key !== 'Tab' && event.code !== 'Tab') return false;
  const dialog = [...document.querySelectorAll('dialog[open]')].at(-1);
  if (!dialog) return false;

  const targets = [...dialog.querySelectorAll(FOCUSABLE)].filter(
    (node) => !node.disabled && node.tabIndex >= 0 && node.getClientRects().length,
  );
  const current = targets.indexOf(document.activeElement);
  const leaving =
    current < 0 ||
    (event.shiftKey && current === 0) ||
    (!event.shiftKey && current === targets.length - 1);
  if (!leaving) return false;

  event.preventDefault();
  if (!focusElement(event.shiftKey ? targets.at(-1) : targets[0])) focusHeading(dialog);
  return true;
}

// Each announcement region's pending message.
const announcements = new WeakMap();

/**
 * Says a message to screen readers, through the front dialog's (or the
 * page's) live region: politely, or at once if `urgent`. The region is
 * emptied first, so repeating a message is announced again.
 */
export function announce(message, urgent = false, document = globalThis.document) {
  const front = [...document.querySelectorAll('dialog[open]')].at(-1);
  const region = (front || document).querySelector(
    `[data-announcer="${urgent ? 'alert' : 'polite'}"]`,
  );
  if (!region) return;
  clearTimeout(announcements.get(region));
  region.textContent = '';
  const delay = urgent ? 20 : 180;
  announcements.set(
    region,
    setTimeout(() => {
      if (region.isConnected) region.textContent = String(message);
    }, delay),
  );
}

/**
 * Moves focus between games in the grid with the arrow keys (up and down
 * by a row), Home and End. Returns true if it handled the key.
 */
export function moveGameFocus(node, key) {
  const grid = node.closest('.grid');
  if (!grid) return false;
  const games = [...grid.querySelectorAll('[data-game]:not([disabled])')];
  const index = games.indexOf(node);
  if (index < 0) return false;

  const columns = getComputedStyle(grid).gridTemplateColumns.split(' ').length;
  const next = {
    ArrowLeft: index - 1,
    ArrowRight: index + 1,
    ArrowUp: index - columns,
    ArrowDown: index + columns,
    Home: 0,
    End: games.length - 1,
  }[key];
  if (next === undefined) return false;
  const target = games[Math.max(0, Math.min(games.length - 1, next))];
  if (focusElement(target)) revealFocus(target);
  return true;
}

/** A text size the app supports (100% to 200% in steps of 25), or 100. */
export function textScale(value) {
  return [100, 125, 150, 175, 200].includes(Number(value)) ? Number(value) : 100;
}
