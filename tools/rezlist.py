"""List (and optionally extract) LithTech 1.0 RezMgr v1 archives.
Usage: python3 -I rezlist.py FILE.REZ [--summary] [--extract OUTDIR]"""
import struct, sys, os, collections

def walk(f, pos, size, prefix, out):
    f.seek(pos); buf = f.read(size); p = 0
    while p < len(buf):
        kind, rpos, rsize, rtime = struct.unpack_from('<IIII', buf, p); p += 16
        if kind == 1:
            e = buf.index(b'\0', p); name = buf[p:e].decode('latin-1'); p = e + 1
            walk(f, rpos, rsize, prefix + name + '/', out)
            f.seek(0)
        else:
            rid = struct.unpack_from('<I', buf, p)[0]
            ext = buf[p+4:p+8].rstrip(b'\0').decode('latin-1')[::-1]
            nkeys = struct.unpack_from('<I', buf, p + 8)[0]; p += 12
            e = buf.index(b'\0', p); name = buf[p:e].decode('latin-1'); p = e + 1
            e = buf.index(b'\0', p); p = e + 1  # description
            p += 4 * nkeys
            out.append((prefix + name + ('.' + ext if ext else ''), rpos, rsize))

def main():
    path = sys.argv[1]
    with open(path, 'rb') as f:
        hdr = f.read(0xA8)
        assert hdr[2:8] == b'RezMgr', 'not a RezMgr file'
        ver, rootpos, rootsize = struct.unpack_from('<III', hdr, 0x7F)
        out = []
        walk(f, rootpos, rootsize, '', out)
        if '--summary' in sys.argv:
            print(f'{path}: {len(out)} resources, version {ver}')
            top = collections.Counter(n.split('/')[0] if '/' in n else '(root)' for n, _, _ in out)
            for k, v in top.most_common(): print(f'  dir {k:20s} {v}')
            exts = collections.Counter(os.path.splitext(n)[1].lower() or '(none)' for n, _, _ in out)
            sizes = collections.Counter()
            for n, _, s in out: sizes[os.path.splitext(n)[1].lower() or '(none)'] += s
            for k, v in exts.most_common(): print(f'  ext {k:8s} {v:6d} files {sizes[k]/1e6:8.1f} MB')
        elif '--extract' in sys.argv:
            root = sys.argv[sys.argv.index('--extract') + 1]
            for n, p, s in out:
                rel = os.path.normpath(n)
                if rel.startswith('..') or os.path.isabs(rel): raise ValueError(n)
                d = os.path.join(root, rel); os.makedirs(os.path.dirname(d), exist_ok=True)
                f.seek(p); open(d, 'wb').write(f.read(s))
            print(len(out), 'files extracted', file=sys.stderr)
        else:
            for n, p, s in out: print(f'{s:10d}  {n}')

if __name__ == '__main__':
    main()
