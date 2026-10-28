#!/usr/bin/env python3
"""Exercise the real frontend in WebKitGTK with a disposable, mocked native bridge.

Run under D-Bus/Xvfb in tools/desktop-tauri.Dockerfile. No games, physical drives,
network downloads or production preferences are touched. Native bridge/file I/O
is covered separately by test-desktop-ui.py and the Rust suites.

The desktop interface (desktop/ui/) is served from a local web server with a
small script injected that stands in for the app's native side, and opened in
WebKitGTK through Selenium. The test then checks, page by page and dialog by
dialog: keyboard navigation and focus, form labels, text contrast in both
themes, and that nothing overflows the window at 100-200% text size.

Each check prints PASS or FAIL. Results and screenshots go to
build/desktop-accessibility/. --baseline runs only the basic checks and
records failures without failing, for comparing against older versions.
"""
import argparse
import functools
import http.server
import json
from pathlib import Path
import threading
import time

from selenium import webdriver
from selenium.webdriver.common.by import By
from selenium.webdriver.common.keys import Keys
from selenium.webdriver.support.ui import WebDriverWait

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "build" / "desktop-accessibility"
UI_FOLDER = ROOT / "desktop/ui"

GAME_TITLES = ["Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta"]
PAGES = ["saves", "media", "drive", "settings", "updates"]
ELEMENT_TIMEOUT_SECONDS = 8

# (window width, height, text size %) combinations checked for reflow.
TEXT_SCALE_SIZES = [(1280, 900, 100), (960, 720, 150), (640, 480, 200)]

# Below this width the selection actions collapse into one menu button.
COMPACT_WIDTH = 760

