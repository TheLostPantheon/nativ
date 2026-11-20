#!/usr/bin/env python3
"""vitasync_test: the launcher's Vita sync (launcher/vitasync.cpp) against a stand-in Vita.

The stand-in speaks Nativ Sync's protocol (TheLostPantheon/nativ-sync, PROTOCOL.md) through the same C file the Vita's
service runs (third_party/nativ-sync/nsync_proto.c, here over ctypes), and keeps its saves in memory: pairing, the
encrypted session, LIST, MANIFEST, GET, PUT, COMMIT, BASE, GUARD. The launcher's side runs as dev/tests/vitasync_cli.cpp
(nativ --vita-sync's commands without the window). Needs cc and c++ (C++17)."""
import ctypes, hashlib, json, os, secrets, shutil, socket, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SLOT = 0x34C
fails = []

def check(cond, what):
    print(("   ok  " if cond else "   FAIL ") + what)
    if not cond: fails.append(what)

def build(tmp):
    cc, cxx = os.environ.get("CC") or "cc", os.environ.get("CXX") or "c++"
    mono = os.path.join(ROOT, "third_party", "monocypher"); ns = os.path.join(ROOT, "third_party", "nativ-sync")
    lib = os.path.join(tmp, "libnsync.so")
    subprocess.check_call([cc, "-O1", "-shared", "-fPIC", "-I", mono, "-o", lib, os.path.join(ns, "nsync_proto.c"), os.path.join(mono, "monocypher.c")])
    objs = []
    for src in (os.path.join(ns, "nsync_proto.c"), os.path.join(mono, "monocypher.c")):
        o = os.path.join(tmp, os.path.basename(src) + ".o"); objs.append(o)
        subprocess.check_call([cc, "-O1", "-std=c99", "-I", mono, "-c", src, "-o", o])
    cli = os.path.join(tmp, "vitasync_cli")
    subprocess.check_call([cxx, "-O1", "-std=c++17", "-I", os.path.join(ROOT, "launcher"), "-I", ns, "-I", mono,
                           os.path.join(ROOT, "dev", "tests", "vitasync_cli.cpp"), os.path.join(ROOT, "launcher", "vitasync.cpp")] + objs + ["-lpthread", "-o", cli])
    L = ctypes.CDLL(lib)
    L.nsync_seal.restype = ctypes.c_size_t; L.nsync_open.restype = ctypes.c_long; L.nsync_frame_length.restype = ctypes.c_uint32
    return L, cli

def blake(b): return hashlib.blake2b(b, digest_size=32).hexdigest()

class Save:
    def __init__(self, version="01.00", name="A Game"):
        self.files, self.slots, self.version, self.name = {}, {}, version, name
    def manifest(self):
        text = "".join("F %s\t%d\t%s\n" % (p, len(d), blake(d)) for p, d in sorted(self.files.items(), key=lambda x: x[0].encode()))
        text += "".join("S %d\t%s\n" % (n, blake(d)) for n, d in sorted(self.slots.items()))
        return text, blake(text.encode())

