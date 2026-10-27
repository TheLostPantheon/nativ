#!/usr/bin/env python3
"""kzserver: a stand-in for Killzone: Mercenary's game server (kzv.online.scee.com, shut down by Sony) -- enough of it for
Project Nativ's port to play Botzone locally, written from what the game's own client sends and parses (eboot 0x815a5xxx:
the request builders and the SAX handlers that read the replies). Nothing here comes from Sony's server or anyone
else's revival server.

The port reaches it by redirecting the game's host name (the host's RECOMP_NET_SERVERS + settings "private_server", or
RECOMP_NET_REDIRECT=kzv.online.scee.com=127.0.0.1:8443). HTTPS with a self-signed certificate made on first run: the host
does not verify a redirected server's certificate.

What the game does, as far as it's known (every request is a POST of an application/xml body, User-Agent killzonengp/469,
and after login carries the x-auth-token header):
  /user/login                   <xmlLoginRequest timestamp machineId language application="kzvita" buildNumber ticket=
                                base64 NP ticket/>; the reply's elements, matched by name wherever they are:
                                userProfile(valour, country, region) owner(uid) friend(name, uid)* authToken(text)
                                environmentId(text) disabledServices(text)* event(eventDefinitionId) attribute(name, value)*
  /user/heartbeat               xmlHeartbeatRequest(activity: PlayingCampaign | PlayingMultiplayer | InFrontend)
  /games/createmultiplayergame  xmlCreateOnlineGameRequest(private, mapId, gameMode, expiry; player(uid, name)) ->
                                xmlCreateOnlineGameResponse(gameId attribute)
  /games/joinmultiplayergame    xmlJoinOnlineGameRequest(game) -> xmlResult
  /user/statUpdate              xmlStatUpdate(timestamp, name; values/entry(key, value)*) -> xmlResult
  /games/postbatch              xmlPostOfflineScoresRequest
  /roundhistory/<kind>/append/<id>  a binary blob (the round's scores) -> xmlResult
  /user/leaderboard/overall/... xmlLeaderboardRequest
  /partyGathering/refresh|clear xmlPartyGathering*Request
Anything not handled yet is logged in full and answered 404, which the game reports as the server rejecting it.

  kzserver.py [--listen 127.0.0.1:8443] [--state DIR] [--dump DIR]
"""
import argparse, base64, hashlib, json, os, secrets, ssl, struct, subprocess, sys, threading, time
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

XML_HEAD = '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'


def ticket_fields(raw):
    """An NP ticket (v2.1 or 3.0: 8-byte header, then typed fields {u16 type, u16 length, data}, blobs nesting; 3.0's
    date-of-birth blob 0x3011 holds raw bytes) -> the body's fields in order, as (type, bytes)."""
    out = []

    def walk(b):
        i = 0
        while i + 4 <= len(b):
            t, n = struct.unpack_from(">HH", b, i)
            d = b[i + 4:i + 4 + n]
            if t & 0xff00 == 0x3000 and t != 0x3011:
                walk(d)
            else:
                out.append((t, d))
            i += 4 + n
    walk(raw[8:])
    return out


def ticket_identity(ticket_b64):
    """(online id, NP user id) from a login ticket: the body's fields run serial, issuer, issued, expires, user id (u64),
    online id (0x20 bytes, NUL padded) -- the order every v2.x ticket has."""
    try:
        f = ticket_fields(base64.b64decode(ticket_b64))
        uid = int.from_bytes(f[4][1], "big") if len(f) > 4 and len(f[4][1]) == 8 else 0
        oid = f[5][1].split(b"\0")[0].decode("ascii", "replace") if len(f) > 5 else ""
        return oid or "player", uid
    except Exception:
        return "player", 0


