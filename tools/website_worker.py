#!/usr/bin/env python3
"""Bounded, same-origin static website mirroring and conditional synchronization."""
import codecs
from collections import deque
import fcntl
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


class Mirror:
    def __init__(self, job):
        self.url = canonical(job.get('url'))
        if not self.url:
            raise ValueError('Website URL must be HTTP or HTTPS without embedded credentials')
        self.selected = origin(self.url)
        directory = job.get('directory')
        if not isinstance(directory, str) or not Path(directory).is_absolute():
            raise ValueError('Website output directory must be an absolute path')
        self.root = Path(directory).resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.depth = self.number(job, 'depth', 2, 0, 10)
        self.max_files = self.number(job, 'maxFiles', 100, 1, 10000)
        self.max_bytes = self.number(job, 'maxBytes', 50 * 1024 * 1024, 1, 1024 * 1024 * 1024)
        self.headers = self.parse_headers(job.get('headers', {}))
        self.timeout = self.number(job, 'timeout', 20, 1, 120)
        self.opener = build_opener(SameOriginRedirect(self.selected))
        self.manifest = {'version': 1, 'url': self.url, 'origin': list(self.selected), 'resources': {}, 'managed': {}}
        self.manifest_path = self.path(MANIFEST)
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

    def atomic(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        descriptor, temporary = tempfile.mkstemp(prefix='.idm-write-', dir=path.parent)
        try:
            with os.fdopen(descriptor, 'wb') as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)

    def save(self):
        self.atomic(self.path(MANIFEST), (json.dumps(self.manifest, ensure_ascii=False, indent=2) + '\n').encode())

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
        self.atomic(path, data)
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
                chunk = response.read(min(65536, limit - size + 1))
                if not chunk:
                    break
                size += len(chunk)
                if size > limit:
                    raise ValueError('Website byte budget exceeded')
                chunks.append(chunk)
            data = b''.join(chunks)
            self.transferred += len(data)
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
            return self.crawl()
        finally:
            os.close(lock)

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
            content_type = entry['contentType']
            final_url = entry['finalUrl']
            def rewrite(value, _page):
                target = canonical(value, base)
                if not target:
                    return value
                target = self.aliases.get(target, target)
                resource = self.manifest['resources'].get(target)
                fragment = urldefrag(urljoin(base, value))[1]
                if (resource and resource['path'] in self.manifest['managed']) or target in current:
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
             limited=self.limited, skippedExternal=self.skipped_external, manifest=str(self.manifest_path), entry=str(self.root / 'index.html'))
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
    except Exception as error:
        emit('error', ok=False, error=str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