# Stands in for the desktop app's native side (window.__TAURI__): every
# request from the interface is answered from fixed data and recorded in
# window.__a11yMock.requests for the test to inspect. Setting
# window.__a11yMock.fail to an action name makes that request fail.
# __RECORDS__, __CHEATS__ and __VERSION__ are filled in by mock_script().
MOCK_SCRIPT = r"""
(() => {
  const records = __RECORDS__;
  const cheats = __CHEATS__;
  const version = __VERSION__;
  const categories = () => ['Games', 'Bonus Discs', 'Music'];

  let prefs = {
    library: '/fixture/library',
    destination: '/fixture/usb',
    offline: true,
    disable_update_checks: true,
    theme: 'light',
  };
  const listeners = {};
  const backup = {
    path: '/fixture/usb/.ohmy3do/saves/slot01.3ds',
    dirty: false,
    bytes: 100,
    entries: [{ name: 'ExampleSave', known: 'Example game', size: 4, file_type: 0 }],
  };
  const storage = {
    games: 2048,
    covers: 0,
    previews: 0,
    support: 0,
    total: 2048,
    to_copy: 2048,
    reserve: 1024,
    required: 3072,
    available: 1048576,
    remaining: 1045504,
    shortfall: 0,
    fits: true,
  };

  window.__a11yMock = { requests: [], fail: null };

  // Every other action briefly reports "busy", like a real operation.
  const QUICK_ACTIONS = ['init', 'ready', 'preferences', 'pick'];

  function answer(action, a) {
    const mock = window.__a11yMock;
    switch (action) {
      case 'init':
        return { prefs, version, categories: categories(), args: [], update_report: null };
      case 'ready':
        return true;
      case 'preferences':
        prefs = { ...a };
        return prefs;
      case 'library':
        return { root: prefs.library, records, categories: categories() };
      case 'drive_browser':
        return {
          root: prefs.destination,
          records,
          categories: categories(),
          identity: { token: 'fixture' },
          warnings: [],
          game_bytes: 12288,
          available: 1048576,
          capacity: 2097152,
        };
      case 'installed_versions':
        return { installed: true, version };
      case 'pick':
        if (a.kind === 'discs') return ['/fixture/source.iso'];
        if (a.kind === 'archive') return backup.path;
        return '/fixture/usb';
      case 'scan':
        // One disc that matched nothing and still needs a category.
        mock.plan = {
          entries: [
            {
              source: '/fixture/source.iso',
              title: 'Example import',
              category: '',
              region: 'USA',
              target: '',
              selected: false,
              blocked: false,
              confidence: 'audio-only',
              reason: 'Choose a category',
              files: [],
              candidates: [],
              game: null,
            },
          ],
          catalog: { games: [{ release: 'Example import' }] },
        };
        return mock.plan;
      case 'edit_import': {
        const entry = mock.plan.entries[a.index];
        if (a.category) {
          entry.category = a.category;
          entry.target = a.category + '/USA/Example/Example.cue';
          entry.confidence = 'unverified';
        }
        if (a.selected !== undefined) entry.selected = a.selected;
        return mock.plan;
      }
      case 'select_import':
        for (const entry of mock.plan.entries)
          entry.selected = a.selected && !entry.blocked && !!entry.category && !!entry.target;
        return mock.plan;
      case 'drive_inspect':
        return { root: a.root, token: 'fixture', identity: { name: 'Fixture drive' }, mismatch: false };
      case 'copy_preview':
        return {
          destination: prefs.destination,
          drive: { identity: { name: 'Fixture drive' } },
          games: 1,
          files: [{ path: 'Games/USA/Alpha/Alpha.iso', status: 'copy' }],
          folders_to_move: 0,
          storage,
        };
      case 'copy':
        return { copied: 1 };
      case 'backup_open':
        return { ...backup };
      case 'backup_entry':
        return { name: 'ExampleSave', data: [0, 1, 2, 3], hex: '00 01 02 03', profile: null };
      case 'slots':
        return [{ path: backup.path, name: 'First slot', valid: true, games: 'Example game', notes: '' }];
      case 'cheats_load':
        return JSON.parse(JSON.stringify(cheats));
      case 'game_info_load':
        return {
          fields: {
            description: 'Fixture description',
            genre: 'Action',
            developer: '',
            publisher: '',
            release_date: '',
            year: '',
            players: '',
          },
          records: [
            {
              path: records[0].path,
              metadata: { description: 'Fixture description', genre: 'Action' },
              overrides: {},
            },
          ],
        };
      default:
        throw 'Unhandled fixture operation: ' + action;
    }
  }

  window.__TAURI__ = {
    event: {
      listen: async (name, handler) => {
        listeners[name] = handler;
        return () => {};
      },
    },
    core: {
      invoke: async (command, input = {}) => {
        if (command !== 'request') return true;
        const { action, args: a = {} } = input;
        window.__a11yMock.requests.push({ action, args: a });
        if (!QUICK_ACTIONS.includes(action)) {
          listeners.busy?.({ payload: true });
          await new Promise((resolve) => setTimeout(resolve, 60));
          listeners.busy?.({ payload: false });
        }
        if (window.__a11yMock.fail === action) throw 'Fixture operation failed';
        return answer(action, a);
      },
    },
  };
})();
"""

# Lists the visible, enabled form controls in the front dialog (or the page)
# that have no label, aria-label, aria-labelledby or title.
UNLABELED_CONTROLS_SCRIPT = r"""
const root = [...document.querySelectorAll('dialog[open]')].at(-1) || document;
const unlabeled = [...root.querySelectorAll('input,select,textarea')].filter((node) => {
  const visible = node.getClientRects().length && !node.disabled && node.type !== 'hidden';
  const labeled =
    node.labels?.length ||
    node.getAttribute('aria-label') ||
    node.getAttribute('aria-labelledby') ||
    node.title;
  return visible && !labeled;
});
return unlabeled.map((node) => node.id || node.outerHTML.slice(0, 180));
"""

