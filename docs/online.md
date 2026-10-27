# Online play

The games keep their own online code. Sony's networking libraries run recompiled and unchanged, and Nativ provides the
network they expect: the matchmaking servers, the address lookups and the paths between players that PSN used to
provide. Sony's own servers are never contacted (`net_blocked` in `runtime/net/net.c`).

There are two ways to play:

- **Peer to peer**, the default today. Every copy of Nativ runs a small matchmaking service inside the game. The player
  who creates a room hosts it, and the other players' copies keep it in step.
- **A community server** that the player names (settings `matching_server`), such as one run by a revival project.
  For Killzone, the plan is for Mirage's server to become the default for public matches once its sign-in is settled,
  so ports and real Vitas share one player pool. Peer to peer then covers LAN, private and self-hosted play.

Two games play online: Killzone (its multiplayer matches and Botzone) and Unit 13 (two-player co-op).

## Playing online

**Play online** is one switch, shared by every game and on by default. It's in three places: the launcher's Settings
(Sound & online), its first-run setup next to your online name, and the F10 menu's Game page. It applies from the
game's next start, and only to games whose pack offers online play. Nothing else needs to be running: Killzone's
login server is answered inside the game itself (`packs/PCSA00107/server.c`), unless you name a private server. Unit 13
needs no game server at all. Its co-op is matchmaking plus a direct link between the two players, and the few settings
files it fetched from its maker's servers are answered inside the game too (`packs/PCSA00014/zipper.c`).

**Status.** The F10 Game page shows the live state: how many other copies are linked, the rooms open, your address as
the internet sees it, and whether your name clashes with another player's. While a game plays online it also writes
`online_status.txt` beside the shared settings every 5 seconds, and the launcher's Settings page shows "Online now"
from it.

**Firewalls.** A firewall that drops traffic it didn't ask for stops other players finding you on your local network.
On Linux with ufw, allow the two ports from your LAN:

```
sudo ufw allow from 192.168.0.0/24 to any port 36580:36581 proto udp comment 'Project Nativ online (LAN)'
```

Port 36580 carries LAN discovery and 36581 the links between copies. Internet play doesn't need this: there, both
sides open the path themselves.

On Windows, the firewall asks whether to allow each game the first time it goes online. Choose Allow, or other
players on your network can't reach that game. The rule belongs to that game's program file, so Killzone and Unit 13
each ask once.

**Data use** is small: about 1.5 MB for an 8-minute session, from boot through 2.5 minutes of a match.

Headless runs stay offline unless they ask for it (`RECOMP_ONLINE=1`, `RECOMP_ONLINE_OVERLAY=1`).

## How copies find each other

- **On the same network**, each copy announces its rooms by multicast (239.255.86.80, port 36580). Copies can also be
  told about each other directly (settings `online_peers`, `RECOMP_ONLINE_PEERS`), which helps when multicast is
  blocked.
- **On the internet**, copies meet through the public BitTorrent DHT. Each one announces itself under a hash of
  "Project Nativ online 1:" and the game's communication ID, then contacts the copies it finds. Both sides sending at
  once opens a path through most home routers (hole punching).
- **UPnP** (settings `online_upnp=1`, off by default) asks the router to forward the port, which makes a copy reachable
  without punching.
- **A relay** (`servers/nativ-node`) handles routers that won't let either side in; see below.

Some routers don't route traffic aimed at their own public address back inside the network; ours, a TP-Link, doesn't.
Two copies at home then link over the LAN instead of through the internet.

## The overlay

Every copy gets a virtual address in 198.18.0.0/15, and the game is told that address is its own. All of a copy's
online traffic goes through one UDP socket (settings `online_port`, default 36581), linked to each other copy:

- the game's peer-to-peer packets;
- a joining player's connection to the host's room server, carried as a reliable stream;
- in-game messages between players;
- discovery between linked copies.

LAN discovery and known peers only start the links. After that, everything runs over them.

## What's encrypted

Every install has its own key pair (X25519, kept as `online_key` beside the shared settings and made on first use). A
copy's identity on the overlay is a hash of its public key, so no copy can pose as another.

