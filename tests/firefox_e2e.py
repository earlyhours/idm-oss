"""Verify the shipped Firefox add-on in isolated headless Zen via Marionette.

Protocol: https://firefox-source-docs.mozilla.org/remote/marionette/Protocol.html
No browser profile, native host registration, or configuration outside temp is used.
"""
import base64
import hashlib
import http.server
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BROWSER = Path('/opt/zen-browser-bin/zen-bin')
ADDON = 'idm-native@earlyhours'
UUID = '8bd559cc-d8a4-4cfb-8c10-153f3529eafe'


def wait(check, label, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError('Timed out: ' + label)


class Marionette:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=15)
        self.stream = self.socket.makefile('rb')
        self.next = 0
        self.hello = self.receive()
        assert self.hello['marionetteProtocol'] == 3, self.hello

    def receive(self):
        prefix = b''
        while True:
            byte = self.stream.read(1)
            if not byte:
                raise EOFError('Marionette disconnected')
            if byte == b':':
                break
            prefix += byte
        return json.loads(self.stream.read(int(prefix)))

    def command(self, name, **parameters):
        self.next += 1
        data = json.dumps([0, self.next, name, parameters]).encode()
        self.socket.sendall(str(len(data)).encode() + b':' + data)
        response = self.receive()
        assert response[:2] == [1, self.next], response
        if response[2]:
            raise RuntimeError(name + ': ' + json.dumps(response[2]))
        return response[3]

    def execute(self, script, asynchronous=False):
        result = self.command('WebDriver:ExecuteAsyncScript' if asynchronous else 'WebDriver:ExecuteScript',
                              script=script, args=[], newSandbox=True, sandbox=None, scriptTimeout=15000)
        return result.get('value', result)

    def close(self):
        self.stream.close()
        self.socket.close()


