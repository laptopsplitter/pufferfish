"""Disassemble functions from a parsed Luau file (see parse.py).

    python3 -I tools/luau/dis.py FILE.luac.pkl INDEX [INDEX ...]

Opcodes in Roblox client bytecode are stored multiplied by 227 (mod 256);
they are decoded with the inverse, 203. Constants are shown resolved.
"""
import sys, pickle
S, P, mainp = pickle.load(open(sys.argv[1], 'rb'))
NAMES = "NOP BREAK LOADNIL LOADB LOADN LOADK MOVE GETGLOBAL SETGLOBAL GETUPVAL SETUPVAL CLOSEUPVALS GETIMPORT GETTABLE SETTABLE GETTABLEKS SETTABLEKS GETTABLEN SETTABLEN NEWCLOSURE NAMECALL CALL RETURN JUMP JUMPBACK JUMPIF JUMPIFNOT JUMPIFEQ JUMPIFLE JUMPIFLT JUMPIFNOTEQ JUMPIFNOTLE JUMPIFNOTLT ADD SUB MUL DIV MOD POW ADDK SUBK MULK DIVK MODK POWK AND OR ANDK ORK CONCAT NOT MINUS LENGTH NEWTABLE DUPTABLE SETLIST FORNPREP FORNLOOP FORGLOOP FORGPREP_INEXT FASTCALL3 FORGPREP_NEXT NATIVECALL GETVARARGS DUPCLOSURE PREPVARARGS LOADKX JUMPX FASTCALL COVERAGE CAPTURE SUBRK DIVRK FASTCALL1 FASTCALL2 FASTCALL2K FORGPREP JUMPXEQKNIL JUMPXEQKB JUMPXEQKN JUMPXEQKS IDIV IDIVK".split()
AUX = {7, 8, 12, 15, 16, 20, 27, 28, 29, 30, 31, 32, 53, 55, 58, 60, 66, 74, 75, 77, 78, 79, 80}
def st(i): return S[i-1] if 0 < i <= len(S) else None
def kstr(K, i):
    if i >= len(K): return '?K%d' % i
    k = K[i]
    if isinstance(k, tuple):
        if k[0] == 's': return repr(st(k[1]))
        if k[0] == 'imp':
            v = k[1]; n = v >> 30; ids = [(v >> 20) & 1023, (v >> 10) & 1023, v & 1023][:n]
            return '.'.join(kstr(K, j).strip("'") for j in ids)
        return str(k)[:60]
    return repr(k)
def name(pi):
    p = P[pi]; return (st(p['name']) or '<anon>') + '@%d' % p['line']
def dis(pi, out):
    p = P[pi]; K = p['K']; code = p['code']; i = 0
    out.append('function %d %s params=%d' % (pi, name(pi), p['nparams']))
    while i < len(code):
        w = code[i]; op = ((w & 255) * 203) & 255; A = (w >> 8) & 255; B = (w >> 16) & 255; C = w >> 24
        D = w >> 16
        if D >= 32768: D -= 65536
        aux = code[i+1] if op in AUX and i + 1 < len(code) else None
        n = NAMES[op] if op < len(NAMES) else 'OP%d' % op
        s = '%4d %-12s A%d B%d C%d' % (i, n, A, B, C)
        if op in (5,): s = '%4d LOADK       R%d = %s' % (i, A, kstr(K, D))
        elif op == 4: s = '%4d LOADN       R%d = %d' % (i, A, D)
        elif op == 3: s = '%4d LOADB       R%d = %s' % (i, A, bool(B))
        elif op == 6: s = '%4d MOVE        R%d = R%d' % (i, A, B)
        elif op == 12: s = '%4d GETIMPORT   R%d = %s' % (i, A, kstr(K, D))
        elif op in (7, 8): s = '%4d %s R%d %s' % (i, n, A, kstr(K, aux))
        elif op == 15: s = '%4d GETTABLEKS  R%d = R%d.%s' % (i, A, B, kstr(K, aux).strip("'"))
        elif op == 16: s = '%4d SETTABLEKS  R%d.%s = R%d' % (i, B, kstr(K, aux).strip("'"), A)
        elif op == 20: s = '%4d NAMECALL    R%d = R%d:%s' % (i, A, B, kstr(K, aux).strip("'"))
        elif op == 21: s = '%4d CALL        R%d(%d args) -> %d' % (i, A, B - 1, C - 1)
        elif op == 9: s = '%4d GETUPVAL    R%d = U%d' % (i, A, B)
        elif op == 19: s = '%4d NEWCLOSURE  R%d = fn %s' % (i, A, name(p['P'][D]) if D < len(p['P']) else D)
        elif op == 64: s = '%4d DUPCLOSURE  R%d = fn %s' % (i, A, kstr(K, D))
        elif op in (23, 24, 25, 26): s = '%4d %-11s R%d -> %d' % (i, n, A, i + 1 + D)
        elif op in (77, 78, 79, 80):
            s = '%4d %-11s R%d %s %s -> %d' % (i, n, A, 'NOT' if aux >> 31 else '==', kstr(K, aux & 0xffffff) if op != 77 else 'nil', i + 1 + D)
        elif op in (27, 28, 29, 30, 31, 32): s = '%4d %-11s R%d R%d -> %d' % (i, n, A, aux, i + 1 + D)
        elif op == 22: s = '%4d RETURN      R%d (%d)' % (i, A, B - 1)
        elif op in (39, 40, 41, 42, 43, 44, 47, 48, 49): s = '%4d %-11s R%d = R%d, %s' % (i, n, A, B, kstr(K, C) if op != 49 else 'R%d..R%d' % (B, C))
        elif op == 54: s = '%4d DUPTABLE    R%d = %s' % (i, A, kstr(K, D))
        out.append(s)
        i += 2 if aux is not None else 1
if __name__ == '__main__':
    out = []
    for a in sys.argv[2:]:
        dis(int(a), out)
        out.append('')
    print('\n'.join(out))