Two copies link with a handshake (`runtime/net/online/crypto.c`). Both sides contribute fresh temporary keys, the
session keys come from four key exchanges plus the handshake transcript, and each side proves who it is: the
responder in its reply, the initiator in its first sealed packet. From then on every packet is sealed with
XChaCha20-Poly1305 (Monocypher, in `third_party/monocypher`), with one key per direction, a counter as the nonce and a
64-packet window against replays.

That covers everything that travels over the overlay. Not encrypted:

- the LAN multicast announcements. With the overlay on, they only say where to start a handshake, and anything they
  claim is trusted only once it arrives over a link;
- play with the overlay turned off (settings `online_overlay=0`, LAN only);
- the DHT, which is public and sees your address announced under a hash of the game.

Linked players see each other's IP addresses, as in any peer-to-peer game; a pair playing through a relay sees the
relay's instead. Online names are chosen by players. When two copies claim the same name, the one with the lower
account ID keeps it, but that's a convention between honest copies, not a guarantee.

## The relay

`servers/nativ-node` is a headless member of the overlay, with its own key and no game, for players whose routers
won't let them reach each other.

1. Copies link to the node like to any other copy.
2. Every 5 seconds the node tells each copy about the others playing the same game, with their public addresses as the
   node sees them. The copies then try each other directly, all at once, which makes hole punching more likely to work.
3. If a pair still has no answer after 3 seconds, they also try through the node. A pair that only gets through there
   plays through it: each wraps its already-sealed packet in another packet sealed to the node, which passes it on.

The node sees who talks to whom and how much, but never what's said. It accepts at most 512 KB a second from one copy.
As soon as a direct path works, it wins over the relay. A datagram longer than about 1,390 bytes can't go through a
relay; it's logged once.

Copies find a node through settings `online_relays` (`<host>[:<port>]`, `RECOMP_ONLINE_RELAYS`) or through the DHT,
where a node announces itself under its game. Build it with `servers/nativ-node/build.sh` and run
`nativ-node [--port 36590] [--state DIR] [--game ID]`.

## Rooms and host handover

On PSN a room lived on Sony's server. Here it lives in one copy, its *authority*: the room server of the player who
created it. Without help, every other player would be cut off when that copy quit. So the room is shared:

- **Every member reaches the room through its own copy's proxy**, a local listener for that room. The proxy passes the
  game's messages through whole and keeps a copy of the room's state.
- **The authority sends each proxy a snapshot of the room** (its fields, key and members) at every change and every
  second. Every member's copy holds the room, and the snapshots double as a heartbeat.
- **When the authority goes**, because its connection ends or 5 seconds pass without a snapshot, every proxy picks the
  same successor: the room's owner if that wasn't the authority's player, otherwise the member in the lowest slot. The
  successor's copy starts serving the room, and every other proxy reconnects to it, proving membership with the room's
  key.
- **The games never notice the switch.** Their connections stay open, and each game hears what PSN would have sent:
  the departed member gone, and the new owner. A member that doesn't reconnect within 15 seconds counts as gone too.
- **A host that was only frozen and comes back** finds that the room moved on: rooms carry a generation number, and a
  copy that hears another serving its room at a later generation ends its own members' connections. Its player sees
  the matchmaking error a player cut off from PSN would have seen; everyone else plays on.

Handovers chain: if the new host leaves too, the next one takes over. Late joiners can join a match in progress after a
handover.

## How it's built

1. **The game's side: Sony's libraries, recompiled and unmodified.** `np_matching2` (rooms and signaling),
   `np_common` (its XML parser and timers), `libssl`, and `librudp` (game traffic). A game's pack lists them in its
   `MODULES`.
2. **The system layer** (`runtime/net/`):
   - `net.c`: sockets. The game's peer-to-peer sockets share one UDP port by virtual port number, as the Vita's
     UDPP2P does. A second copy on the same machine gets its own port offset.
   - `http.c`: HTTPS, the signed matching server list (pointing at the local service or a chosen server), and
     in-process answers for game servers.
   - `np.c`: identity and sign-in tickets.
