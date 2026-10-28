"""Parse a Luau bytecode file (Roblox client format, version 9).

    python3 -I tools/luau/parse.py FILE.luac       -> writes FILE.luac.pkl

Layout: 8-byte header, version, types version, string table, userdata type
list, function prototypes (constants, children, line info, debug info), main.
Read-only analysis of the user's own APK contents; nothing is executed.
"""
import sys, struct
d = open(sys.argv[1], 'rb').read()
p = 0
def u8():
    global p; v = d[p]; p += 1; return v
def vi():
    global p
    r = s = 0
    while True:
        b = d[p]; p += 1; r |= (b & 0x7f) << s; s += 7
        if b < 0x80: return r
p = 8
ver, tver = u8(), u8()
n = vi(); S = []
for _ in range(n):
    l = vi(); S.append(d[p:p+l].decode('utf-8', 'replace')); p += l
while u8() != 0: vi() if False else None  # userdata remap: index byte, name varint
nproto = vi()
protos = []
EXTRA = int(sys.argv[2]) if len(sys.argv) > 2 else 2
for i in range(nproto):
    start = p
    maxstack, nparams, nups, vararg = u8(), u8(), u8(), u8()
    flags = u8(); ts = vi(); p += ts
    sc = vi(); code = struct.unpack_from('<%dI' % sc, d, p); p += 4*sc
    sk = vi(); K = []
    for _ in range(sk):
        t = u8()
        if t == 0: K.append(None)
        elif t == 1: K.append(bool(u8()))
        elif t == 2: K.append(struct.unpack_from('<d', d, p)[0]); p += 8
        elif t == 3: K.append(('s', vi()))
        elif t == 4: K.append(('imp', struct.unpack_from('<I', d, p)[0])); p += 4
        elif t == 5:
            c = vi(); K.append(('tab', [vi() for _ in range(c)]))
        elif t == 6: K.append(('clo', vi()))
        elif t == 7: K.append(('vec', struct.unpack_from('<4f', d, p))); p += 16
        elif t == 8:
            c = vi(); kv = []
            for _ in range(c):
                k = vi(); kv.append((k, struct.unpack_from('<i', d, p)[0])); p += 4
            K.append(('tabk', kv))
        elif t == 9: K.append(('int', struct.unpack_from('<q', d, p)[0])); p += 8
        else:
            sys.exit('proto %d @%d: unknown constant type %d (k %d/%d)' % (i, start, t, len(K), sk))
    spn = vi(); P = [vi() for _ in range(spn)]
    linedef = vi(); dname = vi()
    if u8():
        lg = u8(); p += sc; p += 4 * (((sc - 1) >> lg) + 1)
    if u8():
        for _ in range(vi()): vi(); vi(); vi(); u8()
        for _ in range(vi()): vi()
    protos.append(dict(off=start, code=code, K=K, P=P, line=linedef, name=dname, nparams=nparams))
mainp = vi()
print('version', ver, 'types', tver, 'strings', n, 'protos', nproto, 'main', mainp, 'end', p, 'size', len(d), file=sys.stderr)
import pickle
pickle.dump((S, protos, mainp), open(sys.argv[1] + '.pkl', 'wb'))
