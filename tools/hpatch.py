#!/usr/bin/env python3
"""Hashed patches: our changes to Monolith's game source, written without
quoting it, so they can live in the repository (the source licence forbids
sharing their code, and an ordinary diff is full of it).

A hashed patch says where to change a file and what to put there, but never
what was there: lines are named by number and checked by hash against the
builder's own copy of the source, and each new line is made of pieces copied
from the original ([line, from, to]) and text we wrote ("..."). Unchanged
lines are copied from the builder's source.

    hpatch.py make  BASE EDITED PATCH   write PATCH from the changes BASE -> EDITED
    hpatch.py apply PATCH TREE          apply PATCH to TREE (a fresh BASE) in place
    hpatch.py show  PATCH BASE          the changes as an ordinary diff (for review;
                                        it quotes the source, so keep it local)
    hpatch.py check PATCH BASE          the leak check on its own

BASE is the normalized source (prepare_source.py --no-patches). make refuses
to write a patch whose own text holds a run of LEAK_RUN or more characters
found anywhere in BASE.

File format (text, one record per line):
    hpatch 1
    file PATH SHA            a changed file and the hash of its original
    new PATH                 a file of ours, every line literal
    delete PATH SHA          a file left out
    @ LINE COUNT SHA         replace COUNT original lines from LINE (1-based;
                             COUNT 0 inserts before LINE), SHA hashes them
    + [pieces]               one new line: a JSON list of strings (ours) and
                             [line, from, to] (characters of an original line;
                             to null = to its end, newline included) or
                             [path, line, from, to] (of another original file)
"""
import difflib, hashlib, json, os, sys

LEAK_RUN = 24   # characters in a row: what counts as quoting
FIND_RUN = 10   # literal runs this long are looked up in the file and copied instead
FAR_RUN = 16    # ...and runs this long in the other files (shorter ones aren't worth the scatter)
TEXT_EXT = {'.h', '.c', '.cpp', '.inl', '.rc', '.def'}


def sha(text):
    return hashlib.sha256(text.encode('latin-1')).hexdigest()[:16]


def read(path):
    with open(path, 'r', encoding='latin-1', newline='') as f:
        return f.read()


def write(path, text):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'w', encoding='latin-1', newline='') as f:
        f.write(text)


def text_files(root):
    out = []
    for d, _, files in os.walk(root):
        for f in files:
            if os.path.splitext(f)[1].lower() in TEXT_EXT:
                out.append(os.path.relpath(os.path.join(d, f), root).replace(os.sep, '/'))
    return sorted(out)


# ---- making ----

class Original:
    """One original file, for finding text to copy instead of quoting; other
    (the Corpus) is searched when a run isn't in this file."""

    def __init__(self, text, other=None, path=None):
        self.other, self.path = other, path
        self.lines = text.splitlines(keepends=True)
        self.text = text
        self.starts = []
        pos = 0
        for line in self.lines:
            self.starts.append(pos)
            pos += len(line)

    def locate(self, offset):
        import bisect
        i = bisect.bisect_right(self.starts, offset) - 1
        return i, offset - self.starts[i]

    def find_runs(self, s):
        """s as pieces: runs of FIND_RUN+ characters found in one original line
        become [line, from, to]; the rest stays text."""
        pieces, lit, i = [], '', 0
        while i < len(s):
            j = i + FIND_RUN
            at = self.text.find(s[i:j]) if j <= len(s) else -1
            if at < 0 and j <= len(s) and self.other and '\n' not in s[i:j - 1]:
                ref = self.other.find_run(s, i, self.path)
                if ref:
                    if lit:
                        pieces.append(lit)
                        lit = ''
                    pieces.append(ref[0])
                    i = ref[1]
                    continue
            if at < 0 or '\n' in s[i:j - 1]:
                lit += s[i]
                i += 1
                continue
            # Grow the run while it still occurs (and stays on one line).
            while j < len(s) and '\n' not in s[i:j]:
                nxt = self.text.find(s[i:j + 1])
                if nxt < 0:
                    break
                at, j = nxt, j + 1
            line, col = self.locate(at)
            if col + (j - i) > len(self.lines[line]):
                lit += s[i]
                i += 1
                continue
            if lit:
                pieces.append(lit)
                lit = ''
            end = col + (j - i)
            pieces.append([line + 1, col, None if end == len(self.lines[line]) else end])
            i = j
        if lit:
            pieces.append(lit)
        return pieces