3. **The online service** (`runtime/net/online/`): plain C with no access to guest memory. It serves the game's
   matchmaking library over local sockets like any server, so it builds and tests on its own.
   - `m2_wire.c`: Sony's matchmaking protocol: message header, typed fields, opcodes.
   - `m2_server.c`: the game's connection, sign-in, requests, and the room events pushed to the game.
   - `m2_rooms.c`: worlds, rooms, members, attributes and search, for the rooms this copy serves.
   - `m2_proxy.c`: the per-room proxies and host handover.
   - `discovery.c`: other copies' rooms and players, from LAN announcements and known peers.
   - `basic_msg.c`: in-game messages between players. Killzone sends one to ask a room's owner to let it in.
   - `stun.c`: the STUN server the matchmaking library asks for its own address (RFC 3489).
   - `overlay.c`: the links between copies: virtual addresses, handshakes, keepalives, datagrams, reliable streams, and
     the relay mode `nativ-node` runs in.
   - `crypto.c`: the install's key, session keys and sealing.
   - `dht.c`: the BitTorrent DHT client, used only to meet.
   - `upnp.c`: UPnP port mapping.
   - `status.c`: the status shown in F10 and written to `online_status.txt`.
4. **Game-specific servers live in the game's pack** and answer inside the game's process. Killzone's login, heartbeat,
   round and stats server is `packs/PCSA00107/server.c`; Unit 13's settings files are `packs/PCSA00014/zipper.c`. The same server also builds as its own program,
   `servers/kzserver`, for a community host or for tests (`KZSERVER=127.0.0.1:8443`).

Sign-in travels over TLS on port 443. For the local service, the host passes the library's SSL calls straight through;
for any other server, Sony's own libssl does real TLS.

## Killzone's join, step by step

1. **Search** (opcode `0x120c`): the service answers with this copy's rooms and the ones discovery has heard about.
2. **Ask the owner**, a Killzone-specific step: an in-game data message `{u16 0xcbc5, u16 2, request}` (type 0x8010) to
   the room's owner, whose game answers the same way. The game's handler for these is at offset 0x14 of its
   `SceNpBasicEventHandlers`.
3. **Join**: `0x1209` (locate the room) with the room ID, then `0x3201` on the owner's room server.
4. **Signaling**: offers and answers are relayed as room messages (`0x320a` then `0x320b`). The STUN server must also
   answer the NAT tests at its *changed address*, or the library never finishes. The peers' P2P sockets then echo each
   other, and both sides get event `0x5102` (connected). The signaling thread waits for `SceNetCtl_E8E8DF18` to report
   4 and opens a raw ICMP socket. Reporting NAT type 1 (open) keeps it off the STUN keepalive path.
5. **Game traffic**: librudp over the game's P2P socket (port 3658, virtual port 1). Killzone sizes its receive
   buffer with `sceNetGetSockInfo`.

Two copies on one machine: the second copy's P2P port moves from 3658 to 3659, and any fixed UDP port already taken
(the STUN socket's 10070, for example) moves to the next free one. The first copy's STUN responder answers both.

## Unit 13's co-op, step by step

1. **The Network Pass.** Unit 13's online modes need its Network Pass, an add-on. The game checks it by opening the
   add-on's `unlock.dat`, so the runtime makes that file (`RECOMP_ADDCONT_MARKERS` in the pack, `runtime/core/io.c`).
2. **Hosting** creates a room (`0x3200`) whose internal binary attribute carries the game's protocol hash. A joiner
   compares it with its own, so the room server must hand it back unchanged; an empty one reads as "A protocol
   mismatch occurred". The host then publishes the mission and game type as the room's external integers (`0x1207`),
   which the joiner's lobby list shows.
3. **Joining**: `0x3201`, then signaling in star mode with the host as the hub, as in Killzone.
4. **Game traffic** goes over plain sockets, not librudp: datagrams on the P2P socket (virtual port 20001) and a TCP
   stream (`SCE_NET_SOCK_STREAM_P2P`, port 20002). The host's game accepts the stream only from an address and port
   that signaling reported for a room member, and on a Vita that port is the peer's signaling port, not the stream's.
   So the connecting side's runtime first sends a 12-byte preamble: `NVS1`, its overlay address (0 on the same
   machine) and its signaling port. The accepting side reads it before handing the connection to the game, and reports
   that address and port (`runtime/net/net.c`, `stream_hello`).