# Lists layout problems: horizontal page scrolling, controls outside the
# window (except inside areas meant to scroll), and a front dialog taller
# than the window or with its actions outside it.
LAYOUT_PROBLEMS_SCRIPT = r"""
const result = [];
if (document.documentElement.scrollWidth > innerWidth + 2) {
  result.push('document scrolls horizontally');
}
const front = [...document.querySelectorAll('dialog[open]')].at(-1);
const root = front || document.querySelector('#content');
for (const node of root.querySelectorAll('button,input,select,textarea')) {
  if (!node.getClientRects().length || node.closest('details:not([open])') || node.disabled) {
    continue;
  }
  const box = node.getBoundingClientRect();
  if (box.width < 1 || box.height < 1) continue;
  const scroller = node.closest('.table-wrap,.dialog-body,.dialog-foot,.selection-bar,.entry-list');
  if (!scroller && (box.left < -2 || box.right > innerWidth + 2)) {
    result.push((node.id || node.textContent.trim() || node.tagName).slice(0, 80));
  }
}
if (front) {
  const box = front.getBoundingClientRect();
  const foot = front.querySelector('.dialog-foot').getBoundingClientRect();
  if (box.top < 0 || box.bottom > innerHeight + 2) result.push('dialog exceeds viewport');
  if (foot.bottom > box.bottom + 2 || foot.top < box.top) {
    result.push('dialog actions outside dialog');
  }
}
return [...new Set(result)];
"""

# Lists (up to 15) visible text runs whose contrast with the background
# behind them is below WCAG AA: 4.5:1, or 3:1 for large text (24px, or
# 18.66px bold).
LOW_CONTRAST_SCRIPT = r"""
const rgb = (color) => (color.match(/[0-9.]+/g) || []).map(Number);

// Relative luminance, as WCAG defines it.
const luminance = (color) => {
  const channels = rgb(color).slice(0, 3).map((value) => {
    value /= 255;
    return value <= 0.04045 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
  });
  return channels[0] * 0.2126 + channels[1] * 0.7152 + channels[2] * 0.0722;
};

// A fully transparent background shows its parent's instead.
const isTransparent = (element) => {
  const channels = rgb(getComputedStyle(element).backgroundColor);
  return channels.length === 4 && !channels[3];
};

const root = [...document.querySelectorAll('dialog[open]')].at(-1) || document.querySelector('#content');
const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
const failed = [];
for (let text; (text = walker.nextNode()); ) {
  if (!text.textContent.trim()) continue;
  const node = text.parentElement;
  if (
    !node.getClientRects().length ||
    node.closest('[aria-hidden=true],[disabled],.sr-only,script,style')
  ) {
    continue;
  }

  let background = node;
  while (background && isTransparent(background)) background = background.parentElement;

  const style = getComputedStyle(node);
  const foreground = luminance(style.color);
  const behind = luminance(
    background ? getComputedStyle(background).backgroundColor : 'rgb(255,255,255)'
  );
  const ratio = (Math.max(foreground, behind) + 0.05) / (Math.min(foreground, behind) + 0.05);

  const size = parseFloat(style.fontSize);
  const isLarge = size >= 24 || (size >= 18.66 && Number(style.fontWeight) >= 700);
  const threshold = isLarge ? 3 : 4.5;
  if (ratio + 0.01 < threshold) {
    failed.push({
      text: text.textContent.trim().slice(0, 70),
      ratio: Math.round(ratio * 100) / 100,
      color: style.color,
    });
  }
}
return failed.slice(0, 15);
"""

# Sets a <select> or <input> and fires "change" as a person's edit would.
SET_VALUE_SCRIPT = (
    "arguments[0].value=arguments[1];"
    'arguments[0].dispatchEvent(new Event("change",{bubbles:true}))'
)


def mock_script(version):
    """The bridge mock, with six fixture games, the real cheat database and
    the app's version filled in."""
    records = [
        dict(
            path=f"Games/USA/{title}/{title}.iso",
            title=title,
            category="Games",
            region="USA",
            metadata={"genre": "Action"},
            bytes=2048,
        )
        for title in GAME_TITLES
    ]
    cheats = json.loads((ROOT / "data/save-cheats.json").read_text())
    return (
        MOCK_SCRIPT.replace("__RECORDS__", json.dumps(records))
        .replace("__CHEATS__", json.dumps(cheats))
        .replace("__VERSION__", json.dumps(version))
    )


