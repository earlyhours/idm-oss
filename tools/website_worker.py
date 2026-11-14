#!/usr/bin/env python3
"""Bounded, same-origin static website mirroring and conditional synchronization."""
import codecs
from collections import deque
import fcntl
from fnmatch import fnmatchcase
import hashlib
from html import escape
from html.parser import HTMLParser
import json
import mimetypes
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import sys
import tempfile
import zlib
from urllib.error import HTTPError
from urllib.parse import quote, unquote, urldefrag, urljoin, urlsplit, urlunsplit
from urllib.request import build_opener, HTTPRedirectHandler, Request

MANIFEST = '.idm-site-manifest.json'
CACHE = '.idm-site-cache'
CSS_URL = re.compile(r'url\(\s*(?:"([^"\r\n]*)"|\'([^\'\r\n]*)\'|([^\s\)]+))\s*\)', re.I)
CSS_IMPORT = re.compile(r'(@import\s+)(["\'])(.*?)\2', re.I)
HEADER_NAME = re.compile(r"^[!#$%&'*+.^_`|~0-9A-Za-z-]+$")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value, base=''):
    if not isinstance(value, str) or len(value) > 16384 or re.search(r'[\x00-\x1f]', value):
        return None
    try:
        parsed = urlsplit(urljoin(base, value))
        if parsed.scheme.lower() not in ('http', 'https') or not parsed.hostname or parsed.username is not None or parsed.password is not None:
            return None
        host = parsed.hostname.encode('idna').decode('ascii').lower()
        if ':' in host:
            host = '[' + host + ']'
        port = parsed.port
        if port and port != (443 if parsed.scheme.lower() == 'https' else 80):
            host += ':' + str(port)
        return urlunsplit((parsed.scheme.lower(), host,
                           quote(parsed.path or '/', safe="/%:@!$&'()*+,;=-._~"),
                           quote(parsed.query, safe="%/?@:!$&'()*+,;=-._~"), ''))
    except (ValueError, UnicodeError):
        return None


def origin(url):
    value = urlsplit(url)
    return value.scheme, value.hostname, value.port or (443 if value.scheme == 'https' else 80)


def display_url(url):
    value = urlsplit(url)
    return urlunsplit((value.scheme, value.netloc, value.path, '', ''))


def emit(event, **values):
    print(json.dumps(dict(event=event, **values), ensure_ascii=False), flush=True)


def css_unescape(value):
    return re.sub(r'\\([0-9a-fA-F]{1,6}\s?|.)',
                  lambda match: chr(int(match[1].strip(), 16)) if re.fullmatch(r'[0-9a-fA-F]{1,6}\s?', match[1]) else match[1], value)


def css_transform(text, link):
    def url(match):
        value = next(group for group in match.groups() if group is not None)
        replacement = link(css_unescape(value), False)
        return 'url("' + replacement.replace('\\', '\\\\').replace('"', '\\"') + '")'
    text = CSS_URL.sub(url, text)
    return CSS_IMPORT.sub(lambda match: match[1] + match[2] + link(css_unescape(match[3]), False).replace(match[2], '\\' + match[2]) + match[2], text)


def srcset_transform(value, link):
    candidates = []
    position = 0
    while position < len(value):
        while position < len(value) and value[position] in ' \t\r\n,':
            position += 1
        start = position
        while position < len(value) and not value[position].isspace():
            position += 1
        url = value[start:position]
        if not url:
            break
        if url.endswith(','):
            candidates.append(link(url.rstrip(','), False))
            continue
        start = position
        while position < len(value) and value[position] != ',':
            position += 1
        descriptor = value[start:position].strip()
        candidates.append(link(url, False) + (' ' + descriptor if descriptor else ''))
    return ', '.join(candidates)


class BaseFinder(HTMLParser):
    def __init__(self, text, url):
        super().__init__(convert_charrefs=False)
        self.base = url
        self.found = False
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        if tag == 'base' and not self.found:
            value = dict(attrs).get('href')
            if value:
                self.base = canonical(value, self.base) or self.base
                self.found = True