class Corpus:
    """Every original file, for runs copied from elsewhere."""

    def __init__(self, base, files):
        self.files, self.parts, self.starts, pos = {}, [], [], 0
        for rel in files:
            o = Original(read(os.path.join(base, rel)))
            self.files[rel] = o
            self.parts.append(rel)
            self.starts.append(pos)
            pos += len(o.text) + 1
        self.text = '\0'.join(self.files[r].text for r in self.parts)

    def find_run(self, s, i, skip):
        """The longest run of s from i (FAR_RUN+ characters, one line) in a
        file other than skip: ([path, line, from, to], end) or None."""
        import bisect
        j = i + FAR_RUN
        if j > len(s) or '\n' in s[i:j - 1]:
            return None
        at = self.text.find(s[i:j])
        if at < 0:
            return None
        while j < len(s) and '\n' not in s[i:j]:
            nxt = self.text.find(s[i:j + 1])
            if nxt < 0:
                break
            at, j = nxt, j + 1
        k = bisect.bisect_right(self.starts, at) - 1
        rel = self.parts[k]
        if rel == skip:
            return None  # the file's own search already had its chance
        o = self.files[rel]
        line, col = o.locate(at - self.starts[k])
        end = col + (j - i)
        if end > len(o.lines[line]):
            return None
        return [rel, line + 1, col, None if end == len(o.lines[line]) else end], j


def encode_line(new, candidates, orig):
    """A new line as pieces: matched against the most similar candidate
    original line, then any long literal runs looked up in the whole file."""
    best, best_n = None, -1
    for idx in candidates:
        sm = difflib.SequenceMatcher(None, orig.lines[idx], new, autojunk=False)
        n = sum(b.size for b in sm.get_matching_blocks() if b.size >= 3)
        if n > best_n:
            best, best_n = idx, n
    pieces = []
    if best is not None and best_n > 0:
        src = orig.lines[best]
        pos = 0
        for b in difflib.SequenceMatcher(None, src, new, autojunk=False).get_matching_blocks():
            if b.size < 3:
                continue
            if b.b > pos:
                pieces.append(new[pos:b.b])
            end = b.a + b.size
            pieces.append([best + 1, b.a, None if end == len(src) else end])
            pos = b.b + b.size
        if pos < len(new):
            pieces.append(new[pos:])
    else:
        pieces = [new]
    out = []
    for p in pieces:
        out.extend(orig.find_runs(p) if isinstance(p, str) and len(p) >= FIND_RUN else [p])
    return merge(out)


def merge(pieces):
    out = []
    for p in pieces:
        if isinstance(p, str) and out and isinstance(out[-1], str):
            out[-1] += p
        elif not (isinstance(p, str) and not p):
            out.append(p)
    return out


