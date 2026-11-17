"""Fix VC6 for-loop scoping: a variable declared in a for-init and used after the loop.
Reads clang 'use of undeclared identifier' errors (file:line:col: ... 'NAME') from stdin.
For each, hoists the nearest preceding `for (TYPE NAME = ...` above the loop as `TYPE NAME;`
inserted on its own line, keeping indentation. Prints each change for review."""
import re, sys, collections
errs = collections.defaultdict(set)
for line in sys.stdin:
    m = re.match(r"(?:.*/gamesrc/)?([^:]+):(\d+):\d+: error: use of undeclared identifier '(\w+)'", line.strip())
    if m: errs[(m.group(1), m.group(3))].add(int(m.group(2)))
for (path, name), lines in sorted(errs.items()):
    src = open(path, encoding='latin-1').read().split('\n')
    pat = re.compile(r'^(\s*)(.*?)\bfor\s*\(\s*((?:unsigned\s+|const\s+)?[A-Za-z_]\w*(?:\s*\*)?)\s+' + name + r'\s*=')
    hoisted = set()
    for ln in sorted(lines, reverse=True):
        for j in range(ln - 1, max(-1, ln - 400), -1):
            if j in hoisted: break   # already hoisted for an earlier (lower) use
            m = pat.match(src[j])
            if m:
                indent, prefix, typ = m.group(1), m.group(2), m.group(3)
                if prefix.strip():
                    print(f'SKIP {path}:{j+1}: loop not at statement start: {src[j].strip()}'); break
                src[j] = re.sub(r'\bfor\s*\(\s*' + re.escape(typ) + r'\s+' + name + r'\s*=', f'for ({name} =', src[j], count=1)
                src.insert(j, f'{indent}{typ} {name};')
                hoisted = {d + 1 if d >= j else d for d in hoisted} | {j + 1}
                print(f'{path}:{j+1}: hoisted {typ} {name}')
                break
        else:
            print(f'NOTFOUND {path}:{ln} {name}')
    open(path, 'w', encoding='latin-1', newline='\n').write('\n'.join(src))