def start_server(mock):
    """Serve desktop/ui/ on a free local port, with the mock loaded first in
    index.html's <head>. Returns the server (running on its own thread)."""

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path not in ("/", "/index.html", "/mock.js"):
                super().do_GET()
                return
            if self.path == "/mock.js":
                content = mock
                content_type = "text/javascript"
            else:
                index = (UI_FOLDER / "index.html").read_text()
                content = index.replace("<head>", '<head><script src="mock.js"></script>')
                content_type = "text/html; charset=utf-8"
            body = content.encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), functools.partial(Handler, directory=str(UI_FOLDER))
    )
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


class Audit:
    """The browser session and the running list of passed and failed checks."""

    def __init__(self, driver):
        self.driver = driver
        self.wait = WebDriverWait(driver, ELEMENT_TIMEOUT_SECONDS)
        self.checks = []
        self.findings = []

    def check(self, condition, message):
        (self.checks if condition else self.findings).append(message)
        print(("PASS " if condition else "FAIL ") + message, flush=True)

    def js(self, script, *values):
        return self.driver.execute_script(script, *values)

    def until(self, condition):
        """Wait (up to ELEMENT_TIMEOUT_SECONDS) for `condition()` to be truthy."""
        return self.wait.until(lambda _: condition())

    def element(self, selector):
        """Wait for the first visible element matching a CSS selector."""

        def first_visible():
            nodes = self.driver.find_elements(By.CSS_SELECTOR, selector)
            return next((node for node in nodes if node.is_displayed()), None)

        return self.until(first_visible)

    def exists(self, selector):
        return self.driver.find_elements(By.CSS_SELECTOR, selector)

    def focused(self):
        return self.driver.switch_to.active_element

    def focused_data(self, key):
        """A data-* attribute of the focused element."""
        return self.js(f"return document.activeElement.dataset.{key}")

    def activate(self, selector):
        """Press Enter on an element, as a keyboard user would."""
        self.element(selector).send_keys(Keys.ENTER)
        time.sleep(0.16)

    def page(self, name):
        self.activate(f'[data-page="{name}"]')
        self.until(
            lambda: self.driver.find_element(By.CSS_SELECTOR, f'[data-page="{name}"].active')
        )

    def wait_for_dialogs_closed(self):
        self.until(lambda: not self.exists("dialog[open]"))

    def set_value(self, element, value):
        self.js(SET_VALUE_SCRIPT, element, value)

    def audit_labels(self, name):
        missing = self.js(UNLABELED_CONTROLS_SCRIPT)
        self.check(
            not missing, f'{name}: form controls have explicit accessible labels {missing or ""}'
        )

    def audit_layout(self, name):
        failures = self.js(LAYOUT_PROBLEMS_SCRIPT)
        self.check(not failures, f'{name}: controls reflow inside the viewport {failures or ""}')

    def audit_contrast(self, name):
        failures = self.js(LOW_CONTRAST_SCRIPT)
        self.check(
            not failures, f'{name}: text contrast meets measured size thresholds {failures or ""}'
        )


def check_game_name_exposed(audit, tile):
    """Screen readers should hear the game's full name. Not every WebKit
    driver can report the computed name, so that is only noted."""
    try:
        audit.check(
            tile.accessible_name == "Alpha",
            "WebKit exposes the full game name to accessibility clients",
        )
    except Exception as error:
        print(
            "NOTE WebKit computed-name endpoint unavailable: " + str(error).splitlines()[0],
            flush=True,
        )


