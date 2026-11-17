"""Extract InstallShield 3 .Z archives (data.z, _setup.lib) without running the installer.
Usage: python3 -I is3extract.py ARCHIVE OUTDIR [--list]
Decompression is a Python port of zlib's contrib/blast (PKWARE DCL explode)."""
import struct, sys, os

# ---------------- PKWARE DCL explode (port of blast.c) ----------------
class _Huff:
    def __init__(self, rep):
        lengths = []
        for b in rep:
            n = (b >> 4) + 1
            lengths += [b & 15] * n
        self.count = [0] * 14
        for l in lengths:
            self.count[l] += 1
        offs = [0] * 14
        for l in range(1, 13):
            offs[l + 1] = offs[l] + self.count[l]
        self.symbol = [0] * len(lengths)
        for sym, l in enumerate(lengths):
            if l:
                self.symbol[offs[l]] = sym
                offs[l] += 1

_LIT = _Huff(bytes([11,124,8,7,28,7,188,13,76,4,10,8,12,10,12,10,8,23,8,9,7,6,7,8,7,6,55,8,23,24,12,11,7,9,11,12,6,7,22,5,7,24,6,11,9,6,7,22,7,11,38,7,9,8,25,11,8,11,9,12,8,12,5,38,5,38,5,11,7,5,6,21,6,10,53,8,7,24,10,27,44,253,253,253,252,252,252,13,12,45,12,45,12,61,12,45,44,173]))
_LEN = _Huff(bytes([2,35,36,53,38,23]))
_DIST = _Huff(bytes([2,20,53,230,247,151,248]))
_BASE = [3,2,4,5,6,7,8,9,10,12,16,24,40,72,136,264]
_EXTRA = [0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8]

def explode(src):
    pos = 0; bitbuf = 0; bitcnt = 0
    def bits(need):
        nonlocal pos, bitbuf, bitcnt
        val = bitbuf
        while bitcnt < need:
            if pos >= len(src):
                raise EOFError
            val |= src[pos] << bitcnt; pos += 1; bitcnt += 8
        bitbuf = val >> need; bitcnt -= need
        return val & ((1 << need) - 1)
    def decode(h):
        code = first = index = 0
        for l in range(1, 14):
            code |= bits(1) ^ 1
            c = h.count[l]
            if code < first + c:
                return h.symbol[index + (code - first)]
            index += c; first += c; first <<= 1; code <<= 1
        raise ValueError("bad code")
    lit = bits(8); dictbits = bits(8)
    if lit > 1 or not 4 <= dictbits <= 6:
        raise ValueError("not DCL data")
    out = bytearray()
    while True:
        if bits(1):
            sym = decode(_LEN)
            ln = _BASE[sym] + bits(_EXTRA[sym])
            if ln == 519:
                break
            sym = 2 if ln == 2 else dictbits
            dist = (decode(_DIST) << sym) + bits(sym) + 1
            if dist > len(out):
                raise ValueError("distance too far")
            for _ in range(ln):
                out.append(out[-dist])
        else:
            out.append(decode(_LIT) if lit else bits(8))
    return bytes(out)

# ---------------- IS3 archive layout ----------------
def parse(data):
    if struct.unpack_from('<I', data, 0)[0] != 0x8C655D13:
        raise ValueError('not an InstallShield 3 archive')
    file_count = struct.unpack_from('<H', data, 12)[0]
    toc = struct.unpack_from('<I', data, 41)[0]
    dir_count = struct.unpack_from('<H', data, 49)[0]
    p = toc; dirs = []
    for _ in range(dir_count):
        fc, chunk, nlen = struct.unpack_from('<HHH', data, p)
        dirs.append((data[p+6:p+6+nlen].decode('latin-1'), fc))
        p += chunk
    files = []
    for dname, fc in dirs:
        for _ in range(fc):
            usize, csize, off = struct.unpack_from('<III', data, p + 3)
            chunk = struct.unpack_from('<H', data, p + 23)[0]
            nlen = data[p + 29]
            name = data[p+30:p+30+nlen].decode('latin-1')
            files.append((dname, name, usize, csize, off))
            p += chunk
    assert len(files) == file_count, (len(files), file_count)
    return files

def safe_join(root, *parts):
    rel = os.path.normpath(os.path.join(*[x.replace('\\', '/') for x in parts if x]))
    if rel.startswith('..') or os.path.isabs(rel):
        raise ValueError('unsafe path: ' + rel)
    return os.path.join(root, rel)

def main():
    arc, outdir = sys.argv[1], sys.argv[2]
    data = open(arc, 'rb').read()
    files = parse(data)
    for d, n, us, cs, off in files:
        if '--list' in sys.argv:
            print(f'{us:9d}  {d}\\{n}' if d else f'{us:9d}  {n}')
            continue
        raw = explode(data[off:off+cs])
        if len(raw) != us:
            print('size mismatch', d, n, len(raw), us, file=sys.stderr)
        dest = safe_join(outdir, d, n)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        open(dest, 'wb').write(raw)
    print(f'{len(files)} files', file=sys.stderr)

if __name__ == '__main__':
    main()
