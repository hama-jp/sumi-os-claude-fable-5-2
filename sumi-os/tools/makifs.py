#!/usr/bin/env python3
"""Host-side inspector for MakiFS disk images (the Sumi OS file system). Read-only.

usage: makifs.py IMAGE ls            list every file and directory
       makifs.py IMAGE cat PATH      write a file's contents to stdout
       makifs.py IMAGE fsck          consistency check (chains, cross-links, leaked blocks)
"""
import struct, sys, time

BLK = 4096
BASE = 32768 * 512      # the MakiFS partition starts at sector 32768 (see kernel/fs.c)
EOC, SYS = 0xFFFFFFFF, 0xFFFFFFFE

class FS:
    def __init__(self, path):
        self.f = open(path, 'rb')
        sb = self.block(0)
        self.magic, self.version, self.total, self.fat_start, self.fat_blocks, self.root = struct.unpack_from('<8s5I', sb)
        if self.magic != b'MAKIFS01':
            raise SystemExit('not a MakiFS image (blank disk? Sumi formats it on first boot)')
        self.f.seek(BASE + self.fat_start * BLK)
        self.fat = list(struct.unpack('<%dI' % (self.fat_blocks * BLK // 4), self.f.read(self.fat_blocks * BLK)))
    def block(self, b):
        self.f.seek(BASE + b * BLK); return self.f.read(BLK)
    def chain(self, first):
        out, b = [], first
        while self.root <= b < self.total and len(out) <= self.total:
            out.append(b)
            n = self.fat[b]
            if n == EOC: return out, True
            b = n
        return out, False
    def entries(self, first):
        blocks, ok = self.chain(first)
        for b in blocks:
            d = self.block(b)
            for i in range(BLK // 64):
                name, typ, size, fb, mtime, ctime = struct.unpack_from('<44sB3xIIII', d, i * 64)
                if typ: yield name.split(b'\0')[0].decode('latin-1'), typ, size, fb, mtime
    def walk(self, first=None, prefix=''):
        for name, typ, size, fb, mtime in self.entries(self.root if first is None else first):
            yield prefix + '/' + name, typ, size, fb, mtime
            if typ == 2: yield from self.walk(fb, prefix + '/' + name)
    def read(self, path):
        cur = self.root
        parts = [p for p in path.split('/') if p]
        for k, part in enumerate(parts):
            for name, typ, size, fb, mtime in self.entries(cur):
                if name == part:
                    if k == len(parts) - 1:
                        if typ != 1: raise SystemExit('not a file')
                        blocks, ok = self.chain(fb)
                        return b''.join(self.block(b) for b in blocks)[:size] if size else b''
                    cur = fb; break
            else: raise SystemExit('no such file: ' + path)
        raise SystemExit('not a file')

def fsck(fs):
    errors = 0
    owner = {}
    def claim(blocks, who):
        nonlocal errors
        for b in blocks:
            if b in owner:
                print('ERROR: block %d used by both %s and %s' % (b, owner[b], who)); errors += 1
            owner[b] = who
    blocks, ok = fs.chain(fs.root)
    if not ok: print('ERROR: root directory chain is broken'); errors += 1
    claim(blocks, '/')
    nfiles = ndirs = 0
    for path, typ, size, fb, mtime in fs.walk():
        if typ == 2: ndirs += 1
        else: nfiles += 1
        if typ == 1 and size == 0 and fb == 0: continue
        blocks, ok = fs.chain(fb)
        if not ok: print('ERROR: %s: broken block chain' % path); errors += 1
        if typ == 1 and len(blocks) != (size + BLK - 1) // BLK:
            print('ERROR: %s: size %d needs %d blocks, chain has %d' % (path, size, (size + BLK - 1) // BLK, len(blocks))); errors += 1
        claim(blocks, path)
    used = [b for b in range(fs.root, fs.total) if fs.fat[b] != 0]
    leaked = [b for b in used if b not in owner]
    if leaked: print('ERROR: %d blocks are allocated but belong to no file (e.g. %s)' % (len(leaked), leaked[:8])); errors += 1
    for b in owner:
        if fs.fat[b] == 0: print('ERROR: block %d is in use by %s but marked free' % (b, owner[b])); errors += 1
    for b in range(0, fs.root):
        if fs.fat[b] != SYS: print('ERROR: system block %d not reserved' % b); errors += 1
    print('%d files, %d directories, %d of %d blocks in use, %d errors' % (nfiles, ndirs, len(used), fs.total, errors))
    return errors

def main():
    if len(sys.argv) < 3: raise SystemExit(__doc__)
    fs = FS(sys.argv[1]); cmd = sys.argv[2]
    if cmd == 'ls':
        for path, typ, size, fb, mtime in fs.walk():
            t = time.strftime('%Y-%m-%d %H:%M', time.gmtime(mtime))
            print('%s %9s  %s  %s%s' % ('d' if typ == 2 else '-', '-' if typ == 2 else size, t, path, '/' if typ == 2 else ''))
    elif cmd == 'cat': sys.stdout.buffer.write(fs.read(sys.argv[3]))
    elif cmd == 'fsck': sys.exit(1 if fsck(fs) else 0)
    else: raise SystemExit(__doc__)

if __name__ == '__main__':
    main()
