"""List functions whose constants include given strings.

    python3 -I tools/luau/find.py FILE.luac.pkl NAME [~SUBSTRING ...]
"""
import sys, pickle
S, P, mainp = pickle.load(open(sys.argv[1], 'rb'))
want = sys.argv[2:]
idx = {i + 1: s for i, s in enumerate(S) if any(w == s or (w.startswith('~') and w[1:] in s) for w in want)}
for pi, p in enumerate(P):
    hits = [idx[k[1]] for k in p['K'] if isinstance(k, tuple) and k[0] == 's' and k[1] in idx]
    if hits:
        nm = S[p['name']-1] if p['name'] else '<anon>'
        print(pi, nm, p['line'], len(p['code']), sorted(set(hits))[:6])