def make(base, edited, patch_path):
    records = ['hpatch 1']
    base_files, edited_files = set(text_files(base)), set(text_files(edited))
    corpus = Corpus(base, sorted(base_files))
    for rel in sorted(base_files | edited_files):
        if rel not in edited_files:
            records.append(f'delete {rel} {sha(read(os.path.join(base, rel)))}')
            continue
        new_text = read(os.path.join(edited, rel))
        if rel not in base_files:
            records.append(f'new {rel}')
            orig = Original('')
            for line in new_text.splitlines(keepends=True):
                records.append('+ ' + json.dumps([line]))
            continue
        old_text = read(os.path.join(base, rel))
        if old_text == new_text:
            continue
        orig = Original(old_text, corpus, rel)
        new_lines = new_text.splitlines(keepends=True)
        records.append(f'file {rel} {sha(old_text)}')
        sm = difflib.SequenceMatcher(None, orig.lines, new_lines, autojunk=False)
        for tag, a1, a2, b1, b2 in sm.get_opcodes():
            if tag == 'equal':
                continue
            removed = ''.join(orig.lines[a1:a2])
            records.append(f'@ {a1 + 1} {a2 - a1} {sha(removed) if a2 > a1 else "-"}')
            # Edited lines are usually near the ones they replace.
            near = list(range(max(0, a1 - 3), min(len(orig.lines), a2 + 3)))
            for line in new_lines[b1:b2]:
                records.append('+ ' + json.dumps(encode_line(line, near, orig)))
    text = '\n'.join(records) + '\n'
    leaks = check_text(text, base)
    if leaks:
        for where, rel, run in leaks[:20]:
            sys.stderr.write(f'leak in {where}: {len(run)} characters of {rel}: {run[:60]!r}\n')
        sys.exit(f'hpatch: refusing to write {patch_path}: it would quote the original source')
    # Round trip: applying it to BASE must give EDITED exactly.
    for rel, result in apply_text(text, lambda r: read(os.path.join(base, r)) if r in base_files else None).items():
        want = read(os.path.join(edited, rel)) if rel in edited_files else None
        if result != want:
            sys.exit(f'hpatch: round trip failed for {rel}')
    write(patch_path, text)
    changed = sum(1 for r in records if r.startswith(('file ', 'new ', 'delete ')))
    print(f'{changed} files -> {patch_path}')


# ---- applying ----

def parse(text):
    lines = text.split('\n')
    if not lines or lines[0] != 'hpatch 1':
        raise ValueError('not a hashed patch (version 1)')
    files, cur = [], None
    for n, rec in enumerate(lines[1:], 2):
        if not rec:
            continue
        kind, _, rest = rec.partition(' ')
        if kind in ('file', 'new', 'delete'):
            parts = rest.split(' ')
            cur = {'kind': kind, 'path': parts[0], 'sha': parts[1] if len(parts) > 1 else None, 'hunks': []}
            files.append(cur)
            if kind == 'new':
                cur['hunks'].append({'line': 1, 'count': 0, 'sha': '-', 'lines': []})
        elif kind == '@':
            line, count, h = rest.split(' ')
            cur['hunks'].append({'line': int(line), 'count': int(count), 'sha': h, 'lines': []})
        elif kind == '+':
            cur['hunks'][-1]['lines'].append(json.loads(rest))
        else:
            raise ValueError(f'line {n}: unknown record {kind!r}')
    return files


def build_line(pieces, orig_lines, where, read_original=None):
    out = ''
    for p in pieces:
        if isinstance(p, str):
            out += p
        else:
            lines = orig_lines
            if len(p) == 4:
                other = read_original(p[0]) if read_original else None
                if other is None:
                    raise ValueError(f'{where}: copies from {p[0]}, which is missing')
                lines = other.splitlines(keepends=True)
                p = p[1:]
            line, a, b = p
            if not 1 <= line <= len(lines):
                raise ValueError(f'{where}: no original line {line}')
            src = lines[line - 1]
            out += src[a:] if b is None else src[a:b]
    return out


