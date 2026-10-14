# Per-program fingerprint of the HCTP VU1 programs (reads logs/vu1map/pNN.txt made by vu1_dump.py).
import re, glob, os, collections, sys
d = sys.argv[1]
KEY = ['XGKICK', 'DIV', 'SQRT', 'RSQRT', 'MULq', 'ADDq', 'CLIP', 'FCAND', 'FCOR', 'FCSET', 'FCGET', 'FSAND', 'FMAND',
       'ERSQRT', 'ERSADD', 'ESADD', 'ESQRT', 'EATAN', 'EEXP', 'ESIN', 'ERCPR', 'MFP', 'WAITP', 'WAITQ',
       'FTOI0', 'FTOI4', 'FTOI12', 'FTOI15', 'ITOF0', 'ITOF4', 'ITOF12', 'ITOF15', 'OPMULA', 'OPMSUB', 'MINI', 'MAX',
       'XTOP', 'XITOP', 'BAL', 'JR', 'JALR', 'MFIR', 'MTIR', 'ILW', 'ISW', 'ILWR', 'ISWR', 'LQI', 'SQI', 'LQD', 'SQD', 'RINIT', 'RNEXT', 'RGET']
for f in sorted(glob.glob(os.path.join(d, 'p*.txt'))):
    lines = open(f).read().splitlines()
    head, body = lines[0], lines[1:]
    ops = collections.Counter()
    branches = 0
    ebits = 0
    for l in body:
        m = re.match(r'(\w+): (\S+)\s.*\| (\S+)', l + ' ')
        up = l.split('|')[0].split(':', 1)[1].strip().split(' ')[0].split('.')[0]
        lo = l.split('|')[1].strip().split(' ')[0].split('.')[0]
        ops[up] += 1
        ops[lo] += 1
        if lo.startswith('IB') or lo in ('B', 'BAL'):
            branches += 1
        if '[E]' in l:
            ebits += 1
    feats = ' '.join(f'{k}:{ops[k]}' for k in KEY if ops[k])
    print(head.split(',')[0], f'| branches={branches} ends={ebits}')
    print('   ', feats)