def check_game_selection(audit):
    """Space selects the focused game and keeps focus on it. Returns the
    game's key."""
    tile = audit.element("[data-game]")
    key = tile.get_attribute("data-game")
    check_game_name_exposed(audit, tile)
    tile.send_keys(Keys.SPACE)
    time.sleep(0.2)
    audit.check(
        audit.focused_data("game") == key,
        "Selecting a game with Space retains focus on that game",
    )
    audit.check(
        audit.element("[data-game]").get_attribute("aria-pressed") == "true",
        "Space selects the game",
    )
    return key


def check_game_grid_keyboard(audit, key):
    """The game grid is one Tab stop navigated with the arrow keys, Home and
    End, and Tab moves on to the selection's actions."""
    audit.element("[data-game]").send_keys(Keys.ARROW_RIGHT)
    audit.check(audit.focused_data("game") != key, "Right arrow moves focus between games")
    audit.check(
        len(audit.exists('[data-game][aria-pressed="true"]')) == 1,
        "Arrow navigation does not change selection",
    )
    audit.check(
        len(audit.exists('[data-game][tabindex="0"]')) == 1,
        "The game collection uses one Tab stop",
    )

    audit.focused().send_keys(Keys.END)
    audit.check(
        audit.js(
            'return document.activeElement===document.querySelector("[data-game]:last-child")'
        ),
        "End reaches the last game",
    )
    audit.focused().send_keys(Keys.HOME)
    audit.check(
        audit.js('return document.activeElement===document.querySelector("[data-game]")'),
        "Home reaches the first game",
    )

    audit.focused().send_keys(Keys.TAB)
    audit.check(
        audit.focused_data("action") == "game-details",
        "Tab leaves the game collection for selection actions",
    )
    audit.focused().send_keys(Keys.SHIFT, Keys.TAB)
    audit.check(audit.focused_data("game") == key, "Shift+Tab returns to the focused game")


def check_dialog_focus_wraps(audit):
    """Tab and Shift+Tab cycle within the open copy dialog."""
    audit.element("dialog h2").send_keys(Keys.SHIFT, Keys.TAB)
    backward = audit.js(
        "return {action:document.activeElement.dataset.action,"
        "tag:document.activeElement.tagName,id:document.activeElement.id}"
    )
    audit.check(
        backward.get("action") == "copy-preview",
        f"Shift+Tab from a dialog heading reaches its last action {backward}",
    )
    audit.focused().send_keys(Keys.TAB)
    audit.check(
        audit.focused_data("action") == "close-dialog",
        "Tab wraps from the last dialog action to Close",
    )


def check_copy_dialog(audit, baseline):
    """Open the copy dialog, move on to its review, and close it with Escape;
    focus must stay in the dialog and then return to the button that opened it."""
    audit.activate('[data-action="copy-open"]')
    audit.element("dialog[open]")
    audit.audit_labels("Copy dialog")
    if not baseline:
        check_dialog_focus_wraps(audit)

    audit.activate('[data-action="copy-preview"]')
    audit.until(lambda: audit.driver.find_element(By.CSS_SELECTOR, '[data-action="copy-apply"]'))
    audit.check(
        audit.js('return !!document.activeElement.closest("dialog[open]")'),
        "Copy review keeps focus inside its replacement dialog",
    )
    audit.focused().send_keys(Keys.ESCAPE)
    audit.wait_for_dialogs_closed()
    audit.check(
        audit.focused_data("action") == "copy-open",
        "Closing copy returns focus to its opener",
    )


def check_error_announced(audit):
    """A failed operation's message is read out from inside the dialog."""
    alert_text = 'return document.querySelector("dialog [data-announcer=alert]")?.textContent'
    audit.activate('[data-action="copy-open"]')
    audit.js("window.__a11yMock.fail='copy_preview'")
    audit.activate('[data-action="copy-preview"]')
    audit.until(lambda: audit.js(alert_text))
    audit.check(
        "Fixture operation failed" in audit.js(alert_text),
        "Operation errors are announced inside the modal",
    )
    audit.js("window.__a11yMock.fail=null")
    audit.focused().send_keys(Keys.ESCAPE)
    audit.wait_for_dialogs_closed()