class State:
    """Players by online id, kept in <state>/players.json: their server uid, valour and session token."""

    def __init__(self, path):
        self.path, self.lock = path, threading.Lock()
        try:
            self.players = json.load(open(path))
        except (OSError, ValueError):
            self.players = {}
        self.tokens = {}

    def save(self):
        tmp = self.path + ".tmp"
        json.dump(self.players, open(tmp, "w"), indent=1)
        os.replace(tmp, self.path)

    def login(self, online_id, np_uid):
        with self.lock:
            p = self.players.get(online_id)
            if not p:
                uid = hashlib.sha1(online_id.encode()).hexdigest()[:16]
                p = self.players[online_id] = {"uid": uid, "np_uid": np_uid, "valour": 0, "country": "us", "region": 0}
            token = secrets.token_hex(16)
            self.tokens[token] = online_id
            p["last_login"] = int(time.time())
            self.save()
            return p, token

    def by_token(self, token):
        with self.lock:
            oid = self.tokens.get(token)
            return (oid, self.players.get(oid)) if oid else (None, None)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "kzserver/0.1"

    def log_message(self, fmt, *a):
        pass

    def reply(self, status, body=b"", ctype="application/xml"):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)
        self.server.note(f"   -> {status} {len(body)} bytes")
        if body and self.server.dump:
            self.server.note("      " + body.decode("utf-8", "replace")[:600])

    def do_GET(self):
        self.handle_any(b"")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        self.handle_any(self.rfile.read(n) if n else b"")

    def handle_any(self, body):
        srv = self.server
        path = self.path.split("?")[0]
        token = self.headers.get("x-auth-token", "")
        srv.note(f"{time.strftime('%H:%M:%S')} {self.command} {self.path} ({len(body)} bytes{', token ' + token[:8] if token else ''})")
        srv.dump_request(self, body)
        try:
            doc = ET.fromstring(body) if body.strip() else None
        except ET.ParseError:
            doc = None
        route = {"/user/login": self.login, "/user/heartbeat": self.heartbeat, "/games/createmultiplayergame": self.create_game,
                 "/games/joinmultiplayergame": self.join_game, "/user/statUpdate": self.stat_update}.get(path)
        if route:
            return route(doc, token)
        if path.startswith("/roundhistory/"):   # the round's history, a binary blob (application/octet-stream): kept as received
            oid, _ = self.server.state.by_token(token)
            srv.note(f"   round history {path.rsplit('/', 1)[-1]}: {len(body)} bytes from {oid or '?'}")
            return self.reply(200, f"{XML_HEAD}<xmlResult/>".encode())
        srv.note("   (not handled yet) headers: " + "; ".join(f"{k}: {v}" for k, v in self.headers.items()))
        if body:
            srv.note("   " + body.decode("utf-8", "replace")[:2000])
        self.reply(404)

    def login(self, doc, token):
        if doc is None or doc.tag != "xmlLoginRequest":
            return self.reply(400)
        oid, np_uid = ticket_identity(doc.get("ticket", ""))
        p, tok = self.server.state.login(oid, np_uid)
        self.server.note(f"   login: {oid} (build {doc.get('buildNumber')}, {doc.get('language')}) -> uid {p['uid']}")
        x = (f'{XML_HEAD}<xmlLoginResponse>'
             f'<userProfile valour="{p["valour"]}" country="{p["country"]}" region="{p["region"]}"/>'
             f'<owner uid="{p["uid"]}"/>'
             f'<authToken>{tok}</authToken>'
             f'<environmentId>{self.server.environment}</environmentId>'
             f'</xmlLoginResponse>')
        self.reply(200, x.encode())

    def heartbeat(self, doc, token):
        oid, _ = self.server.state.by_token(token)
        self.server.note(f"   heartbeat: {oid or '?'} {doc.get('activity') if doc is not None else ''}")
        self.reply(200, f"{XML_HEAD}<xmlResult/>".encode())


    def create_game(self, doc, token):
        # xmlCreateOnlineGameRequest {private, mapId, gameMode, expiry; player(uid, name)*} -> the round's id, read as the reply
        # element's gameId attribute (eboot 0x815ddbbc, the start-element handler)
        oid, _ = self.server.state.by_token(token)
        gid = secrets.token_hex(8)
        self.server.note(f"   game round {gid}: {doc.get('gameMode') if doc is not None else '?'} on {doc.get('mapId') if doc is not None else '?'}"
                         f"{' (private)' if doc is not None and doc.get('private') == 'true' else ''}, host {oid or '?'}")
        self.reply(200, f'{XML_HEAD}<xmlCreateOnlineGameResponse gameId="{gid}"/>'.encode())

    def join_game(self, doc, token):   # xmlJoinOnlineGameRequest {game}: acknowledged
        oid, _ = self.server.state.by_token(token)
        self.server.note(f"   joins game round {doc.get('game') if doc is not None else '?'}: {oid or '?'}")
        self.reply(200, f"{XML_HEAD}<xmlResult/>".encode())

    def stat_update(self, doc, token):   # xmlStatUpdate {timestamp, name; values}: the game's own reports (errors, telemetry)
        oid, _ = self.server.state.by_token(token)
        vals = {e.findtext('key'): e.findtext('value') for e in doc.iter('entry')} if doc is not None else {}
        self.server.note(f"   stat {doc.get('name') if doc is not None else '?'} from {oid or '?'}: " + ", ".join(f"{k}={v}" for k, v in vals.items() if v)[:400])
        self.reply(200, f"{XML_HEAD}<xmlResult/>".encode())


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, state, dump, environment):
        super().__init__(addr, Handler)
        self.state, self.dump, self.environment = state, dump, environment
        self.seq, self.lock = 0, threading.Lock()

    def note(self, s):
        with self.lock:
            print(s, flush=True)

    def dump_request(self, h, body):
        if not self.dump:
            return
        with self.lock:
            self.seq += 1
            n = self.seq
        with open(os.path.join(self.dump, f"req_{n:03d}.txt"), "wb") as f:
            f.write(f"{h.command} {h.path}\n{h.headers}".encode() + body)


def ensure_cert(d):
    crt, key = os.path.join(d, "cert.pem"), os.path.join(d, "key.pem")
    if not (os.path.exists(crt) and os.path.exists(key)):
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650", "-subj",
                        "/CN=kzv.online.scee.com", "-keyout", key, "-out", crt], check=True, capture_output=True)
    return crt, key


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--listen", default="127.0.0.1:8443")
    ap.add_argument("--state", default=os.path.expanduser("~/.local/share/ativ/kzserver"))
    ap.add_argument("--dump", default=None, help="write each request to this directory")
    ap.add_argument("--environment", default="1", help="the environmentId the login hands out")
    a = ap.parse_args()
    os.makedirs(a.state, exist_ok=True)
    if a.dump:
        os.makedirs(a.dump, exist_ok=True)
    host, port = a.listen.rsplit(":", 1)
    srv = Server((host, int(port)), State(os.path.join(a.state, "players.json")), a.dump, a.environment)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(*ensure_cert(a.state))
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    print(f"kzserver on https://{a.listen} (state {a.state})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
