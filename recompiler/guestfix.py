"""Known bugs in Sony's libraries as games ship them, fixed in the code before it is analysed and recompiled (analyze.py):
each a byte pattern ("??" any byte) that must occur exactly once in a module's code, and the bytes written over the start
of the match ("??" keeps the byte there). A copy where the pattern isn't found, or is found more than once, is left alone.

libfios2's lost kick: found by nckstwrt's Vita3K-Plus (vita3k/modules/guest_code_fixes.cpp, GPL-2.0), the analysis and
the patch theirs. libfios2's ExecuteChunk drops the scheduler lock while it dispatches a chunk; on "would block" it puts
the chunk back and moves the scan cursor on with cursor + 1. A kick in between (a decompression job finishing sets the
cursor to 0 and signals a scheduler that isn't waiting yet) becomes 1, and the chunk at queue position 0 is never looked at
again: a finished job stays queued holding its archive buffers and every later read waits behind it. Later libfios2
builds guard the increment with a kick counter; here the cursor only moves on while it is still non-zero. The builds
with it (2026-11-21): Need for Speed Most Wanted, Assassin's Creed III: Liberation, Resistance: Burning Skies and two more
of the library's games."""

FIXES = [
    ("libfios2: the scheduler no longer loses a kick while it re-queues a blocked chunk",
     "41 F2 7C 00 58 F8 00 20 40 46 39 1C ?? ?? ?? ?? "   # movw r0,#0x107c; ldr.w r2,[r8,r0]; mov r0,r8; adds r1,r7,#0; bl insert
     "58 F8 04 00 01 30 48 F8 04 00 02 E0 28 1C ?? ?? ?? ?? "   # ldr.w r0,[r8,r4]; adds r0,#1; str.w r0,[r8,r4]; b would_block; adds r0,r5,#0; bl lock
     "1E 20 C8 F2 82 00",   # would_block: movs r0,#0x1e; movt r0,#0x8082
     "08 EB 04 06 B2 68 40 46 39 1C 00 BF ?? ?? ?? ?? "   # add.w r6,r8,r4; ldr r2,[r6,#8]; mov r0,r8; adds r1,r7,#0; nop; bl insert
     "30 68 10 B1 01 30 30 60 00 BF"),   # ldr r0,[r6]; cbz r0,(past); adds r0,#1; str r0,[r6]; nop (the b would_block follows)
]


def _bytes(text):
    return [None if t == "??" else int(t, 16) for t in text.split()]


def _find(code, pat):
    hits, n, first = [], len(pat), pat[0]
    start = 0
    while True:
        i = code.find(bytes([first]), start) if first is not None else start
        if i < 0 or i + n > len(code): return hits
        if all(p is None or code[i + k] == p for k, p in enumerate(pat)): hits.append(i)
        start = i + 1


def apply(code, log=None):
    """code: the module's text as a bytearray, patched in place; -> the descriptions of the fixes applied"""
    done = []
    for what, pattern, replacement in FIXES:
        pat, rep = _bytes(pattern), _bytes(replacement)
        hits = _find(code, pat)
        if len(hits) != 1:
            if len(hits) > 1 and log: log(f"guest fix not applied ({len(hits)} matches): {what}")
            continue
        for k, b in enumerate(rep):
            if b is not None: code[hits[0] + k] = b
        done.append(what)
        if log: log(f"guest fix at +{hits[0]:#x}: {what}")
    return done
