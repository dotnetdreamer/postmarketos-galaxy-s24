#!/usr/bin/python3
# s24bt safety check: compare the layout (member names, offsets, sizes) of every struct/union that
# exists both in the running kernel's BTF (/sys/kernel/btf/vmlinux, copied to vmlinux.btf) and in
# my module's DWARF. Any difference means the module was built against different types: don't load.
import subprocess, re, sys

DEV = ['-F', 'btf', '/work/s24bt/vmlinux.btf']
MINE = ['-F', 'dwarf', sys.argv[1] if len(sys.argv) > 1 else '/work/s24bt/s24bt.ko']


def names(src):
    out = subprocess.run(['pahole'] + src + ['--sizes'], capture_output=True, text=True).stdout
    return {l.split('\t')[0] for l in out.splitlines() if '\t' in l}


def strip_attrs(s):
    # remove __attribute__((...)) with balanced parentheses
    while '__attribute__' in s:
        i = s.index('__attribute__')
        j = s.index('(', i)
        depth = 0
        for k in range(j, len(s)):
            depth += {'(': 1, ')': -1}.get(s[k], 0)
            if depth == 0:
                break
        s = s[:i] + s[k + 1:]
    return s


def member(line):
    if '/*' not in line:
        return None
    decl, _, rest = line.rpartition('/*')
    nums = ' '.join(rest.replace('*/', '').split())
    decl = strip_attrs(decl).strip().rstrip(';').strip()
    m = re.search(r'\(\s*\*\s*(\w+)\s*\)\s*\(', decl)       # function pointer
    if m:
        return (m.group(1), nums)
    decl = re.sub(r'\s*:\s*\d+$', '', decl)                   # bitfield width
    decl = re.sub(r'(\[[^\]]*\])+$', '', decl).strip()        # array dims
    m = re.search(r'(\w+)$', decl)
    return (m.group(1), nums) if m else (decl, nums)


def layouts(src, wanted):
    res = {}
    for i in range(0, len(wanted), 40):
        chunk = wanted[i:i + 40]
        out = subprocess.run(['pahole'] + src[:2] + ['-C', ','.join(chunk), src[2]],
                             capture_output=True, text=True).stdout
        cur = None
        for ln in out.splitlines():
            m = re.match(r'^(struct|union) (\S+) \{', ln)
            if m:
                cur = m.group(2)
                res[cur] = []
                continue
            if ln.startswith('};'):
                cur = None
                continue
            if cur and ln.startswith('\t') and not ln.startswith('\t\t') and not ln.startswith('\t/*'):
                mem = member(ln)
                if mem:
                    res[cur].append(mem)
    return res


common = sorted(names(DEV) & names(MINE))
d = layouts(DEV, common)
m = layouts(MINE, common)
bad = []
for n in common:
    if d.get(n) is None or m.get(n) is None:
        # a batch didn't print it: ask for it alone
        for src, res in ((DEV, d), (MINE, m)):
            if res.get(n) is None:
                res.update(layouts(src, [n]))
    if d.get(n) != m.get(n):
        bad.append(n)
for n in bad[:20]:
    a, b = d.get(n) or [], m.get(n) or []
    diffs = [(x, y) for x, y in zip(a, b) if x != y][:2]
    print('DIFF %s: device %d members, mine %d; first differences %s' % (n, len(a), len(b), diffs))
print('%d structs/unions in both, compared member by member: %d differ' % (len(common), len(bad)))
