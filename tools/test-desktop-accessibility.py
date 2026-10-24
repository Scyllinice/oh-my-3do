#!/usr/bin/env python3
"""Exercise the real frontend in WebKitGTK with a disposable, mocked native bridge.

Run under D-Bus/Xvfb in tools/desktop-tauri.Dockerfile. No games, physical drives,
network downloads or production preferences are touched. Native bridge/file I/O
is covered separately by test-desktop-ui.py and the Rust suites.
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
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline', action='store_true', help='Record findings without failing')
args = parser.parse_args()
work = ROOT / 'build' / 'desktop-accessibility'
work.mkdir(parents=True, exist_ok=True)
version = json.loads((ROOT / 'desktop/shell/tauri.conf.json').read_text())['version']
cheats = json.loads((ROOT / 'data/save-cheats.json').read_text())
records = [dict(path=f'Games/USA/{title}/{title}.iso', title=title, category='Games',
                region='USA', metadata={'genre': 'Action'}, bytes=2048)
           for title in ['Alpha', 'Beta', 'Gamma', 'Delta', 'Epsilon', 'Zeta']]
mock = r"""
const records=RECORDS,cheats=CHEATS;
let prefs={library:'/fixture/library',destination:'/fixture/usb',offline:true,disable_update_checks:true,theme:'light'};
const listeners={};
const backup={path:'/fixture/usb/.ohmy3do/saves/slot01.3ds',dirty:false,bytes:100,entries:[{name:'ExampleSave',known:'Example game',size:4,file_type:0}]};
const storage={games:2048,covers:0,previews:0,support:0,total:2048,to_copy:2048,reserve:1024,required:3072,available:1048576,remaining:1045504,shortfall:0,fits:true};
window.__a11yMock={requests:[],fail:null};
window.__TAURI__={event:{listen:async(name,fn)=>{listeners[name]=fn;return ()=>{};}},core:{invoke:async(command,input={})=>{
 if(command!=='request')return true;
 const {action,args:a={}}=input;window.__a11yMock.requests.push({action,args:a});
 if(!['init','ready','preferences','pick'].includes(action)){
  listeners.busy?.({payload:true});await new Promise(resolve=>setTimeout(resolve,60));listeners.busy?.({payload:false});
 }
 if(window.__a11yMock.fail===action)throw 'Fixture operation failed';
 switch(action){
 case 'init':return {prefs,version:VERSION,categories:['Games'],args:[],update_report:null};
 case 'ready':return true;
 case 'preferences':prefs={...a};return prefs;
 case 'library':return {root:prefs.library,records,categories:['Games']};
 case 'drive_browser':return {root:prefs.destination,records,categories:['Games'],identity:{token:'fixture'},warnings:[],game_bytes:12288,available:1048576,capacity:2097152};
 case 'installed_versions':return {installed:true,version:VERSION};
 case 'pick':return a.kind==='discs'?['/fixture/source.iso']:a.kind==='archive'?backup.path:'/fixture/usb';
 case 'scan':return {entries:[{source:'/fixture/source.iso',title:'Example import',category:'Games',region:'USA',target:'Games/USA/Example/Example.iso',selected:true,blocked:false,confidence:'name',reason:'Fixture match',files:[],candidates:[],game:0}],catalog:{games:[{release:'Example import'}]}};
 case 'drive_inspect':return {root:a.root,token:'fixture',identity:{name:'Fixture drive'},mismatch:false};
 case 'copy_preview':return {destination:prefs.destination,drive:{identity:{name:'Fixture drive'}},games:1,files:[{path:'Games/USA/Alpha/Alpha.iso',status:'copy'}],folders_to_move:0,storage};
 case 'copy':return {copied:1};
 case 'backup_open':return {...backup};
 case 'backup_entry':return {name:'ExampleSave',data:[0,1,2,3],hex:'00 01 02 03',profile:null};
 case 'slots':return [{path:backup.path,name:'First slot',valid:true,games:'Example game',notes:''}];
 case 'cheats_load':return JSON.parse(JSON.stringify(cheats));
 case 'game_info_load':return {fields:{description:'Fixture description',genre:'Action',developer:'',publisher:'',release_date:'',year:'',players:''},records:[{path:records[0].path,metadata:{description:'Fixture description',genre:'Action'},overrides:{}}]};
 default:throw 'Unhandled fixture operation: '+action;
 }
}}};
""".replace('RECORDS', json.dumps(records)).replace('CHEATS', json.dumps(cheats)).replace('VERSION', json.dumps(version))


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path in ('/', '/index.html', '/mock.js'):
            content = mock if self.path == '/mock.js' else (ROOT / 'desktop/ui/index.html').read_text().replace('<head>', '<head><script src="mock.js"></script>')
            content = content.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/javascript' if self.path.endswith('.js') else 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(content)))
            self.end_headers()
            self.wfile.write(content)
        else:
            super().do_GET()


server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), functools.partial(Handler, directory=str(ROOT / 'desktop/ui')))
threading.Thread(target=server.serve_forever, daemon=True).start()
driver = webdriver.WebKitGTK()
wait = WebDriverWait(driver, 8)
findings = []
checks = []


def check(condition, message):
    (checks if condition else findings).append(message)
    print(('PASS ' if condition else 'FAIL ') + message, flush=True)


def js(script, *values):
    return driver.execute_script(script, *values)


def element(selector):
    return wait.until(lambda _: next((n for n in driver.find_elements(By.CSS_SELECTOR, selector) if n.is_displayed()),None))


def activate(selector):
    node = element(selector)
    node.send_keys(Keys.ENTER)
    time.sleep(.16)


def page(name):
    activate(f'[data-page="{name}"]')
    wait.until(lambda _: driver.find_element(By.CSS_SELECTOR, f'[data-page="{name}"].active'))


def audit_labels(name):
    missing = js("""const root=[...document.querySelectorAll('dialog[open]')].at(-1)||document;return [...root.querySelectorAll('input,select,textarea')].filter(n=>n.getClientRects().length&&!n.disabled&&n.type!=='hidden'&&!(n.labels?.length||n.getAttribute('aria-label')||n.getAttribute('aria-labelledby')||n.title)).map(n=>n.id||n.outerHTML.slice(0,180));""")
    check(not missing, f'{name}: form controls have explicit accessible labels {missing or ""}')


def audit_layout(name):
    failures = js("""
     const result=[];
     if(document.documentElement.scrollWidth>innerWidth+2)result.push('document scrolls horizontally');
     const front=[...document.querySelectorAll('dialog[open]')].at(-1);
     const root=front||document.querySelector('#content');
     for(const node of root.querySelectorAll('button,input,select,textarea')){
      if(!node.getClientRects().length||node.closest('details:not([open])')||node.disabled)continue;
      const r=node.getBoundingClientRect();if(r.width<1||r.height<1)continue;
      const scroll=node.closest('.table-wrap,.dialog-body,.dialog-foot,.selection-bar,.entry-list');
      if(!scroll&&(r.left< -2||r.right>innerWidth+2))result.push((node.id||node.textContent.trim()||node.tagName).slice(0,80));
     }
     if(front){
      const r=front.getBoundingClientRect(),foot=front.querySelector('.dialog-foot').getBoundingClientRect();
      if(r.top<0||r.bottom>innerHeight+2)result.push('dialog exceeds viewport');
      if(foot.bottom>r.bottom+2||foot.top<r.top)result.push('dialog actions outside dialog');
     }
     return [...new Set(result)];
    """)
    check(not failures, f'{name}: controls reflow inside the viewport {failures or ""}')


def audit_contrast(name):
    failures = js("""
     const rgb=color=>(color.match(/[0-9.]+/g)||[]).map(Number);
     const light=color=>{const c=rgb(color).slice(0,3).map(v=>{v/=255;return v<=.04045?v/12.92:((v+.055)/1.055)**2.4;});return c[0]*.2126+c[1]*.7152+c[2]*.0722;};
     const root=[...document.querySelectorAll('dialog[open]')].at(-1)||document.querySelector('#content');
     const walker=document.createTreeWalker(root,NodeFilter.SHOW_TEXT);const failed=[];
     for(let text; text=walker.nextNode();){
      if(!text.textContent.trim())continue;const node=text.parentElement;
      if(!node.getClientRects().length||node.closest('[aria-hidden=true],[disabled],.sr-only,script,style'))continue;
      let background=node;while(background&&(!rgb(getComputedStyle(background).backgroundColor)[3]&&rgb(getComputedStyle(background).backgroundColor).length===4))background=background.parentElement;
      const style=getComputedStyle(node),a=light(style.color),b=light(background?getComputedStyle(background).backgroundColor:'rgb(255,255,255)');
      const ratio=(Math.max(a,b)+.05)/(Math.min(a,b)+.05);
      const threshold=parseFloat(style.fontSize)>=24||parseFloat(style.fontSize)>=18.66&&Number(style.fontWeight)>=700?3:4.5;
      if(ratio+.01<threshold)failed.push({text:text.textContent.trim().slice(0,70),ratio:Math.round(ratio*100)/100,color:style.color});
     }
     return failed.slice(0,15);
    """)
    check(not failures, f'{name}: text contrast meets measured size thresholds {failures or ""}')


try:
    driver.set_window_size(1280, 900)
    driver.get(f'http://127.0.0.1:{server.server_port}/')
    wait.until(lambda _: len(driver.find_elements(By.CSS_SELECTOR, '[data-game]')) == 6)
    tile = element('[data-game]')
    key = tile.get_attribute('data-game')
    try:
        check(tile.accessible_name=='Alpha', 'WebKit exposes the full game name to accessibility clients')
    except Exception as error:
        print('NOTE WebKit computed-name endpoint unavailable: '+str(error).splitlines()[0],flush=True)
    tile.send_keys(Keys.SPACE)
    time.sleep(.2)
    check(js('return document.activeElement.dataset.game') == key, 'Selecting a game with Space retains focus on that game')
    check(element('[data-game]').get_attribute('aria-pressed') == 'true', 'Space selects the game')
    if not args.baseline:
        element('[data-game]').send_keys(Keys.ARROW_RIGHT)
        check(js('return document.activeElement.dataset.game') != key, 'Right arrow moves focus between games')
        check(len(driver.find_elements(By.CSS_SELECTOR, '[data-game][aria-pressed="true"]')) == 1, 'Arrow navigation does not change selection')
        check(len(driver.find_elements(By.CSS_SELECTOR, '[data-game][tabindex="0"]')) == 1, 'The game collection uses one Tab stop')
        driver.switch_to.active_element.send_keys(Keys.END)
        check(js('return document.activeElement===document.querySelector("[data-game]:last-child")'), 'End reaches the last game')
        driver.switch_to.active_element.send_keys(Keys.HOME)
        check(js('return document.activeElement===document.querySelector("[data-game]")'), 'Home reaches the first game')
        driver.switch_to.active_element.send_keys(Keys.TAB)
        check(js('return document.activeElement.dataset.action') == 'game-details', 'Tab leaves the game collection for selection actions')
        driver.switch_to.active_element.send_keys(Keys.SHIFT,Keys.TAB)
        check(js('return document.activeElement.dataset.game') == key, 'Shift+Tab returns to the focused game')
    audit_labels('Library')
    audit_contrast('Light Library')
    activate('[data-action="copy-open"]')
    element('dialog[open]')
    audit_labels('Copy dialog')
    if not args.baseline:
        element('dialog h2').send_keys(Keys.SHIFT,Keys.TAB)
        backward=js('return {action:document.activeElement.dataset.action,tag:document.activeElement.tagName,id:document.activeElement.id}')
        check(backward.get('action') == 'copy-preview', f'Shift+Tab from a dialog heading reaches its last action {backward}')
        driver.switch_to.active_element.send_keys(Keys.TAB)
        check(js('return document.activeElement.dataset.action') == 'close-dialog', 'Tab wraps from the last dialog action to Close')
    activate('[data-action="copy-preview"]')
    wait.until(lambda _: driver.find_element(By.CSS_SELECTOR, '[data-action="copy-apply"]'))
    check(js('return !!document.activeElement.closest("dialog[open]")'), 'Copy review keeps focus inside its replacement dialog')
    driver.switch_to.active_element.send_keys(Keys.ESCAPE)
    wait.until(lambda _: not driver.find_elements(By.CSS_SELECTOR, 'dialog[open]'))
    check(js('return document.activeElement.dataset.action') == 'copy-open', 'Closing copy returns focus to its opener')
    if not args.baseline:
        activate('[data-action="copy-open"]')
        js("window.__a11yMock.fail='copy_preview'")
        activate('[data-action="copy-preview"]')
        wait.until(lambda _: js('return document.querySelector("dialog [data-announcer=alert]")?.textContent'))
        check('Fixture operation failed' in js('return document.querySelector("dialog [data-announcer=alert]").textContent'), 'Operation errors are announced inside the modal')
        js('window.__a11yMock.fail=null')
        driver.switch_to.active_element.send_keys(Keys.ESCAPE)
        wait.until(lambda _: not driver.find_elements(By.CSS_SELECTOR, 'dialog[open]'))
    for name in ['saves', 'media', 'drive', 'settings', 'updates']:
        page(name)
        audit_labels(name)
    page('library')
    activate('[data-action="add-discs"]')
    activate('[data-action="import-files"]')
    activate('[data-action="scan"]')
    wait.until(lambda _: driver.find_element(By.CSS_SELECTOR, '[data-import-check]'))
    audit_labels('Import review')
    page('saves')
    activate('[data-action="backup-open"]')
    wait.until(lambda _: driver.find_element(By.CSS_SELECTOR, '#hex-editor'))
    audit_labels('Save editor')
    page('settings')
    activate('[data-action="cheats-open"]')
    element('dialog[open]')
    audit_labels('Cheat database editor')
    if not args.baseline:
        # Escape must use the same dirty-edit protection as the Close button.
        field = element('[data-profile-key="game"]')
        field.send_keys(Keys.END, ' edited', Keys.TAB)
        driver.switch_to.active_element.send_keys(Keys.ESCAPE)
        wait.until(lambda _: driver.find_element(By.CSS_SELECTOR, '#confirm-dialog[open]'))
        check(element('#cheats-dialog').is_displayed(), 'Escape keeps unsaved database edits behind a confirmation')
        check(js('return document.activeElement.dataset.answer') == 'cancel', 'Discard confirmation initially focuses Cancel')
        driver.switch_to.active_element.send_keys(Keys.ESCAPE)
        wait.until(lambda _: not driver.find_elements(By.CSS_SELECTOR, '#confirm-dialog[open]'))
        check(element('[data-profile-key="game"]').get_attribute('value').endswith(' edited'), 'Canceling discard preserves the database draft')
        driver.switch_to.active_element.send_keys(Keys.ESCAPE)
        activate('#confirm-dialog [data-answer="yes"]')
        wait.until(lambda _: not driver.find_elements(By.CSS_SELECTOR, 'dialog[open]'))

        for theme in ['light','dark']:
            select = element('#pref-theme')
            js('arguments[0].value=arguments[1];arguments[0].dispatchEvent(new Event("change",{bubbles:true}))', select, theme)
            page('library')
            audit_contrast(theme+' Library')
            page('settings')
            audit_contrast(theme+' Settings')

        for width,height,scale in [(1280,900,100),(960,720,150),(640,480,200)]:
            driver.set_window_size(width,height)
            page('settings')
            scale_node=element('#pref-text-scale')
            js('arguments[0].value=arguments[1];arguments[0].dispatchEvent(new Event("change",{bubbles:true}))', scale_node, str(scale))
            time.sleep(.2)
            check(js('return document.documentElement.dataset.textScale') == str(scale), f'{scale}% text size applies')
            check(js('return window.__a11yMock.requests.filter(r=>r.action==="preferences").at(-1).args.text_scale') == scale, f'{scale}% text size is persisted')
            for name in ['library','saves','media','drive','settings','updates']:
                page(name)
                audit_layout(f'{name} {width}×{height} at {scale}%')
            page('library')
            if not driver.find_elements(By.CSS_SELECTOR, '[data-action="copy-open"]'):
                element('[data-game]').send_keys(Keys.SPACE)
            if width<=760:
                activate('[data-action="selection-actions"]')
                check(element('dialog[open] h2').text=='Selected games', 'Compact selection actions are keyboard accessible')
            activate('[data-action="copy-open"]')
            audit_layout(f'Copy dialog {width}×{height} at {scale}%')
            audit_contrast(f'Copy dialog at {scale}%')
            driver.save_screenshot(str(work / f'copy-{width}-{scale}.png'))
            element('dialog[open] h2').send_keys(Keys.ESCAPE)
            wait.until(lambda _: not driver.find_elements(By.CSS_SELECTOR, 'dialog[open]'))
    if not args.baseline:
        page('library');element('[data-game]').send_keys(Keys.HOME)
        check(js('const title=document.activeElement.querySelector(".game-title").getBoundingClientRect(),bar=document.querySelector(".selection-bar").getBoundingClientRect();return title.top>=0&&title.bottom<=bar.top+1'), 'Focused game title stays visible above compact selection actions')
    driver.save_screenshot(str(work / ('baseline.png' if args.baseline else 'tested.png')))
except Exception:
    driver.save_screenshot(str(work / 'failure.png'))
    raise
finally:
    driver.quit()
    server.shutdown()

report = {'version': version, 'passed': checks, 'failed': findings, 'environment': 'WebKitGTK frontend; mocked native bridge'}
(work / ('baseline.json' if args.baseline else 'results.json')).write_text(json.dumps(report, indent=2) + '\n')
if findings and not args.baseline:
    raise SystemExit(f'{len(findings)} accessibility checks failed')