class FakeVita:
    """the Vita's service, in memory"""
    def __init__(self, L, code):
        self.L, self.code, self.saves, self.pairs, self.bases, self.guards, self.running = L, code, {}, {}, {}, {}, set()
        self.stage, self.commits, self.pending, self.sessions = {}, 0, [], []
        self.sock = socket.socket(); self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0)); self.sock.listen(4); self.port = self.sock.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()
    def serve(self):
        while True:
            c, _ = self.sock.accept(); threading.Thread(target=self.conn, args=(c,), daemon=True).start()
    def line(self, c):
        b = b""
        while not b.endswith(b"\n"):
            d = c.recv(1)
            if not d: raise ConnectionError
            b += d
        return b.decode().rstrip("\r\n")
    def conn(self, c):
        try:
            l = self.line(c)
            if l.startswith("NSYNC1 PAIR "): return self.pair(c, l[12:])
            _, _, pc_id, nonce = l.split(" ", 3)
            if pc_id not in self.pairs: c.sendall(b"NSYNC1 ERR unpaired\n"); return
            vn = secrets.token_bytes(16); c.sendall(("NSYNC1 HELLO %s\n" % vn.hex()).encode())
            ses = ctypes.create_string_buffer(80); self.L.nsync_session(ses, self.pairs[pc_id][0], bytes.fromhex(nonce), vn, 1)
            out = ctypes.create_string_buffer((128 << 10) + 64); inb = ctypes.create_string_buffer((128 << 10) + 64); plain = ctypes.create_string_buffer((128 << 10) + 64)
            while True:
                head = self.recv(c, 4); blen = int.from_bytes(head, "big"); body = self.recv(c, blen)
                ctypes.memmove(inb, body, blen); n = self.L.nsync_open(ses, plain, inb, blen)
                if n < 0: return
                msg = plain.raw[:n]; reply = self.handle(pc_id, msg)
                fl = self.L.nsync_seal(ses, out, reply, len(reply)); c.sendall(out.raw[:fl])
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            c.close()
    def recv(self, c, n):
        b = b""
        while len(b) < n:
            d = c.recv(n - len(b))
            if not d: raise ConnectionError
            b += d
        return b
    def pair(self, c, rest):
        pc_id, nonce, name = rest.split(" ", 2)
        vn = secrets.token_bytes(16); c.sendall(("NSYNC1 PAIR %s\n" % vn.hex()).encode())
        a = bytes.fromhex(self.line(c))
        p = ctypes.create_string_buffer(128); self.L.nsync_pair_start(p, bytes.fromhex(nonce), vn, self.code.encode(), secrets.token_bytes(32))
        mine = p.raw[96:128]
        pk, cf, tr = ctypes.create_string_buffer(32), ctypes.create_string_buffer(32), ctypes.create_string_buffer(96)
        if self.L.nsync_pair_finish(p, 1, a, pk, cf, tr): c.sendall(b"NSYNC1 ERR bad point\n"); return
        tag = ctypes.create_string_buffer(32); self.L.nsync_pair_tag(tag, cf, b"vita", tr)
        c.sendall((mine.hex() + "\n" + tag.raw.hex() + "\n").encode())
        theirs = bytes.fromhex(self.line(c)); expect = ctypes.create_string_buffer(32); self.L.nsync_pair_tag(expect, cf, b"pc", tr)
        if theirs != expect.raw: c.sendall(b"ERR wrong code\n"); return
        self.pairs[pc_id] = (pk.raw, name); c.sendall(b"OK 00112233aabbccdd Test Vita\n")
    def handle(self, pc, msg):
        line, _, body = msg.partition(b"\n"); a = line.decode().split(" ")
        cmd = a[0]
        ok = lambda payload=None: b"OK" + (b"\n" + json.dumps(payload).encode() if payload is not None else b"")
        if cmd == "HELLO": return ok({"protocol": 1, "console": "Test Vita", "account_id": "0807060504030201", "running": sorted(self.running)})
        if cmd == "LIST":
            ids = [a[1]] if len(a) > 1 and a[1] else sorted(self.saves)
            return ok({"games": [{"id": i, "name": self.saves[i].name if i in self.saves else "", "version": self.saves[i].version if i in self.saves else "",
                                  "save": i in self.saves, "base": self.bases.get((pc, i)), "guard": self.guards.get(i, "none")} for i in ids]})
        if cmd == "CHOICES":
            mine = [c for c in self.pending if c[0] == pc]; self.pending = [c for c in self.pending if c[0] != pc]
            return ok({"choices": [{"id": c[1], "choice": c[2]} for c in mine]})
        tid = a[1]; s = self.saves.get(tid)
        if cmd in ("MANIFEST", "GET", "COMMIT") and tid in self.running: return b"ERR busy"
        if cmd == "MANIFEST":
            text, h = s.manifest()
            return ok({"hash": h, "files": [{"path": p, "size": len(d), "hash": blake(d), "mtime": "2026-11-16 21:12"} for p, d in s.files.items()],
                       "slots": [{"slot": n, "hash": blake(d), "title": d[4:68].split(b"\0")[0].decode(), "subtitle": "", "detail": "", "time": "2026-11-16 21:12"} for n, d in s.slots.items()]})
        if cmd == "GET":
            path, off, n = a[2], int(a[3]), int(a[4])
            data = s.slots[int(path[5:])] if path.startswith("slot/") else s.files[path]
            return b"OK\n" + data[off:off + n]
        if cmd == "PUT":
            path, off, total = a[2], int(a[3]), int(a[4])
            st = self.stage.setdefault(tid, {}); cur = st.get(path, b"")[:off]
            st[path] = cur + body; return ok()
        if cmd == "COMMIT":
            h = a[2]
            if blake(body) != h: return b"ERR the manifest doesn't match its hash"
            st = self.stage.get(tid, {}); new = Save(s.version, s.name)
            for l in body.decode().splitlines():
                kind, rest = l[0], l[2:]
                if kind == "F":
                    p, size, fh = rest.split("\t"); d = st.get(p, s.files.get(p))
                    if d is None or blake(d) != fh: return b"ERR " + p.encode() + b" is neither staged nor here"
                    new.files[p] = d
                else:
                    n, sh = rest.split("\t"); d = st.get("slot/" + n, s.slots.get(int(n)))
                    if d is None or blake(d) != sh: return b"ERR slot"
                    new.slots[int(n)] = d
            if new.manifest()[1] != h: return b"ERR the save didn't read back as sent"
            self.saves[tid] = new; self.stage.pop(tid, None); self.bases[(pc, tid)] = h; self.commits += 1
            return ok({"backup": "20261120-000000"})
        if cmd == "BASE": self.bases[(pc, tid)] = a[2]; return ok()
        if cmd == "GUARD":
            kv = dict(l.split("\t", 1) for l in body.decode().splitlines() if "\t" in l)
            self.guards[tid] = kv.get("state", "none"); return ok()
        if cmd == "RELEASE": return ok()
        if cmd == "BEGIN":
            if tid in self.running: return b"ERR busy"
            self.sessions.append(("begin", tid)); return ok()
        if cmd == "END": self.sessions.append(("end", tid)); return ok()
        return b"ERR unknown request"