def check_import_review(audit):
    """An unmatched import can't be selected until it has a category, and
    a chosen category (including a custom one) sticks."""
    audit.page("library")
    audit.activate('[data-action="add-discs"]')
    audit.activate('[data-action="import-files"]')
    audit.activate('[data-action="scan"]')
    audit.until(lambda: audit.driver.find_element(By.CSS_SELECTOR, "[data-import-check]"))
    audit.audit_labels("Import review")
    audit.check(
        not audit.element("[data-import-check]").is_enabled(),
        "Unresolved imports cannot be selected",
    )
    audit.check(
        audit.element("[data-import-category]").get_attribute("value") == "",
        "Missing category has an explicit placeholder",
    )

    audit.set_value(audit.element("[data-import-category]"), "Bonus Discs")
    audit.until(lambda: audit.element("[data-import-check]").is_enabled())
    audit.check(
        audit.element("[data-import-category]").get_attribute("value") == "Bonus Discs",
        "Unmatched import retains Bonus Discs after refresh",
    )

    audit.set_value(audit.element("[data-import-category]"), "Music")
    audit.until(lambda: "Music/USA" in audit.element("td.path").text)
    audit.check(
        audit.element("[data-import-category]").get_attribute("value") == "Music",
        "Custom import category remains selectable",
    )

    audit.element("[data-import-check]").send_keys(Keys.SPACE)
    audit.until(lambda: "Import 1 selected" in audit.element('[data-action="import-apply"]').text)
    audit.check(
        audit.element("[data-import-check]").is_selected(), "Resolved import can be selected"
    )


def check_cheat_editor_escape(audit):
    """Escape must use the same dirty-edit protection as the Close button."""
    field = audit.element('[data-profile-key="game"]')
    field.send_keys(Keys.END, " edited", Keys.TAB)
    audit.focused().send_keys(Keys.ESCAPE)
    audit.until(lambda: audit.driver.find_element(By.CSS_SELECTOR, "#confirm-dialog[open]"))
    audit.check(
        audit.element("#cheats-dialog").is_displayed(),
        "Escape keeps unsaved database edits behind a confirmation",
    )
    audit.check(
        audit.focused_data("answer") == "cancel",
        "Discard confirmation initially focuses Cancel",
    )

    # Escape cancels the confirmation and keeps the edit.
    audit.focused().send_keys(Keys.ESCAPE)
    audit.until(lambda: not audit.exists("#confirm-dialog[open]"))
    audit.check(
        audit.element('[data-profile-key="game"]').get_attribute("value").endswith(" edited"),
        "Canceling discard preserves the database draft",
    )

    # Escape again, and this time discard.
    audit.focused().send_keys(Keys.ESCAPE)
    audit.activate('#confirm-dialog [data-answer="yes"]')
    audit.wait_for_dialogs_closed()


def check_themes(audit):
    for theme in ["light", "dark"]:
        audit.set_value(audit.element("#pref-theme"), theme)
        audit.page("library")
        audit.audit_contrast(theme + " Library")
        audit.page("settings")
        audit.audit_contrast(theme + " Settings")


