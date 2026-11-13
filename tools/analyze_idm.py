#!/usr/bin/env python3
"""Read PE metadata and bounded feature strings without executing the target."""
import argparse
import hashlib
import json
import re
import struct
import tempfile
from pathlib import Path

PATTERN = re.compile(r"range:|content-range|accept-ranges|if-range|content-length|etag:|last-modified|resume|MSG#|plugin\.v|tonec|IDMMsgHost|IDMNetMon|IDMVMPrs|libssl|libcrypto|WebSocket|\.idm|download.*part|SSL_(?:read|write|connect|CTX_new)|^send$|^recv$|^connect$|ws2_32|wsock32", re.I)


def inspect(path):
    data = path.read_bytes()
    def u16(pos): return struct.unpack_from('<H', data, pos)[0]
    def u32(pos): return struct.unpack_from('<I', data, pos)[0]
    pe = u32(0x3c)
    if data[:2] != b'MZ' or data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Not a PE image')
    machine, sections = u16(pe+4), u16(pe+6)
    opt = pe+24
    plus = u16(opt) == 0x20b
    if u16(opt) not in (0x10b, 0x20b): raise ValueError('Unknown PE optional header')
    base = struct.unpack_from('<Q' if plus else '<I', data, opt+(24 if plus else 28))[0]
    table = opt+u16(pe+20)
    rows = []
    for index in range(sections):
        pos = table+index*40
        name = data[pos:pos+8].split(b'\0')[0].decode('ascii', 'replace')
        virtual_size, rva, size, raw = struct.unpack_from('<IIII', data, pos+8)
        if raw+size > len(data): raise ValueError('Truncated PE section')
        rows.append(dict(name=name,rva=rva,virtual_size=virtual_size,file_offset=raw,file_size=size))
    def offset(rva):
        for row in rows:
            delta=rva-row['rva']
            if 0 <= delta < row['file_size']: return row['file_offset']+delta
        raise ValueError(f'RVA outside file-backed sections: {rva:x}')
    def cstring(pos):
        end = data.find(b'\0',pos,min(len(data),pos+4096))
        if end < 0: raise ValueError('Unterminated string')
        return data[pos:end].decode('ascii','replace')
    def va(pos):
        for row in rows:
            if row['file_offset'] <= pos < row['file_offset']+row['file_size']:
                return hex(base+row['rva']+pos-row['file_offset'])
        return None
    imports=[]
    directory = opt+(112 if plus else 96)
    import_rva, import_size = struct.unpack_from('<II',data,directory+8)
    if import_rva:
        pos=offset(import_rva)
        for _ in range(4096):
            lookup,stamp,forward,name,iat=struct.unpack_from('<IIIII',data,pos)
            if not any((lookup,stamp,forward,name,iat)): break
            dll=cstring(offset(name)); funcs=[]
            thunk=offset(lookup or iat)
            width=8 if plus else 4
            for index in range(65536):
                item=struct.unpack_from('<Q' if plus else '<I',data,thunk+index*width)[0]
                if not item: break
                ordinal=1 << (width*8-1)
                funcs.append({'name':f'ordinal:{item & 65535}' if item & ordinal else cstring(offset(item)+2),'iat_va':hex(base+iat+index*width)})
            else: raise ValueError('Unbounded import thunk table')
            imports.append(dict(dll=dll,functions=funcs)); pos+=20
        else: raise ValueError('Unbounded import descriptor table')
    strings=[]
    for encoding, regex in [('ascii',rb'[\x20-\x7e]{5,}'),('utf-16le',rb'(?:[\x20-\x7e]\x00){5,}')]:
        for match in re.finditer(regex,data):
            text=match.group().decode(encoding)
            if len(text)<=500 and PATTERN.search(text):
                strings.append(dict(file_offset=match.start(),va=va(match.start()),encoding=encoding,text=text))
    return dict(path=path.name,sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),machine=hex(machine),format='PE32+' if plus else 'PE32',image_base=hex(base),entry_va=hex(base+u32(opt+16)),sections=rows,imports=imports,feature_strings=strings)


def self_test():
    data=bytearray(2048)
    data[:2]=b'MZ'
    struct.pack_into('<I',data,0x3c,0x80)
    data[0x80:0x84]=b'PE\0\0'
    struct.pack_into('<HH',data,0x84,0x14c,1)
    struct.pack_into('<H',data,0x94,224)
    opt=0x98
    struct.pack_into('<H',data,opt,0x10b)
    struct.pack_into('<I',data,opt+16,0x1000)
    struct.pack_into('<I',data,opt+28,0x400000)
    struct.pack_into('<II',data,opt+104,0x1100,40)
    section=opt+224
    data[section:section+8]=b'.rdata\0\0'
    struct.pack_into('<IIII',data,section+8,0x600,0x1000,0x600,0x200)
    struct.pack_into('<IIIII',data,0x300,0x1180,0,0,0x1150,0x1190)
    data[0x350:0x35d]=b'KERNEL32.dll\0'
    struct.pack_into('<I',data,0x380,0x11b0)
    struct.pack_into('<I',data,0x390,0x11b0)
    data[0x3b2:0x3bb]=b'ReadFile\0'
    data[0x450:0x45f]=b'Content-Range:\0'
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory)/'fixture.exe'
        path.write_bytes(data)
        result=inspect(path)
    assert result['entry_va']=='0x401000'
    assert result['imports']==[{'dll':'KERNEL32.dll','functions':[{'name':'ReadFile','iat_va':'0x401190'}]}]
    assert result['feature_strings']==[{'file_offset':0x450,'va':'0x401250','encoding':'ascii','text':'Content-Range:'}]
    print('PE parser self-check passed')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test',action='store_true')
    parser.add_argument('directory',type=Path,nargs='?')
    parser.add_argument('output',type=Path,nargs='?')
    args=parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.directory or not args.output:
        parser.error('directory and output are required unless --self-test is used')
    artifacts=[]
    for path in sorted(args.directory.rglob('*')):
        if path.is_file() and path.suffix.lower() in ('.exe','.dll','.sys'):
            artifacts.append(inspect(path))
    assert len({x['path'] for x in artifacts})==len(artifacts)
    result=dict(tool='Python standard library PE parser; no target execution',artifacts=artifacts,limitations=['Import inventory excludes delay-import and dynamically resolved calls.','Feature strings are bounded ASCII/ASCII-range UTF-16 printable runs of 5..500 characters matching the documented regex.','String or import presence does not establish runtime behavior.','Publisher signature authenticity is not verified.'])
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(f"Inspected {len(artifacts)} PE files; wrote {args.output}")


if __name__=='__main__':
    main()
