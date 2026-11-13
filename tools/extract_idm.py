#!/usr/bin/env python3
"""Extract observed IDM installer records statically; no installer execution."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zlib

SOURCE = None
OUTPUT = Path('/tmp/idm-extracted-64318')
EXPECTED_SHA256 = '7346128fab42957b4254eabdb012bfdbd751e9069361e98a6916f26bb03d62ee'


def safe_path(name):
    parts = PurePosixPath(name.replace('\\', '/'))
    if parts.is_absolute() or not parts.parts or '..' in parts.parts or ':' in name:
        raise ValueError(f'unsafe name: {name!r}')
    return OUTPUT.joinpath(*parts.parts)


def extract():
    data = SOURCE.read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_SHA256:
        raise ValueError('This extractor supports only the supplied 6.43 Build 18 installer hash')
    OUTPUT.mkdir(exist_ok=False)
    offset = 0x17400
    rows = []
    names = []
    for index in range(188):
        declared, packed = struct.unpack_from('<II', data, offset)
        assert 0 < packed <= len(data) - offset - 32
        stream = zlib.decompressobj()
        payload = stream.decompress(data[offset + 32:offset + 32 + packed], 100_000_000)
        payload += stream.flush()
        assert stream.eof and not stream.unused_data and not stream.unconsumed_tail
        # Descriptor size is inconsistent in this supplied installer; all payloads must match.
        assert index == 0 or len(payload) == declared
        if index == 0:
            names = [name.decode('ascii') for _, name in re.findall(rb'<P(\d+)="([^"]+)"', payload)]
            assert len(names) == 187 and len(set(names)) == len(names)
            name = '_installer-descriptor.txt'
        else:
            name = names[index - 1]
        destination = safe_path(name)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open('xb') as target:
            target.write(payload)
        rows.append(dict(index=index, path=name, header_offset=offset, data_offset=offset + 32,
                         declared_size=declared, observed_size=len(payload), packed_size=packed,
                         zlib_checksum_verified=True, sha256=hashlib.sha256(payload).hexdigest()))
        offset += 32 + packed
    result = dict(source=str(SOURCE), source_sha256=hashlib.sha256(data).hexdigest(),
                  format='observed 32-byte headers and zlib streams', record_count=len(rows),
                  end_offset=offset, tail_size=len(data)-offset,
                  limitations=['Descriptor declared size differs from observed size.',
                               'Header timestamp fields not decoded or authenticated.',
                               'Checksums establish stream integrity, not publisher authenticity.'], records=rows)
    (OUTPUT / '_extraction-manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k:v for k,v in result.items() if k != 'records'}, indent=2))


if __name__ == '__main__':
    assert safe_path('Languages\\template.lng') == OUTPUT / 'Languages/template.lng'
    for bad in ['../escape', '/absolute', 'C:\\escape']:
        try:
            safe_path(bad)
        except ValueError:
            pass
        else:
            raise AssertionError(bad)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('installer', type=Path)
    parser.add_argument('output', type=Path, help='Fresh output directory; must not exist')
    args = parser.parse_args()
    SOURCE, OUTPUT = args.installer, args.output
    extract()