def check_text_scale(audit, width, height, scale):
    """At one window size and text size: the setting applies and is saved,
    every page and the copy dialog fit, and the dialog keeps its contrast."""
    audit.driver.set_window_size(width, height)
    audit.page("settings")
    audit.set_value(audit.element("#pref-text-scale"), str(scale))
    time.sleep(0.2)
    audit.check(
        audit.js("return document.documentElement.dataset.textScale") == str(scale),
        f"{scale}% text size applies",
    )
    saved_scale = audit.js(
        "return window.__a11yMock.requests"
        '.filter(r=>r.action==="preferences").at(-1).args.text_scale'
    )
    audit.check(saved_scale == scale, f"{scale}% text size is persisted")

    for name in ["library"] + PAGES:
        audit.page(name)
        audit.audit_layout(f"{name} {width}×{height} at {scale}%")

    # Open the copy dialog, selecting a game first if none is selected.
    audit.page("library")
    if not audit.exists('[data-action="copy-open"]'):
        audit.element("[data-game]").send_keys(Keys.SPACE)
    if width <= COMPACT_WIDTH:
        audit.activate('[data-action="selection-actions"]')
        audit.check(
            audit.element("dialog[open] h2").text == "Selected games",
            "Compact selection actions are keyboard accessible",
        )
    audit.activate('[data-action="copy-open"]')
    audit.audit_layout(f"Copy dialog {width}×{height} at {scale}%")
    audit.audit_contrast(f"Copy dialog at {scale}%")
    audit.driver.save_screenshot(str(WORK / f"copy-{width}-{scale}.png"))
    audit.element("dialog[open] h2").send_keys(Keys.ESCAPE)
    audit.wait_for_dialogs_closed()


def check_focused_title_visible(audit):
    """In the small window the selection bar covers the bottom of the page;
    the focused game's title must stay above it."""
    audit.page("library")
    audit.element("[data-game]").send_keys(Keys.HOME)
    audit.check(
        audit.js(
            'const title=document.activeElement.querySelector(".game-title").getBoundingClientRect(),'
            'bar=document.querySelector(".selection-bar").getBoundingClientRect();'
            "return title.top>=0&&title.bottom<=bar.top+1"
        ),
        "Focused game title stays visible above compact selection actions",
    )


def run_checks(audit, port, baseline):
    driver = audit.driver
    driver.set_window_size(1280, 900)
    driver.get(f"http://127.0.0.1:{port}/")
    audit.until(lambda: len(audit.exists("[data-game]")) == len(GAME_TITLES))

    key = check_game_selection(audit)
    if not baseline:
        check_game_grid_keyboard(audit, key)
    audit.audit_labels("Library")
    audit.audit_contrast("Light Library")

    check_copy_dialog(audit, baseline)
    if not baseline:
        check_error_announced(audit)

    for name in PAGES:
        audit.page(name)
        audit.audit_labels(name)

    check_import_review(audit)

    audit.page("saves")
    audit.activate('[data-action="backup-open"]')
    audit.until(lambda: driver.find_element(By.CSS_SELECTOR, "#hex-editor"))
    audit.audit_labels("Save editor")

    audit.page("settings")
    audit.activate('[data-action="cheats-open"]')
    audit.element("dialog[open]")
    audit.audit_labels("Cheat database editor")
    if not baseline:
        check_cheat_editor_escape(audit)
        check_themes(audit)
        for width, height, scale in TEXT_SCALE_SIZES:
            check_text_scale(audit, width, height, scale)
        check_focused_title_visible(audit)

    driver.save_screenshot(str(WORK / ("baseline.png" if baseline else "tested.png")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", action="store_true", help="Record findings without failing")
    args = parser.parse_args()

    WORK.mkdir(parents=True, exist_ok=True)
    version = json.loads((ROOT / "desktop/shell/tauri.conf.json").read_text())["version"]
    server = start_server(mock_script(version))
    driver = webdriver.WebKitGTK()
    audit = Audit(driver)
    try:
        run_checks(audit, server.server_port, args.baseline)
    except Exception:
        driver.save_screenshot(str(WORK / "failure.png"))
        raise
    finally:
        driver.quit()
        server.shutdown()

    report = {
        "version": version,
        "passed": audit.checks,
        "failed": audit.findings,
        "environment": "WebKitGTK frontend; mocked native bridge",
    }
    (WORK / ("baseline.json" if args.baseline else "results.json")).write_text(
        json.dumps(report, indent=2) + "\n"
    )
    if audit.findings and not args.baseline:
        raise SystemExit(f"{len(audit.findings)} accessibility checks failed")


if __name__ == "__main__":
    main()
