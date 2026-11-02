#!/usr/bin/env python3
"""seqdiff.py: what one moment ran at one setting and never at the other.

The verify build with RECOMP_SEQ_WINDOW=<ms> writes seqw_<frame>.txt when a hook arms it (hook_seq_arm; Killzone's
takedown on the player): every (function, caller) pair called in that window, with the order of its first call. Give
it the records of each side (each file's header names its frame rate); it prints the pairs every record of one side has
and no record of the other, in the order the first record of that side met them. Several records a side drop the
noise a single moment carries (a sound, an effect, an AI choice).

    seqdiff.py seqw_*.txt          (sides by frame rate)
    seqdiff.py a1.txt a2.txt -- b1.txt b2.txt
"""
import sys


def load(path):
    fps, pairs = None, {}
    with open(path) as f:
        for line in f:
            if line.startswith('#'):
                if fps is None and ' fps, ' in line:
                    fps = int(line.split(' fps, ')[0].rsplit(' ', 1)[1])
                continue
            p = line.split(maxsplit=4)
            if len(p) < 4:
                continue
            key = (int(p[2], 16), int(p[3], 16))
            pairs[key] = (int(p[0]), int(p[1]), p[4].strip() if len(p) > 4 else '?')
    return fps, pairs


def side_only(mine, theirs):
    keys = set(mine[0][1])
    for _, pairs in mine[1:]:
        keys &= set(pairs)
    for _, pairs in theirs:
        keys -= set(pairs)
    first = mine[0][1]
    return sorted(keys, key=lambda k: first[k][0])


def show(title, keys, recs):
    print(f'== {title}: {len(keys)} pairs')
    for k in keys:
        counts = ' '.join(str(p[k][1]) for _, p in recs)
        order, _, thread = recs[0][1][k]
        print(f'  {order:>10}  fn_{k[0]:08x} from {k[1]:08x}  count {counts:<12} {thread}')


def main(args):
    if '--' in args:
        i = args.index('--')
        a = [(path, load(path)[1]) for path in args[:i]]
        b = [(path, load(path)[1]) for path in args[i + 1:]]
        names = ('first side', 'second side')
    else:
        recs = [(path, *load(path)) for path in args]
        a = [(p, pairs) for p, fps, pairs in recs if fps == 60]
        b = [(p, pairs) for p, fps, pairs in recs if fps != 60]
        names = ('60 fps', '30 fps')
    if not a or not b:
        sys.exit('seqdiff: need records of both sides')
    print(f'{names[0]}: ' + ' '.join(p for p, _ in a))
    print(f'{names[1]}: ' + ' '.join(p for p, _ in b))
    show(f'only at {names[1]}', side_only(b, a), b)
    show(f'only at {names[0]}', side_only(a, b), a)


if __name__ == '__main__':
    main(sys.argv[1:])