def apply_text(text, read_original):
    """{path: new text, or None for deleted} from a patch and a reader of originals."""
    results, cache = {}, {}

    def original_of(rel):
        if rel not in cache:
            cache[rel] = read_original(rel)
        return cache[rel]

    files = parse(text)
    for f in files:
        if f['kind'] != 'new':
            original_of(f['path'])
        for h in f['hunks']:
            for p in h['lines']:
                for q in p:
                    if isinstance(q, list) and len(q) == 4:
                        original_of(q[0])
    for f in files:
        path = f['path']
        if f['kind'] == 'new':
            results[path] = ''.join(build_line(p, [], path) for p in f['hunks'][0]['lines'])
            continue
        original = original_of(path)
        if original is None:
            raise ValueError(f'{path}: missing from the source')
        if sha(original) != f['sha']:
            raise ValueError(f'{path}: not the file this patch was made for (another source version?)')
        if f['kind'] == 'delete':
            results[path] = None
            continue
        lines = original.splitlines(keepends=True)
        out, pos = [], 0
        for h in f['hunks']:
            start = h['line'] - 1
            if start < pos or start + h['count'] > len(lines):
                raise ValueError(f'{path}: change at line {h["line"]} out of order or past the end')
            removed = ''.join(lines[start:start + h['count']])
            if h['count'] and sha(removed) != h['sha']:
                raise ValueError(f'{path}: lines {h["line"]}+{h["count"]} differ from what the patch expects')
            out.extend(lines[pos:start])
            out.extend(build_line(p, lines, f'{path}:{h["line"]}', original_of) for p in h['lines'])
            pos = start + h['count']
        out.extend(lines[pos:])
        results[path] = ''.join(out)
    return results


def apply(patch_path, tree):
    results = apply_text(read(patch_path), lambda r: read(os.path.join(tree, r)) if os.path.exists(os.path.join(tree, r)) else None)
    for rel, text in results.items():
        target = os.path.join(tree, rel)
        if text is None:
            os.remove(target)
        else:
            write(target, text)
    return len(results)


# ---- the leak check ----

def check_text(text, base):
    """(file, run) for each run of LEAK_RUN+ characters of the patch's own
    text (its literal strings, joined per new line) found in BASE."""
    literals, where = [], '?'
    for rec in text.split('\n'):
        if rec.startswith(('file ', 'new ', 'delete ')):
            where = rec.split(' ')[1]
        elif rec.startswith('+ '):
            for p in json.loads(rec[2:]):
                if isinstance(p, str) and len(p) >= LEAK_RUN:
                    literals.append((where, p))
    if not literals:
        return []
    grams = {}
    for where, lit in literals:
        for i in range(len(lit) - LEAK_RUN + 1):
            grams.setdefault(lit[i:i + LEAK_RUN], where)
    leaks, seen = [], set()
    for rel in text_files(base):
        src = read(os.path.join(base, rel))
        for i in range(len(src) - LEAK_RUN + 1):
            g = src[i:i + LEAK_RUN]
            if g in grams and g not in seen:
                seen.add(g)
                leaks.append((grams[g], rel, g))
    return leaks


def main():
    if len(sys.argv) != 5 and not (len(sys.argv) == 4 and sys.argv[1] in ('apply', 'show', 'check')):
        sys.exit(__doc__)
    cmd = sys.argv[1]
    try:
        if cmd == 'make':
            make(sys.argv[2], sys.argv[3], sys.argv[4])
        elif cmd == 'apply':
            n = apply(sys.argv[2], sys.argv[3])
            print(f'applied {os.path.basename(sys.argv[2])}: {n} files')
        elif cmd == 'show':
            base = sys.argv[3]
            results = apply_text(read(sys.argv[2]), lambda r: read(os.path.join(base, r)) if os.path.exists(os.path.join(base, r)) else None)
            for rel, text in results.items():
                old = read(os.path.join(base, rel)) if os.path.exists(os.path.join(base, rel)) else ''
                for line in difflib.unified_diff(old.splitlines(keepends=True), (text or '').splitlines(keepends=True),
                                                 'a/' + rel, 'b/' + rel):
                    sys.stdout.write(line if line.endswith('\n') else line + '\n\\ No newline at end of file\n')
        elif cmd == 'check':
            leaks = check_text(read(sys.argv[2]), sys.argv[3])
            for where, rel, run in leaks:
                print(f'leak in {where}: {rel}: {run!r}')
            print(f'{len(leaks)} leak(s)')
            sys.exit(1 if leaks else 0)
        else:
            sys.exit(__doc__)
    except ValueError as e:
        sys.exit(f'hpatch: {e}')


if __name__ == '__main__':
    main()