def slot(title):
    b = bytearray(SLOT); t = title.encode(); b[4:4 + len(t)] = t; return bytes(b)

def main():
    tmp = tempfile.mkdtemp(prefix="vitasync_test")
    L, cli = build(tmp)
    vita = FakeVita(L, "482913")
    prefs, backups, save_dir = os.path.join(tmp, "prefs"), os.path.join(tmp, "backups"), os.path.join(tmp, "savedata0")
    os.makedirs(prefs)
    env = dict(os.environ, VSYNC_PREFS=prefs, VSYNC_BACKUPS=backups, ATIV_VSYNC_PORT=str(vita.port))
    run = lambda *a: subprocess.run([cli] + list(a), env=env, capture_output=True, text=True, timeout=60).stdout.strip()
    def local(path): return open(os.path.join(save_dir, *path.split("/")), "rb").read()

    print("== pairing")
    check("didn't match" in run("pair", "127.0.0.1", "482914"), "a wrong code is refused")
    check(run("pair", "127.0.0.1", "482913") == "paired with Test Vita", "the right code pairs")
    check("connected; account 0102030405060708" in run("status"), "a session opens, and the account id reads the right way round")

    print("== the decisions and the copies")
    g = vita.saves["PCSE00640"] = Save("01.08", "Shovel Knight")
    g.files["saveData.bin"] = b"YCFS=1\nm_rPlayerName_1=MATT\n" * 500; g.files["sub/more.dat"] = os.urandom(300000); g.slots[0] = slot("Shovel Knight")
    check(run("check", "PCSE00640", "1.00", save_dir).startswith("skip: Version differs (Vita 1.08, Nativ 1.00)"), "a version difference copies nothing")
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("newer on the Vita"), "no save here: the Vita's is newer")
    check(run("pull", "PCSE00640", "1.08", save_dir) == "done", "pull")
    check(local("sub/more.dat") == g.files["sub/more.dat"] and local(".slot0") == g.slots[0], "the files (one bigger than a frame) and the slot arrive")
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("in sync"), "then in sync")
    with open(os.path.join(save_dir, "saveData.bin"), "ab") as f: f.write(b"played here\n")
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("newer here"), "a change here: this side is newer")
    check(run("push", "PCSE00640", "1.08", save_dir) == "done" and g is not vita.saves["PCSE00640"], "push")
    g = vita.saves["PCSE00640"]
    check(g.files["saveData.bin"].endswith(b"played here\n") and g.files["sub/more.dat"] == local("sub/more.dat"), "the Vita has this side's save")
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("in sync"), "then in sync")
    g.files["saveData.bin"] += b"played on the Vita\n"
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("newer on the Vita"), "a change there: the Vita's is newer")
    with open(os.path.join(save_dir, "saveData.bin"), "ab") as f: f.write(b"and here\n")
    out = run("check", "PCSE00640", "1.08", save_dir)
    check(out.startswith("conflict") and "16 Nov" in out and "Shovel Knight" in out, "changes on both sides: a conflict, with each side's time and slot text")
    os.remove(os.path.join(save_dir, ".slot0"))
    check(run("pull", "PCSE00640", "1.08", save_dir) == "done" and local("saveData.bin") == g.files["saveData.bin"] and local(".slot0") == g.slots[0], "keeping the Vita's: its save replaces this one")
    backed = [d for d in os.listdir(os.path.join(backups, "PCSE00640"))]
    check(len(backed) >= 1 and os.path.isfile(os.path.join(backups, "PCSE00640", sorted(backed)[-1], "saveData.bin")), "and this one was backed up first")
    vita.running.add("PCSE00640")
    check(run("check", "PCSE00640", "1.08", save_dir).startswith("skip: Playing on the Vita"), "a game running on the Vita: its save left alone")
    check("running on the Vita" in run("pull", "PCSE00640", "1.08", save_dir), "a fetch while the game runs there is refused")
    vita.running.discard("PCSE00640")
    vita.sessions.clear(); run("pull", "PCSE00640", "1.08", save_dir)
    check(vita.sessions == [("begin", "PCSE00640"), ("end", "PCSE00640")], "a fetch is one session: the Vita holds launches for all of it")
    check(run("check", "PCSA00999", "1.00", save_dir).startswith("skip: Not on the Vita"), "a game the Vita doesn't have")
    print("== choices made at the Vita while this computer was away")
    pc_id = [l.split("=", 1)[1] for l in open(os.path.join(prefs, "vita-sync.ini")).read().splitlines() if l.startswith("pc_id=")][0]
    g.files["saveData.bin"] += b"the Vita again\n"
    with open(os.path.join(save_dir, "saveData.bin"), "ab") as f: f.write(b"this side again\n")
    vita.pending.append((pc_id, "PCSE00640", "vita"))
    out = run("choices", "PCSE00640", "1.08", save_dir)
    check(local("saveData.bin") == g.files["saveData.bin"] and "In sync" in out, "keep the Vita's (chosen there): its save fetched at once")
    with open(os.path.join(save_dir, "saveData.bin"), "ab") as f: f.write(b"and here once more\n")
    vita.pending.append((pc_id, "PCSE00640", "pc")); vita.pending.append(("someone-else-00", "PCSE00640", "vita"))
    out = run("choices", "PCSE00640", "1.08", save_dir)
    check(vita.saves["PCSE00640"].files["saveData.bin"] == local("saveData.bin") and "In sync" in out, "use the PC's (chosen there): this save sent")
    check(vita.pending == [("someone-else-00", "PCSE00640", "vita")], "another computer's choice left for it")
    check("off" in run("off", "PCSE00640") and run("check", "PCSE00640", "1.08", save_dir).startswith("skip: Off"), "a game turned off is left alone")

    print("%s" % ("all passed" if not fails else "%d failed" % len(fails)))
    shutil.rmtree(tmp, ignore_errors=True)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
