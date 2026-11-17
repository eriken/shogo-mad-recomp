#!/usr/bin/env python3
"""Copy Monolith's Shogo v2.2 game source into the build tree, normalized for a
modern Clang build on macOS. The pristine source in third_party/ is never modified.

Steps, in order:
  1. Copy AppHeaders, Shared, ClientShellDLL, ObjectDLL, ClientRes, ServerRes, Misc.
  2. CRLF -> LF, and backslashes -> slashes inside #include lines.
  3. Apply each patches/*.patch (unified diff, -p1 relative to the copied tree).

Usage: prepare_source.py SRC_DIR PATCH_DIR OUT_DIR
       prepare_source.py SRC_DIR --no-patches OUT_DIR   (baseline for making patches)
"""
import os, re, shutil, subprocess, sys

DIRS = ['AppHeaders', 'Shared', 'ClientShellDLL', 'ObjectDLL', 'ClientRes', 'ServerRes', 'Misc']
TEXT_EXT = {'.h', '.c', '.cpp', '.inl', '.rc', '.def'}
INCLUDE_RE = re.compile(r'^(\s*#\s*include\s*[<"])([^>"]+)([>"])', re.M)


def normalize(text):
    text = text.replace('\r\n', '\n').replace('\r', '\n')
    return INCLUDE_RE.sub(lambda m: m.group(1) + m.group(2).replace('\\', '/') + m.group(3), text)


def main():
    src, patch_dir, out = sys.argv[1:4]
    stamp = os.path.join(out, '.prepared')
    if os.path.isdir(out):
        shutil.rmtree(out)
    for d in DIRS:
        for root, _, files in os.walk(os.path.join(src, d)):
            rel = os.path.relpath(root, src)
            os.makedirs(os.path.join(out, rel), exist_ok=True)
            for f in files:
                s, t = os.path.join(root, f), os.path.join(out, rel, f)
                if os.path.splitext(f)[1].lower() in TEXT_EXT:
                    with open(s, 'r', encoding='latin-1', newline='') as fh:
                        data = normalize(fh.read())
                    with open(t, 'w', encoding='latin-1', newline='\n') as fh:
                        fh.write(data)
                else:
                    shutil.copyfile(s, t)
    patches = sorted(p for p in os.listdir(patch_dir) if p.endswith('.patch')) if os.path.isdir(patch_dir) else []
    if patch_dir == '--no-patches':
        patches = []
    for p in patches:
        r = subprocess.run(['patch', '-p1', '--forward', '--batch', '--no-backup-if-mismatch', '-d', out,
                            '-i', os.path.abspath(os.path.join(patch_dir, p))], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f'patch {p} failed:\n{r.stdout}{r.stderr}')
    open(stamp, 'w').write('\n'.join(patches) + '\n')
    print(f'prepared {out} ({len(patches)} patches)')


if __name__ == '__main__':
    main()