class HTMLCopy(HTMLParser):
    def __init__(self, text, link):
        super().__init__(convert_charrefs=False)
        self.link = link
        self.output = []
        self.in_style = False
        self.feed(text)
        self.close()

    def handle_starttag(self, tag, attrs):
        self.start(tag, attrs, False)

    def handle_startendtag(self, tag, attrs):
        self.start(tag, attrs, True)

    def start(self, tag, attrs, closing):
        if tag == 'base':
            return
        values = dict(attrs)
        rendered = []
        for name, value in attrs:
            if value is not None:
                if name in ('href', 'src', 'poster', 'data'):
                    value = self.link(value, tag in ('a', 'area', 'iframe', 'frame') and name in ('href', 'src'))
                elif name == 'srcset':
                    value = srcset_transform(value, self.link)
                elif name == 'style':
                    value = css_transform(value, self.link)
                elif tag == 'meta' and name == 'charset':
                    value = 'utf-8'
                elif tag == 'meta' and name == 'content' and values.get('http-equiv', '').lower() == 'content-type':
                    value = re.sub(r'charset=[^;\s]+', 'charset=utf-8', value, flags=re.I)
            rendered.append(' ' + name + ('' if value is None else '="' + escape(value, quote=True) + '"'))
        self.output.append('<' + tag + ''.join(rendered) + (' />' if closing else '>'))
        if tag == 'head':
            self.output.append('<meta charset="utf-8">')
        if tag == 'style':
            self.in_style = True

    def handle_endtag(self, tag):
        if tag == 'style':
            self.in_style = False
        if tag != 'base':
            self.output.append('</' + tag + '>')

    def handle_data(self, data):
        self.output.append(css_transform(data, self.link) if self.in_style else data)

    def handle_entityref(self, name): self.output.append('&' + name + ';')
    def handle_charref(self, name): self.output.append('&#' + name + ';')
    def handle_comment(self, data): self.output.append('<!--' + data + '-->')
    def handle_decl(self, decl): self.output.append('<!' + decl + '>')
    def unknown_decl(self, data): self.output.append('<![' + data + ']>')
    def handle_pi(self, data): self.output.append('<?' + data + '>')


def decode_text(data, content_type):
    charset = re.search(r'charset\s*=\s*["\']?([^;\s"\']+)', content_type, re.I)
    if not charset:
        charset = re.search(rb'<meta[^>]+charset\s*=\s*["\']?([^\s"\'>/;]+)', data[:4096], re.I)
    encoding = charset[1] if charset else 'utf-8'
    if isinstance(encoding, bytes):
        encoding = encoding.decode('ascii', 'replace')
    try:
        codecs.lookup(encoding)
    except LookupError:
        encoding = 'utf-8'
    return data.decode(encoding, errors='replace')


def html_type(data, content_type):
    return content_type.split(';', 1)[0].lower() in ('text/html', 'application/xhtml+xml') or bool(re.match(rb'\s*(?:<!doctype\s+html|<html|<head|<body)', data[:512], re.I))


class SameOriginRedirect(HTTPRedirectHandler):
    def __init__(self, selected):
        self.selected = selected

    def redirect_request(self, request, fp, code, message, headers, new_url):
        target = canonical(new_url, request.full_url)
        if not target or origin(target) != self.selected:
            raise ValueError('Redirect outside the selected origin was blocked')
        return super().redirect_request(request, fp, code, message, headers, target)


class QuotaExceeded(ValueError):
    pass


