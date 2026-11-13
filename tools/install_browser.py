#!/usr/bin/env python3
"""Register this app's native messaging host in user browser directories."""
import argparse
import json
import os
from pathlib import Path
import shlex
import tempfile

HOST = 'org.earlyhours.idm_native'
EXTENSION = 'hjjceibkcmhbdlljimdlpbffdllhjhcb'
DESCRIPTION = 'IDM Native browser integration'

def write_atomic(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix='.idm-native-', dir=path.parent)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            os.fchmod(stream.fileno(), mode)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)

def install(home, binary, share):
    binary = binary.resolve(strict=True)
    share = share.resolve(strict=True)
    if not (share/'browser/chromium/manifest.json').is_file():
        raise ValueError('Build or install the browser extension before registration')
    wrapper = home/'.local/share/idm-native/browser-host'
    if wrapper.is_symlink(): raise ValueError('Refusing a symlink at the native host launcher')
    if wrapper.exists() and '# IDM Native managed native-host launcher' not in wrapper.read_text():
        raise ValueError('Refusing to replace an unrelated native host launcher')
    base = dict(name=HOST,description=DESCRIPTION,path=str(wrapper),type='stdio')
    chromium = dict(base,allowed_origins=[f'chrome-extension://{EXTENSION}/'])
    firefox = dict(base,allowed_extensions=['idm-native@earlyhours'])
    destinations = [
        (home/'.config'/name/'NativeMessagingHosts'/f'{HOST}.json',chromium)
        for name in ['chromium','google-chrome','BraveSoftware/Brave-Browser','microsoft-edge']
    ]
    destinations += [(home/name/'native-messaging-hosts'/f'{HOST}.json',firefox)
                     for name in ['.mozilla','.zen','.librewolf']]
    for path, manifest in destinations:
        if path.is_symlink(): raise ValueError(f'Refusing a symlink manifest: {path}')
        if path.exists():
            existing=json.loads(path.read_text())
            if existing.get('name')!=HOST or existing.get('description')!=DESCRIPTION:
                raise ValueError(f'Refusing an unrelated native host manifest: {path}')
    write_atomic(wrapper, '#!/bin/sh\n# IDM Native managed native-host launcher\nexec '+shlex.quote(str(binary))+' browser-host "$@"\n', 0o700)
    for path, manifest in destinations:
        write_atomic(path,json.dumps(manifest,indent=2)+'\n',0o600)
    return dict(ok=True,extensionDirectory=str(share/'browser/chromium'),
                chromiumId=EXTENSION,firefoxId='idm-native@earlyhours',manifests=[str(p) for p,_ in destinations])

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True)
    parser.add_argument('--share',type=Path,required=True)
    parser.add_argument('--home',type=Path,default=Path.home())
    args=parser.parse_args()
    try: print(json.dumps(install(args.home,args.binary,args.share)))
    except (OSError,ValueError) as error:
        parser.exit(1,str(error)+'\n')