5. **Player select.** Each player's loadout and ready state is that player's member internal binary attribute
   (`0x3206`). The host's DEPLOY counts the players whose record says ready. The matchmaking library updates even its
   own copy of the record only from the member-data event (`0x320d`), so the room server sends that event to every
   member, the sender included.

The matching server list, its signature and the library's first message are in `runtime/net/http.c`
(`matching_list`). Protocol details are mapped from `np_matching2` 3.65, the same build the game loads.

## Testing

The scripts in `dev/tests/` drive headless copies through the games' menus into a match:

- `online_duo.sh`: two copies on one machine. A hosts, B joins about 20 seconds later; 2 to 3 minutes a run.
- `online_lan.sh`: this machine hosts and a second one (`REMOTE`, an ssh host) joins; `SWAP=1` turns them round.
- `online_lobby.sh`: up to eight copies in one room. `YANK=lobby|match` removes the host (`YANK_HOW=kill|stop|blip`),
  `YANK2_AFTER` removes the next one too, `LATE=1` adds a late joiner, and `REMOTE=<ssh host>` puts a copy on a second machine.
- `u13_online_duo.sh`: two Unit 13 copies on one machine. A hosts mission 01, B finds it in the lobby and joins; a
  `DEPLOY` tap then takes both into the mission (pass it in `A_TOUCH`). Builds a second binary first
  (`RECOMP_BIN=a ./vitarecomp build PCSA00014`); `U13_NET_DIAG=1` in that build logs the game's matchmaking callbacks.
- `u13_online_lan.sh`: the same between this machine and `REMOTE`, as `online_lan.sh` (`SWAP=1` too). Both copies use
  this machine's Unit 13 save.

Useful switches: `RECOMP_ONLINE_NO_LAN=1` turns LAN discovery off, to force internet paths at home;
`RECOMP_ONLINE_NO_DIRECT=1` drops every packet that didn't come through a named relay, to test relaying;
`RECOMP_M2_LOG=2` logs handshakes.

## Known limits

- **Real Vitas** also expect Sony's encrypted UDPP2P framing between peers. Nativ-to-Nativ traffic uses our own, so
  playing alongside real Vitas goes through a community server like Mirage.
- **No default relay.** Two copies that can't reach each other directly (strict NATs on both sides) need a relay, and
  none is run for players: anyone can run `servers/nativ-node` and list it (settings `online_relays`).
- **Relayed datagrams** are limited to about 1,390 bytes.
- **The room server without the overlay** listens on every interface, so other machines' games can connect; it accepts
  only loopback and private-network addresses (`m2_server.c`, `local_peer`). With the overlay on (the default) it listens
  on loopback alone.
- **Unit 13**: voice chat and the leaderboards aren't there yet (their libraries answer as if empty), and the High
  Value Target and Daily Challenge modes haven't been tried online.
- **Names** aren't verified: the name-clash rule is a convention, not a check.
- **HUD elements missing** (match clock, minimap, score) appeared on the Linux copy in 2 of 3 early handover runs, and
  not since. It may be the same as the HUD flicker seen in normal play at 60 fps, which is still being investigated.
- **"The matchmaking service failed"** was seen once, on 2026-10-24, in a run where two players had the same name, a
  setup now refused. It hasn't appeared since.

## History

- **2026-10-24**: two copies on one machine play a match; then a Mac and a Linux PC on a LAN; the overlay; meeting
  through the DHT; eight copies in one room; host handover in the lobby and mid-match.
- **2026-10-25**: a match across the internet, one copy on a phone's hotspot; handover across the
  internet; encrypted links; the relay node; online play built into the normal Killzone build, with the Play online
  switch and status.
- **2026-10-26**: Unit 13 co-op: two copies on one machine play mission 01 together, then a Mac and a Windows PC on a
  LAN. Windows needed two fixes in `net.c`: the STUN server's loopback address (a `struct in_addr` initializer that
  Windows' union read as 127.0.0.0), and socket errors, which Winsock reports through `WSAGetLastError()`, not `errno`.