class Mirror:
    def __init__(self, job):
        self.url = canonical(job.get('url'))
        if not self.url:
            raise ValueError('Website URL must be HTTP or HTTPS without embedded credentials')
        self.mode = job.get('mode', 'website')
        if self.mode not in ('website', 'file'):
            raise ValueError('Unsupported synchronization mode')
        self.filename = job.get('filename', '')
        if self.mode == 'file' and (not isinstance(self.filename, str) or not self.filename or
                self.filename in ('.', '..') or self.filename.startswith('.idm-') or
                len(self.filename.encode()) > 180 or re.search(r'[/\\\x00-\x1f\x7f]', self.filename)):
            raise ValueError('File synchronization needs a plain filename without reserved .idm- prefix')
        self.report_transfer = 'transferLimit' in job
        self.transfer_limit = self.number(job, 'transferLimit', 1024 * 1024 * 1024, 0, 1024 * 1024 * 1024 * 1024)
        self.selected = origin(self.url)
        directory = job.get('directory')
        if not isinstance(directory, str) or not Path(directory).is_absolute():
            raise ValueError('Website output directory must be an absolute path')
        self.root = Path(directory).resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.depth = self.number(job, 'depth', 2, 0, 10)
        self.max_files = self.number(job, 'maxFiles', 100, 1, 10000)
        self.max_bytes = self.number(job, 'maxBytes', 50 * 1024 * 1024, 1,
                                     1024 ** 4 if self.mode == 'file' else 1024 ** 3)
        self.include = self.filters(job, 'include')
        self.exclude = self.filters(job, 'exclude')
        self.headers = self.parse_headers(job.get('headers', {}))
        self.timeout = self.number(job, 'timeout', 20, 1, 120)
        self.opener = build_opener(SameOriginRedirect(self.selected))
        self.manifest = {'version': 1, 'url': self.url, 'origin': list(self.selected), 'resources': {}, 'managed': {}}
        self.manifest_name = MANIFEST if self.mode == 'website' else '.idm-file-' + digest(self.filename.encode()) + '.json'
        self.manifest_path = self.path(self.manifest_name)
        self.files = 0
        self.bytes = 0
        self.transferred = 0
        self.limited = False
        self.skipped_external = 0
        self.aliases = {}

    def load_manifest(self):
        if self.manifest_path.exists():
            data = json.loads(self.manifest_path.read_text())
            if data.get('version') != 1 or data.get('url') != self.url or not isinstance(data.get('resources'), dict) or not isinstance(data.get('managed'), dict):
                raise ValueError('Output directory has a different or invalid website manifest')
            for name, hashes in data['managed'].items():
                self.path(name)
                if not isinstance(hashes, list) or not hashes or len(hashes) > 2 or any(not isinstance(h, str) or not re.fullmatch(r'[0-9a-f]{64}', h) for h in hashes):
                    raise ValueError('Website manifest contains invalid managed-file hashes')
            self.manifest = data

    @staticmethod
    def number(job, name, default, low, high):
        value = job.get(name, default)
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f'{name} must be between {low} and {high}')
        return value

    @staticmethod
    def filters(job, name):
        values = job.get(name, [])
        if not isinstance(values, list) or len(values) > 32 or any(
                not isinstance(value, str) or not value or len(value) > 256 or
                re.search(r'[\x00-\x1f\x7f]', value) for value in values):
            raise ValueError(name + ' must contain at most 32 nonempty wildcard patterns of at most 256 characters')
        return values

    def included(self, url):
        path = unquote(urlsplit(url).path).lstrip('/')
        return (not self.include or any(fnmatchcase(path, pattern) for pattern in self.include)) and not any(
            fnmatchcase(path, pattern) for pattern in self.exclude)

    @staticmethod
    def parse_headers(headers):
        if not isinstance(headers, dict) or len(headers) > 32:
            raise ValueError('Invalid website request headers')
        denied = {'host', 'range', 'if-range', 'if-none-match', 'if-modified-since', 'content-length', 'connection', 'transfer-encoding', 'proxy-authorization', 'proxy-connection', 'proxy-authenticate', 'keep-alive', 'te', 'trailer', 'upgrade', 'expect'}
        clean = {'User-Agent': 'IDM-Native-Website/0.2', 'Accept-Encoding': 'identity'}
        for name, value in headers.items():
            if not isinstance(name, str) or not HEADER_NAME.fullmatch(name) or name.lower() in denied or not isinstance(value, str) or len(value) > 65536 or re.search(r'[\x00-\x08\x0a-\x1f\x7f]', value):
                raise ValueError('Unsupported website request header')
            try:
                value.encode('latin-1')
            except UnicodeEncodeError as error:
                raise ValueError('Unsupported website request header encoding') from error
            clean[name] = value
        return clean

    def path(self, relative):
        if not isinstance(relative, str) or not relative or '\\' in relative:
            raise ValueError('Unsafe managed website path')
        parts = PurePosixPath(relative)
        if parts.is_absolute() or '..' in parts.parts or relative == '.':
            raise ValueError('Unsafe managed website path')
        destination = self.root.joinpath(*parts.parts)
        cursor = destination
        while cursor != self.root:
            if cursor.is_symlink():
                raise ValueError('Symlinks are not allowed in managed website paths')
            cursor = cursor.parent
        if not destination.resolve().is_relative_to(self.root):
            raise ValueError('Website output escaped selected directory')
        return destination

    def atomic(self, path, data, owned=None):
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor, temporary = tempfile.mkstemp(prefix='.idm-write-', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            self.publish(temporary, path, owned)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def publish(self, temporary, path, owned):
        self.path(path.relative_to(self.root).as_posix())
        if owned is not None:
            if path.exists():
                if not path.is_file():
                    raise ValueError('Managed synchronization target is not a regular file')
                with path.open('rb') as stream:
                    actual = hashlib.file_digest(stream, 'sha256').hexdigest()
                if actual not in owned:
                    raise ValueError('Refusing to overwrite an unrelated or modified file: ' + path.name)
            else:
                # Hard-link creation cannot clobber a file created while downloading.
                os.link(temporary, path)
                return
        os.replace(temporary, path)

    def save(self):
        self.atomic(self.path(self.manifest_name), (json.dumps(self.manifest, ensure_ascii=False, indent=2) + '\n').encode())

    def managed_write(self, relative, data):
        path = self.path(relative)
        allowed = self.manifest['managed'].get(relative, [])
        previous = None
        if path.exists():
            if not path.is_file():
                raise ValueError('Managed website target is not a regular file')
            if not allowed:
                raise ValueError(f'Refusing to overwrite an unrelated or modified file: {relative}')
            with path.open('rb') as stream:
                previous = hashlib.file_digest(stream, 'sha256').hexdigest()
            if previous not in allowed:
                raise ValueError(f'Refusing to overwrite an unrelated or modified file: {relative}')
        expected = digest(data)
        if previous == expected:
            if allowed != [expected]:
                self.manifest['managed'][relative] = [expected]
                self.save()
            return
        # Persist ownership intent first so interrupted atomic writes can be resumed safely.
        self.manifest['managed'][relative] = list(dict.fromkeys([expected] + ([previous] if previous else [])))
        self.save()
        self.atomic(path, data, [previous] if previous else [])
        self.manifest['managed'][relative] = [expected]
        self.save()

    def cached(self, entry):
        if not entry or not isinstance(entry.get('rawPath'), str):
            return None
        path = self.path(entry['rawPath'])
        if not path.is_file():
            return None
        if path.stat().st_size > self.max_bytes:
            raise ValueError('Cached website source exceeds byte budget')
        data = path.read_bytes()
        actual = digest(data)
        if actual not in self.manifest['managed'].get(entry['rawPath'], []):
            raise ValueError('Cached website source was modified')
        if actual != entry.get('rawSha256'):
            return None
        return data

    def fetch(self, url):
        previous = self.manifest['resources'].get(url)
        cached = self.cached(previous)
        headers = dict(self.headers)
        if previous and cached is not None:
            if previous.get('etag'): headers['If-None-Match'] = previous['etag']
            if previous.get('lastModified'): headers['If-Modified-Since'] = previous['lastModified']
        try:
            response = self.opener.open(Request(url, headers=headers), timeout=self.timeout)
        except HTTPError as error:
            if error.code == 304 and cached is not None:
                if self.bytes + len(cached) > self.max_bytes:
                    raise ValueError('Website byte budget exceeded')
                return cached, previous['contentType'], previous.get('etag', ''), previous.get('lastModified', ''), previous.get('finalUrl', url), 304
            raise ValueError(f'HTTP {error.code} while fetching {display_url(url)}') from error
        with response:
            final_url = canonical(response.url)
            if not final_url or origin(final_url) != self.selected:
                raise ValueError('Response escaped the selected origin')
            limit = self.max_bytes - self.bytes
            length = response.headers.get('Content-Length')
            if length and length.isdigit() and int(length) > limit:
                raise ValueError('Website byte budget exceeded')
            chunks = []
            size = 0
            while True:
                remaining = self.transfer_limit - self.transferred
                if remaining <= 0:
                    if length and length.isdigit() and size == int(length):
                        break
                    raise QuotaExceeded('Download quota exhausted; waiting for the next period')
                chunk = response.read(min(65536, limit - size + 1, remaining))
                if not chunk:
                    break
                size += len(chunk)
                self.transferred += len(chunk)
                if self.report_transfer:
                    emit('transfer', transferredBytes=self.transferred)
                if size > limit:
                    raise ValueError('Website byte budget exceeded')
                chunks.append(chunk)
            data = b''.join(chunks)
            encoding = response.headers.get('Content-Encoding', '').strip().lower()
            if encoding in ('gzip', 'x-gzip', 'deflate'):
                modes = [16 + zlib.MAX_WBITS] if encoding != 'deflate' else [zlib.MAX_WBITS, -zlib.MAX_WBITS]
                decoded = None
                for mode in modes:
                    decoder = zlib.decompressobj(mode)
                    try:
                        decoded = decoder.decompress(data, limit + 1)
                        break
                    except zlib.error:
                        continue
                if decoded is None:
                    raise ValueError('Invalid compressed website resource')
                if len(decoded) > limit or decoder.unconsumed_tail:
                    raise ValueError('Website byte budget exceeded after decompression')
                if not decoder.eof or decoder.unused_data:
                    raise ValueError('Incomplete or unsupported compressed website resource')
                data = decoded
            elif encoding and encoding != 'identity':
                raise ValueError('Server ignored identity encoding; unsupported website compression: ' + encoding)
            etag = response.headers.get('ETag', '')
            modified = response.headers.get('Last-Modified', '')
            return data, response.headers.get('Content-Type', ''), etag if len(etag) <= 2048 and not re.search(r'[\r\n\x00]', etag) else '', modified if len(modified) <= 2048 and not re.search(r'[\r\n\x00]', modified) else '', final_url, response.status

    def output_path(self, url, data, content_type):
        if url == self.url:
            return 'index.html'
        if html_type(data, content_type):
            suffix = '.html'
        else:
            mime = content_type.split(';', 1)[0].strip().lower()
            suffix = '.css' if mime == 'text/css' else mimetypes.guess_extension(mime) or Path(urlsplit(url).path).suffix
            if not re.fullmatch(r'\.[a-zA-Z0-9]{1,10}', suffix or ''):
                suffix = '.bin'
        name = unquote(PurePosixPath(urlsplit(url).path).name).rsplit('.', 1)[0]
        name = re.sub(r'[^a-zA-Z0-9_-]', '_', name)[:48] or 'resource'
        return 'files/' + name + '-' + digest(url.encode()) + suffix

    def run(self):
        lock = os.open(self.path('.idm-site.lock'), os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        try:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ValueError('Another website job is already using this directory') from error
            self.load_manifest()
            return self.sync_file() if self.mode == 'file' else self.crawl()
        finally:
            os.close(lock)

    def file_hash(self):
        path = self.path(self.filename)
        if not path.exists():
            return None
        if not path.is_file():
            raise ValueError('Managed synchronization target is not a regular file')
        with path.open('rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual not in self.manifest['managed'].get(self.filename, []):
            raise ValueError('Refusing to overwrite an unrelated or modified file: ' + self.filename)
        return actual

    @staticmethod
    def strong_etag(value):
        return isinstance(value, str) and len(value) <= 2048 and bool(re.fullmatch(r'"[\x21\x23-\x7e\x80-\xff]*"', value))

    def restart_file(self, path, reason):
        self.manifest.pop('partial', None)
        path.unlink(missing_ok=True)
        self.save()
        raise ValueError(reason + '; partial discarded, next run will restart')

    def sync_file(self):
        previous = self.manifest['resources'].get(self.url)
        if previous and previous.get('path') != self.filename:
            raise ValueError('File synchronization manifest has a different output')
        actual = self.file_hash()
        if actual is not None and self.path(self.filename).stat().st_size > self.max_bytes:
            raise ValueError('Cached website source exceeds byte budget')
        partial_path = self.path(self.manifest_name.removesuffix('.json') + '.part')
        partial = self.manifest.get('partial')
        headers = dict(self.headers)
        headers_hash = digest(json.dumps(headers, sort_keys=True).encode())
        # Byte ranges refer to the identity representation, never a decoded body.
        headers = {name: value for name, value in headers.items() if name.lower() != 'accept-encoding'}
        headers['Accept-Encoding'] = 'identity'
        offset = 0
        hasher = hashlib.sha256()
        if partial is not None:
            if (not isinstance(partial, dict) or
                    type(partial.get('size')) is not int or type(partial.get('total')) is not int or
                    not 0 <= partial['size'] <= partial['total'] <= self.max_bytes or
                    not self.strong_etag(partial.get('etag')) or
                    partial.get('headersSha256') != headers_hash or
                    not isinstance(partial.get('sha256'), str) or
                    not re.fullmatch(r'[0-9a-f]{64}', partial['sha256']) or
                    canonical(partial.get('finalUrl')) != partial.get('finalUrl') or
                    not partial.get('finalUrl') or origin(partial['finalUrl']) != self.selected or
                    any(not isinstance(partial.get(key), str) for key in ('contentType', 'lastModified'))):
                self.restart_file(partial_path, 'Cannot safely resume invalid or changed file identity')
            if not partial_path.is_file():
                self.restart_file(partial_path, 'Cannot safely resume a missing partial file')
            info = partial_path.stat()
            if info.st_uid != os.getuid() or info.st_nlink != 1 or stat.S_IMODE(info.st_mode) & 0o077:
                raise ValueError('Synchronization partial must be a private, owned regular file')
            if info.st_size != partial['size']:
                self.restart_file(partial_path, 'Cannot safely resume a modified partial file')
            with partial_path.open('rb') as stream:
                hasher = hashlib.file_digest(stream, 'sha256')
            if hasher.hexdigest() != partial['sha256']:
                self.restart_file(partial_path, 'Cannot safely resume a modified partial file')
            offset = partial['size']
        elif partial_path.exists():
            raise ValueError('Untracked synchronization partial; remove it before restarting')
        if partial and offset == partial['total']:
            return self.finish_file(partial_path, partial, previous, 206)
        if partial:
            if not self.transfer_limit:
                raise QuotaExceeded('Download quota exhausted; waiting for the next period')
            end = min(partial['total'] - 1, offset + self.transfer_limit - 1)
            headers['Range'] = f'bytes={offset}-{end}'
            headers['If-Range'] = partial['etag']
        elif previous and actual is not None and actual == previous.get('rawSha256'):
            if previous.get('etag'): headers['If-None-Match'] = previous['etag']
            if previous.get('lastModified'): headers['If-Modified-Since'] = previous['lastModified']
        try:
            response = self.opener.open(Request(self.url, headers=headers), timeout=self.timeout)
        except HTTPError as error:
            if error.code == 304 and not partial and previous and actual is not None and actual == previous.get('rawSha256'):
                if self.file_hash() != actual:
                    raise ValueError('Managed synchronization target changed during conditional request')
                emit('complete', ok=True, files=1, bytes=self.path(self.filename).stat().st_size,
                     transferredBytes=0, status=304, unchanged=True, entry=str(self.path(self.filename)))
                return 0
            if partial and error.code in (304, 412, 416):
                self.restart_file(partial_path, 'Server rejected conditional range resume')
            raise ValueError(f'HTTP {error.code} while fetching {display_url(self.url)}') from error
        with response:
            final_url = canonical(response.url)
            if not final_url or origin(final_url) != self.selected:
                raise ValueError('Response escaped the selected origin')
            lengths = response.headers.get_all('Content-Length', [])
            if len(lengths) > 1 or (lengths and not re.fullmatch(r'[0-9]+', lengths[0])):
                self.restart_file(partial_path, 'Invalid Content-Length for file synchronization')
            length = int(lengths[0]) if lengths else None
            etags = response.headers.get_all('ETag', [])
            etag = etags[0] if len(etags) == 1 else ''
            modified = response.headers.get('Last-Modified', '')
            etag, modified = [value if len(value) <= 2048 and not re.search(r'[\r\n\x00]', value) else ''
                              for value in (etag, modified)]
            encodings = response.headers.get_all('Content-Encoding', [])
            encoding = encodings[0].strip().lower() if encodings else ''
            if len(encodings) > 1 or encoding not in ('', 'identity'):
                self.restart_file(partial_path, 'Server ignored identity encoding; cannot safely resume encoded files')
            if partial:
                ranges = response.headers.get_all('Content-Range', [])
                match = re.fullmatch(r'bytes ([0-9]+)-([0-9]+)/([0-9]+)', ranges[0]) if len(ranges) == 1 else None
                if response.status != 206:
                    self.restart_file(partial_path, 'Server ignored Range or the resource changed; cannot safely resume')
                if etag != partial['etag'] or final_url != partial['finalUrl']:
                    self.restart_file(partial_path, 'Remote resource identity changed during resume')
                if (not match or tuple(map(int, match.groups())) != (offset, end, partial['total']) or
                        (length is not None and length != end - offset + 1)):
                    self.restart_file(partial_path, 'Invalid Content-Range for conditional file resume')
                expected = end - offset + 1
                total = partial['total']
            else:
                if response.status != 200 or response.headers.get('Content-Range'):
                    raise ValueError('Expected a complete HTTP 200 file response')
                expected = total = length
            if total is not None and total > self.max_bytes:
                raise ValueError('Website byte budget exceeded')
            if not self.transfer_limit:
                raise QuotaExceeded('Download quota exhausted; waiting for the next period')
            state = dict(size=offset, total=total, sha256=hasher.hexdigest(), etag=etag,
                         headersSha256=headers_hash, finalUrl=final_url,
                         contentType=response.headers.get('Content-Type', ''),
                         lastModified=modified)
            resumable = total is not None and self.strong_etag(etag)
            if resumable and not partial:
                # An interrupted first write must leave metadata that rejects unverified bytes.
                self.manifest['partial'] = state
                self.save()
            flags = os.O_WRONLY | os.O_NOFOLLOW | (os.O_APPEND if partial else os.O_CREAT | os.O_EXCL)
            descriptor = os.open(partial_path, flags, 0o600)
            received = 0
            complete = False
            try:
                with os.fdopen(descriptor, 'ab' if partial else 'wb') as stream:
                    try:
                        while expected is None or received < expected:
                            remaining = self.transfer_limit - self.transferred
                            if remaining <= 0:
                                if not resumable:
                                    raise ValueError('Cannot safely resume without a strong ETag and Content-Length; restart with a larger quota')
                                raise QuotaExceeded('Download quota exhausted; partial saved for the next period')
                            chunk = response.read(min(65536, remaining, self.max_bytes - offset - received + 1,
                                                      expected - received if expected is not None else 65536))
                            if not chunk:
                                if expected is not None:
                                    raise ValueError('Incomplete file response; no truncated file was published')
                                total = offset + received
                                break
                            self.transferred += len(chunk)
                            if self.report_transfer:
                                emit('transfer', transferredBytes=self.transferred)
                            if offset + received + len(chunk) > self.max_bytes:
                                raise ValueError('Website byte budget exceeded')
                            stream.write(chunk)
                            hasher.update(chunk)
                            received += len(chunk)
                        complete = offset + received == total
                    finally:
                        stream.flush()
                        os.fsync(stream.fileno())
                        state.update(size=offset + received, total=total, sha256=hasher.hexdigest())
                        if resumable:
                            self.manifest['partial'] = state
                            self.save()
            finally:
                if not resumable and not complete:
                    partial_path.unlink(missing_ok=True)
            if not complete:
                raise QuotaExceeded('Download quota exhausted; partial saved for the next period')
            try:
                return self.finish_file(partial_path, state, previous, response.status)
            finally:
                if not resumable:
                    partial_path.unlink(missing_ok=True)

    def finish_file(self, partial_path, state, previous, status):
        actual = self.file_hash()
        expected = state['sha256']
        self.path(partial_path.relative_to(self.root).as_posix())
        if state['size'] != state['total'] or partial_path.stat().st_size != state['total']:
            self.restart_file(partial_path, 'Incomplete synchronization partial cannot be published')
        with partial_path.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != expected:
                self.restart_file(partial_path, 'Modified synchronization partial cannot be published')
        # Persist ownership intent before the atomic publication, as for website writes.
        self.manifest['managed'][self.filename] = list(dict.fromkeys([expected] + ([actual] if actual else [])))
        self.save()
        if actual != expected:
            self.publish(partial_path, self.path(self.filename), [actual] if actual else [])
        partial_path.unlink(missing_ok=True)
        self.manifest['managed'][self.filename] = [expected]
        self.manifest.pop('partial', None)
        self.manifest['resources'][self.url] = {
            'path': self.filename, 'rawPath': self.filename, 'rawSha256': expected,
            'contentType': state['contentType'], 'etag': state['etag'],
            'lastModified': state['lastModified'], 'finalUrl': state['finalUrl']}
        self.save()
        emit('complete', ok=True, files=1, bytes=state['size'], transferredBytes=self.transferred,
             status=status, unchanged=bool(previous and previous.get('rawSha256') == expected),
             entry=str(self.path(self.filename)))
        return 0

    def crawl(self):
        queue = deque([(self.url, 0, True)])
        discovered = {self.url}
        current = {}
        while queue:
            if self.files >= self.max_files:
                self.limited = True
                break
            url, depth, follow_html = queue.popleft()
            data, content_type, etag, modified, final_url, status = self.fetch(url)
            if url == self.url and not html_type(data, content_type):
                raise ValueError('Starting URL did not return an HTML document')
            if final_url != url:
                self.aliases[final_url] = url
            relative = self.output_path(url, data, content_type)
            raw_path = CACHE + '/' + digest(url.encode()) + '.body'
            entry = {'path': relative, 'rawPath': raw_path, 'rawSha256': digest(data), 'contentType': content_type, 'etag': etag, 'lastModified': modified, 'finalUrl': final_url}
            self.manifest['resources'][url] = entry
            self.save()
            self.managed_write(raw_path, data)
            current[url] = (entry, data)
            self.files += 1
            self.bytes += len(data)
            emit('progress', url=display_url(url), path=relative, files=self.files, bytes=self.bytes, transferredBytes=self.transferred, status=status)
            def discover(value, page):
                target = canonical(value, base)
                if not target:
                    return value
                if origin(target) != self.selected:
                    self.skipped_external += 1
                    return target
                target = self.aliases.get(target, target)
                # Discovery pages and stylesheets are needed even for an images-only grab.
                if not page and not self.included(target) and not urlsplit(target).path.lower().endswith('.css'):
                    return target
                if target not in discovered:
                    if page and depth >= self.depth:
                        self.limited = True
                    elif len(discovered) >= self.max_files * 4:
                        self.limited = True
                    else:
                        discovered.add(target)
                        queue.append((target, depth + 1 if page else depth, page))
                return target
            if html_type(data, content_type) and follow_html:
                text = decode_text(data, content_type)
                base = BaseFinder(text, final_url).base
                HTMLCopy(text, discover)
            elif content_type.split(';', 1)[0].lower() == 'text/css' or urlsplit(url).path.lower().endswith('.css'):
                base = final_url
                css_transform(decode_text(data, content_type), discover)
        for url, (entry, data) in current.items():
            if not self.included(url):
                continue
            content_type = entry['contentType']
            final_url = entry['finalUrl']
            def rewrite(value, _page):
                target = canonical(value, base)
                if not target:
                    return value
                target = self.aliases.get(target, target)
                resource = self.manifest['resources'].get(target)
                fragment = urldefrag(urljoin(base, value))[1]
                if self.included(target) and ((resource and resource['path'] in self.manifest['managed']) or target in current):
                    local = current[target][0]['path'] if target in current else resource['path']
                    return posixpath.relpath(local, posixpath.dirname(entry['path']) or '.') + ('#' + fragment if fragment else '')
                return target + ('#' + fragment if fragment else '')
            if html_type(data, content_type):
                text = decode_text(data, content_type)
                base = BaseFinder(text, final_url).base
                output = ''.join(HTMLCopy(text, rewrite).output)
                if not re.search(r'<head(?:\s|>)', output, re.I):
                    output = '<meta charset="utf-8">\n' + output
                data = output.encode()
            elif content_type.split(';', 1)[0].lower() == 'text/css' or urlsplit(url).path.lower().endswith('.css'):
                base = final_url
                data = css_transform(decode_text(data, content_type), rewrite).encode()
            if len(data) > self.max_bytes:
                raise ValueError('Rewritten website resource exceeds byte budget')
            self.managed_write(entry['path'], data)
        self.save()
        emit('complete', ok=True, files=self.files, bytes=self.bytes, transferredBytes=self.transferred,
             limited=self.limited, skippedExternal=self.skipped_external, manifest=str(self.manifest_path), entry=str(self.root / 'index.html') if self.included(self.url) else '')
        return 0


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    try:
        if len(args) != 1:
            raise ValueError('Usage: website_worker.py JOB.json')
        job_path = Path(args[0])
        job = json.loads(job_path.read_text())
        if not isinstance(job, dict):
            raise ValueError('Website job must be a JSON object')
        if job.get('headers') and (stat.S_IMODE(job_path.stat().st_mode) & 0o077):
            raise ValueError('Website job files containing headers must be private to their owner')
        return Mirror(job).run()
    except QuotaExceeded as error:
        emit('quota', ok=False, error=str(error))
        return 2
    except Exception as error:
        emit('error', ok=False, error=str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
