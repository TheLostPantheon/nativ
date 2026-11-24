# Player guide

Everything you need to play your PS Vita games with Project Nativ: what to bring, how to install it, the first run, the
controls, online play, and what to do when something goes wrong.

## What you need

- **A computer:** a Mac with Apple silicon (any M-series) on macOS 11 Big Sur or newer, a Windows 10 or 11 PC on x86-64, or a
  Linux PC on x86-64, including a Steam Deck.
- **Your own copies of your games**, with their updates if you have them. Nativ takes game folders copied from your
  Vita, PKG files, or a whole folder to search. See [Supported games](../README.md#supported-games) for exactly which
  releases play. Other games can be tried as experimental ones (see [Other games](#other-games)).
- **Their licences.** A game folder copied from your Vita carries its own. For PKGs, Nativ fetches the licence from your
  Vita over Wi-Fi, once.
- **The PS Vita system software update file**, free from Sony's website. Nativ uses the system libraries inside it and
  builds every game against them.

Nothing you add leaves your computer, and Nativ itself contains no Sony or game code.

## Installing

**On a Mac**

1. Open the `.dmg` and drag **Project Nativ** into Applications.
2. Open it from Applications. macOS asks once whether to open an app downloaded from the internet: choose **Open**.
3. If your Mac doesn't have Apple's free Command Line Tools, Nativ asks you to install them. Nativ uses them to build
   each game on your Mac. Install them from the window that opens, then open Nativ again.

**On Windows**

1. Unzip the download anywhere you like, outside `Program Files`.
2. Double-click **Project Nativ.exe**. Nativ isn't signed yet, so Windows may say it protected your PC: choose
   **More info**, then **Run anyway**. You only do this once per download.

**On Linux and Steam Deck**

1. Put the `.AppImage` anywhere you like and allow it to run as a program: in the file manager, its Properties ›
   Permissions (or `chmod +x` on it).
2. Open it. On a Steam Deck, in Desktop Mode, add it to Steam as a non-Steam game, then start it from Game Mode.

Nativ keeps your games, builds and saves in its library, a folder apart from the install, so replacing the install never
touches them: `~/Library/Application Support/ProjectNativ/Nativ/library` on a Mac,
`%LOCALAPPDATA%\ProjectNativ\Nativ\library` on Windows and `~/.local/share/ProjectNativ/Nativ/library` on Linux.
To put it on another drive, use **Settings**, **Library**, **Move it…**. On Windows, **Make this install portable** keeps the
library and your settings inside the `ProjectNativ` folder instead, so the whole folder can move to another drive or PC.

**Updating:** when a new version is out, the launcher says so above your library, with a link to its download page.
On a Mac, drag the new Project Nativ into Applications and replace the old one; on Windows, replace the `ProjectNativ`
folder with the new one; on Linux, the `.AppImage` with the new one. Your library stays where it is. Each game rebuilds once with the new version the next time
you play it.

## The first run

After a welcome screen, the launcher walks you through four steps. You can come back to them any time through
**Setup**, at the top right.

1. **Firmware.** Download the PS Vita system software update file (`PSVUPDAT.PUP` or `PSP2UPDAT.PUP`, version 3.74;
   any recent version works) from Sony's page, which the launcher links to, then choose the file. Nativ decrypts it
   once.
2. **Games.** Add your games and their updates. Each one is decrypted and checked against its game pack, which knows
   which versions it plays. Licences are kept once, so later updates and add-ons of the same game just add.
3. **Online name.** The name other players see when you play online. It shows as `ntv_` plus your name, so they know
   you're playing on PC. Up to 10 letters, digits, `-` or `_`.
4. **Done.** Your library opens.

## Other games

The library has two sections. **Enhanced with Nativ** holds the supported games: each has fixes and extras made for it,
and is tested with every release. **Other games** holds everything else you add, built from your copy as experimental
games.

- **A game Nativ doesn't support yet** gets a pack made from your copy: its main program and the Sony libraries it
  uses, with no game-specific work. When adding from a folder search, tick **Build experimental games too** to build
  them; otherwise they're only added, and the card's **Build** builds them later.
- **A release or version an Enhanced game's pack wasn't made for** (another region, an update Nativ doesn't play yet)
  shows **Try it anyway** on its card. It then plays without that game's fixes and extras.

An experimental game may not start, may show graphics or sound problems, or may stop partway. When it stops at code
its build missed, Nativ builds it again with that code and starts it again, a few times over. There's no support for
these games, but a report zip (the card's **...** menu, **Report a problem**) in a GitHub issue shows what a game is
missing.

## Building and playing

The first time you play a game, Nativ builds it into a native program on your computer. That takes a few minutes:
about two for Shovel Knight, and longer for the bigger games, Killzone the longest. After that it starts straight away.

After you update Nativ, the next Play brings that game's build up to date before it starts. That's usually under a
minute; when an update changes how games are translated, it's as long as the first build.

The first time you start a game, Nativ may pause it on a "Preparing shaders" card while it builds the game's shaders
for your GPU. Killzone takes a few minutes; the other games take seconds and usually don't show the card. You can skip
it and play at once: the shaders keep building while you play, with short pauses until they're done. Later starts are
quick. The card comes back only when there's something new to build, such as after an update.

## Controls

| Vita | Keyboard and mouse | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad |
| Cross, Circle, Square, Triangle | Space, C, F, E | Cross, Circle, Square, Triangle (Xbox: A, B, X, Y) |
| Start, Select | Enter, Tab | Options, Share or Create (Xbox: Menu, View) |
| L, R | Right and left mouse buttons (or Q and R) | L2, R2 (Xbox: LT, RT) |
| Sticks | WASD, and the mouse | Sticks |
| Front touch panel | Hold Alt (Option on a Mac) and click or drag. Where the mouse doesn't aim, a plain click works too. | The touchpad, or hold L1 and steer a cursor with a stick, touching with Cross |
| Rear touch panel | Right mouse button, the same way | The touchpad with R1 held, or hold R1 and use a stick and Cross |
| Motion sensor | | A gamepad's gyro, such as a DualShock 4's or DualSense's; R3 recentres it |

On a touchscreen, touch the picture: that's the front panel. Hold R1 as a finger lands for the rear panel.

Buttons are named as on a PlayStation controller; on an Xbox controller, L1 and R1 are LB and RB, and Cross is A.

**L and R buttons** (Controls, in the menu and the launcher's Settings) swaps the shoulder pairs: L and R on the bumpers
(L1 and R1), and the touch cursor on the triggers.

**Changing the buttons.** The menu's Controls page (F10, or the Guide button) lists every Vita button with the key and
the controller button that press it. Choose Change next to one and press the new key or button; Esc cancels. A key or
button that already did something else is taken from it, and that one shows "none" until you set it again. The triggers
count as buttons, and the controller column uses your controller's own names (A, B, X, Y and LB, RB on an Xbox
controller). The same table has the touch cursor's two buttons, which are the shoulder pair that isn't L and R until you
change them, and L3 and R3, which only a PS TV layout uses (R3 also recentres the gyro). F10 and the Guide button
always open the menu and can't be bound. Reset keys to the defaults and Reset controller buttons put the originals back.

The buttons are the same in every game. Turn on **Different buttons for this game** to give one game its own keys and
controller buttons, starting from the ones you have now; turning it off goes back to the shared set. The launcher's
Settings > Controls shows what has been changed and can reset it; the buttons themselves are changed in a game.

**Controller layout** (Killzone 1.12): the game decides as it starts whether it plays with its touch controls, as on a
handheld, or with its own PS TV layout, which puts every touch action on buttons and sticks, and keeps that until you
quit. The **Standard** layout, the game's touch controls, is the default and works with keyboard, mouse and controller
(its touch cursor). With a controller connected, a card at the start offers the **PS TV** layout; tick **Remember my
choice** to skip the card next time. The PS TV layout is for a controller only: keyboard and mouse don't play the game in
it. In it the shoulder pair that isn't L and R becomes the game's L2 and R2, and the sticks click as L3 and R3. Change it
in the menu: Game > Controller layout (Ask at start, Standard or PS TV), from the next start.

The first time you start a game, a card shows how to touch the screen with a keyboard and mouse, a controller or a
touch screen, with the one you're using highlighted. Touch once the way it shows to carry on (Esc skips it). It
appears only once.

**Keys while playing:**

- **F10**, or the PS button: the in-game menu, with display, controls, frame rate and field of view. The game
  pauses while it's open.
- **F1**: the frame-rate overlay.
- **F2**: cycle the render scale (sharper picture, more GPU work).
- **F5**: mark a moment for a bug report (see below).
- **F7**: switch the camera's light on or off, for Uncharted's hold-it-to-the-light puzzles.
- **F8**: switch direct mouse aim on or off (the mouse turns the game's camera itself, rather than acting as a stick).
- **F12**: capture a frame's graphics, for graphics bugs.

Closing the game's window (Cmd+W or Cmd+Q on a Mac, Alt+F4 elsewhere, or its close button) asks first, so a stray
shortcut doesn't lose your progress: **Keep playing** or **Quit**, and closing it again quits.

## Settings

**Settings**, at the top right of the launcher, holds the display, controls and sound for every game. Each game's card
has a menu with that game's own options. Some worth knowing:

- **Render scale**: draws the game at up to six times the Vita's resolution.
- **Aspect ratio**: native, 21:9, 32:9 or your window's shape. The HUD keeps its own shape on wide screens.
- **Frame rate**: 30 or 60. 60 is experimental: parts of a game timed in fixed 1/30 second steps can run fast.
- **Frame generation** (off by default): on a display that refreshes faster than the game draws (a 30 fps game on a 60 Hz
  screen, or 60 fps on 120 Hz), it shows a frame made between each two of the game's, for smoother motion. The frames
  are made from the finished pictures, so it works in any game, and a still HUD stays sharp; where the renderer can tell
  how the game's 3D scene moved, that motion is used too, which keeps turns and flat ground cleaner. The game's own frames
  show a little later (17 ms on a 60 Hz screen), so leave it off where reactions matter most. Fast motion, or something
  appearing suddenly, can leave a faint smear in a made frame, and a thin thing moving fast (a pole in a quick turn) can
  flicker. The frame rate counters show the game's own frames, with the frames shown beside them ("frame gen 120").
- **Language** (a game's settings, and the menu's Game page): the language the game is told the system speaks, from the
  ones that game has been checked in (Uncharted: English and Spanish). It applies the next time the game starts.
  Automatic is English.
- **Field of view** (Killzone, Unit 13 and Uncharted).
- **QTE time** (Killzone): how long you get for the swipe in a takedown or an interrogation, from 1 second (what the
  game's own slow motion gives) to 3. It's the same at 30 and 60 fps.
- **Stick deadzone** (Controls): how far a controller's stick moves before the game sees it. Raise it if the camera or
  your character drifts with the stick at rest.
- **Library** (Settings): where your library is, and **Move it…** to put it somewhere else. Moving it to another drive
  copies everything first and checks the copy before deleting anything. **Launcher size** makes the launcher's text and
  controls larger or smaller.

## Online play

Killzone: Mercenary plays online (multiplayer and Botzone), and Unit 13 plays two-player co-op. The **Play online**
switch is on by default, in Settings, the first-run setup and the F10 menu's Game page. Unit 13's Network Pass counts
as owned. If you haven't chosen an online name, Nativ asks for one the first time you start a game that plays online.

- **On the same network**, copies of Nativ find each other by themselves. If one computer has a firewall that blocks
  incoming traffic, allow UDP ports 36580 and 36581 from your local network. On Linux with ufw:
  `sudo ufw allow from 192.168.0.0/24 to any port 36580:36581 proto udp`. On Windows, choose **Allow** when the
  firewall asks about a game the first time it goes online.
- **Over the internet**, copies meet through the public BitTorrent DHT and connect directly, with no setup.
- **Starting a match:** if two of you search at the same moment, you can both end up hosting. Let one person start
  the match, then join from the others.
- **Unit 13 co-op:** both players open ONLINE COOPERATIVE. One chooses HOST CO-OP and a mission; the other presses
  REFRESH until that game is listed and picks it. Once both operatives show, the host's DEPLOY starts the mission.
  Voice chat and the leaderboards don't work yet.
- **Some networks can't connect.** Behind some mobile, carrier or university networks, two copies can't reach each other
  directly, and Nativ has no relay of its own yet. Playing on the same network always works.
- **What other players see:** your online name and your computer's IP address, as in any peer-to-peer game. Copies
  that meet over the internet also list their address in the public BitTorrent DHT. Names aren't verified, so anyone
  can call themselves anything.
- **Real Vitas:** playing alongside Vitas on the Mirage community server is planned.

The F10 menu's Game page shows whether you're linked to other players.

## Saves

Each game's saves live in your library, at `decrypted/<title id>/recomp/fs/savedata0/`. Nativ keeps them when you
update. Copy that folder somewhere safe to back them up.

## Vita sync

Nativ can keep a game's saves the same on your PS Vita and here, so you can play on either and carry on on the other.
Your Vita needs the Nativ Sync app (from VitaDB), and both need to be on the same network.

1. On the Vita, open Nativ Sync and choose **Pair with a computer**. It shows a six-digit code.
2. In Nativ, open **Settings**, **Vita sync**, **Find my Vita**, pick your Vita and type the code.

From then on a game syncs before it starts and after it closes, here and on the Vita. When both sides have new
progress, Nativ asks which save to keep, showing each one's time and the game's own description of it, and the Vita
asks the same when you start the game there. The save you don't keep is backed up: Nativ's in
`backups/vita-sync/<title id>/` in your library, the Vita's on the Vita. A save goes to the same version of its game or
a newer one, never an older one: an update reads the saves before it, but an older version may not read a newer one's.
So with an update on one side only, saves still move from the older side to the updated one. A game syncs once it has a
save on the Vita (play it there once). Settings, Vita sync turns it off for one
game or all of them.

## Reporting a bug

1. When something goes wrong, press **F5** straight away. That marks the moment in the log and saves pictures of the
   last three seconds of frames.
2. Note what you were doing.
3. Back in the launcher, open the game's **⋯** menu and choose **Report a problem**. Nativ gathers the game's logs from
   its last two sessions, the launcher's log, your settings and the pictures from your last F5 into one zip, then shows
   it in its folder. It holds no game files, saves or keys, and your home folder's name is left out of the logs.
4. Send the zip with your note. Without F5 pictures it's well under a megabyte; with them, up to a few tens of megabytes,
   too big for some chat apps, so share it through a file host.

## Known issues

- **Killzone: Mercenary**
  - At 60 fps, the HUD (map, ammo, score) sometimes flashes out for a moment.
  - Now and then a character's line plays twice.
- **Uncharted: Golden Abyss**
  - In cutscenes, voices run about a third of a second ahead of the lips.
- **All games**
  - 60 fps is experimental.
  - The first build of each game takes minutes.
  - A Linux game folder on another drive needs that drive mounted before you play. On a drive shared with Windows,
    shut Windows down fully (not Fast Startup) so Linux can mount it.

## Troubleshooting

- **macOS says the app can't be opened, or is damaged.** Run the Terminal command from Installing, step 2. Or open
  System Settings, then Privacy & Security, and choose **Open Anyway** beside Project Nativ.
- **A game won't build.** On a Mac, check that the Command Line Tools are installed: run `xcode-select --install` in
  Terminal. Send `session.log` if it still fails.
- **The game window closes as it starts.** Check that the game's files are still where you added them from, for example
  a drive that's no longer connected or mounted.
- **No other players online.** Check the Play online switch, and the firewall note above.
- **Windows says Smart App Control blocked a game.** Windows judges each new program once, and every game Nativ builds is
  new to it, so now and then it refuses one. Choose **Rebuild and try again** in the dialog Nativ shows: the rebuilt game
  is a new file, and Windows checks it afresh. If that's refused too, turn Smart App Control off in Windows Security, under
  App & browser control, then Smart App Control settings. On most versions of Windows 11 it can't be turned back on
  without reinstalling Windows.