def main():
    assert BROWSER.is_file(), 'Zen browser executable unavailable'
    with tempfile.TemporaryDirectory(prefix='idm-gecko-e2e-') as temporary:
        temporary = Path(temporary)
        home, profile, state, output, fixtures = [temporary / name for name in ('home', 'profile', 'state', 'downloads', 'fixtures')]
        for directory in (home, profile, state, output, fixtures):
            directory.mkdir()
        payload = bytes(range(256)) * 4096
        (fixtures / 'fixture.bin').write_bytes(payload)
        subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc=size=160x90:rate=12', '-t', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', str(fixtures / 'clip.mp4')], check=True)
        (fixtures / 'index.html').write_text('<!doctype html><title>IDM Gecko fixture</title><video src="clip.mp4" controls></video><a href="fixture.bin">Download fixture</a>')
        class Handler(http.server.SimpleHTTPRequestHandler):
            def __init__(self, *args, **kwargs):
                super().__init__(*args, directory=str(fixtures), **kwargs)
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        origin = f'http://127.0.0.1:{server.server_port}'
        (state / 'automation.json').write_text(json.dumps({'version':1,'preferences':{'directory':str(output),'notifications':False,'sound':False},'queues':{},'policies':{}}))
        binary = Path(os.environ.get('IDM_TEST_BINARY', str(ROOT / 'build/idm-native'))).resolve(strict=True)
        wrapper = temporary / 'native-host'
        wrapper.write_text('#!/bin/sh\nexport IDM_NATIVE_STATE_DIRECTORY=' + shlex.quote(str(state)) + '\nexport IDM_NATIVE_DOWNLOAD_DIRECTORY=' + shlex.quote(str(output)) + '\nexec ' + shlex.quote(str(binary)) + ' browser-host "$@"\n')
        wrapper.chmod(0o700)
        manifest = {'name':'org.earlyhours.idm_native','description':'IDM isolated Gecko test','path':str(wrapper),'type':'stdio','allowed_extensions':[ADDON]}
        for name in ('.mozilla', '.zen', '.librewolf'):
            directory = home / name / 'native-messaging-hosts'
            directory.mkdir(parents=True)
            (directory / 'org.earlyhours.idm_native.json').write_text(json.dumps(manifest))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        preferences = {'marionette.port':port,'marionette.enabled':True,'browser.shell.checkDefaultBrowser':False,'browser.startup.homepage':'about:blank','browser.startup.page':0,'zen.welcome-screen.seen':True,'zen.welcome-screen.enabled':False,'extensions.webextensions.uuids':json.dumps({ADDON:UUID})}
        (profile / 'user.js').write_text(''.join(f'user_pref({json.dumps(key)}, {json.dumps(value)});\n' for key,value in preferences.items()))
        environment = {**os.environ,'HOME':str(home),'XDG_CONFIG_HOME':str(home / '.config'),'MOZ_HEADLESS':'1','MOZ_REMOTE_ALLOW_SYSTEM_ACCESS':'1','MOZ_NO_REMOTE':'1','QT_QPA_PLATFORM':'offscreen','QT_QUICK_BACKEND':'software','IDM_NATIVE_STATE_DIRECTORY':str(state),'IDM_NATIVE_DOWNLOAD_DIRECTORY':str(output)}
        browser = app = client = None
        logs = temporary / 'processes.log'
        try:
            with logs.open('wb') as log:
                app = subprocess.Popen([str(binary),'open','--state-directory',str(state)],env=environment,stdout=log,stderr=log)
                wait(lambda:(state / 'browser.sock').exists(), 'native app socket')
                browser = subprocess.Popen([str(BROWSER),'--headless','--no-remote','--profile',str(profile),'--marionette','about:blank'],env=environment,stdout=log,stderr=log)
                def connect():
                    try:
                        return Marionette(port)
                    except OSError:
                        if browser.poll() is not None:
                            raise AssertionError('Headless Zen exited: ' + logs.read_text(errors='replace')[-1500:])
                client = wait(connect,'Marionette listener')
                session = client.command('WebDriver:NewSession', capabilities={})
                capabilities = session['capabilities']
                assert capabilities['moz:headless'] is True
                assert Path(capabilities['moz:profile']).resolve() == profile.resolve()
                addon = client.command('Addon:Install', path=str(ROOT / 'build/browser/firefox'), temporary=True)
                assert addon['value'] == ADDON, addon
                # Zen's initial placeholder is not exposed as an extension tab.
                handle = client.command('WebDriver:NewWindow', type='tab')
                client.command('WebDriver:SwitchToWindow', handle=handle['handle'])
                client.command('WebDriver:Navigate', url=origin + '/')
                wait(lambda:client.execute("return Boolean(document.getElementById('idm-native-capture-root')?.shadowRoot)"),'Gecko content panel')
                client.execute("document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.capture-toggle').click(); return true")
                assert client.execute("return !document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.panel').hidden")
                screenshot = client.command('WebDriver:TakeScreenshot', full=False, scroll=False)
                screenshot_path = Path(os.environ.get('IDM_UI_SCREENSHOT_DIR', str(ROOT / 'build/screenshots/iteration-3'))) / 'extension-gecko-panel.png'
                screenshot_path.parent.mkdir(parents=True,exist_ok=True)
                screenshot_path.write_bytes(base64.b64decode(screenshot['value']))
                client.execute("document.getElementById('idm-native-capture-root').shadowRoot.querySelector('.download').click(); return true")
                def job(url):
                    try:
                        rows = json.loads((state / 'queue.json').read_text())['downloads']
                    except FileNotFoundError:
                        return None
                    return next((item for item in rows if item['url'] == url), None)
                media = wait(lambda:job(origin + '/clip.mp4'),'Gecko native media acceptance')
                assert media['media'], media
                def complete(url):
                    record = job(url)
                    if record and record['status'] == 'error':
                        raise AssertionError(record['error'])
                    return record if record and record['status'] == 'complete' else None
                media = wait(lambda:complete(origin + '/clip.mp4'), 'Gecko native media completion', seconds=40)
                def frames(filename):
                    result = subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-i',str(filename),'-map','0:v:0','-f','framemd5','-'],check=True,capture_output=True,text=True,timeout=15)
                    return [line.split(',')[-1].strip() for line in result.stdout.splitlines() if line and not line.startswith('#')]
                decoded = frames(fixtures / 'clip.mp4')
                assert decoded and frames(media['path']) == decoded
                client.command('WebDriver:Navigate', url=f'moz-extension://{UUID}/popup.html')
                wait(lambda:client.execute("return document.getElementById('connection-card')?.dataset.state === 'connected'"),'Gecko popup connection')
                response = client.execute("const done=arguments[arguments.length-1]; browser.runtime.sendMessage({type:'batch',tabId:0,urls:[" + json.dumps(origin + '/fixture.bin') + "]}).then(done,e=>done({error:e.message}));", asynchronous=True)
                assert response['ok'], response
                direct = wait(lambda:complete(origin + '/fixture.bin'), 'Gecko exact-byte native file completion')
                assert Path(direct['path']).read_bytes() == payload
                report = {'ok':True,'nativeBinary':str(binary),'browser':str(BROWSER),'version':session.get('capabilities',{}).get('browserVersion'),'addon':ADDON,'checks':['Firefox manifest temporary installation','content panel injection and opening','media download through real native app with exact decoded video frames','popup native connection','batch file download byte equality'],'fileSha256':hashlib.sha256(payload).hexdigest(),'profileIsolated':True,'headless':True,'decodedFrames':len(decoded),'screenshot':str(screenshot_path.relative_to(ROOT))}
                destination = ROOT / 'build/verification/firefox-e2e.json'
                destination.parent.mkdir(parents=True,exist_ok=True)
                destination.write_text(json.dumps(report,indent=2) + '\n')
                print(json.dumps(report))
        except Exception:
            print(logs.read_text(errors='replace')[-2500:])
            raise
        finally:
            if client:
                client.close()
            for process in (browser,app):
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait(timeout=3)
            server.shutdown(); server.server_close()


if __name__ == '__main__':
    main()
