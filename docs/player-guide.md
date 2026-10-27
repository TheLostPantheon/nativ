# Player guide

Everything you need to play your PS Vita games with Project Nativ: what to bring, how to install it, the first run, the
controls, online play, and what to do when something goes wrong.

## What you need

- **A computer:** a Mac with Apple silicon (M1 or later) on macOS 13 or newer, a Windows 10 or 11 PC on x86-64, or a
  Linux PC on x86-64, including a Steam Deck.
- **Your own copies of your games**, with their updates if you have them. Nativ takes game folders copied from your
  Vita, PKG files, or a whole folder to search. See [Supported games](../README.md#supported-games) for exactly which
  releases play.
- **Their licences.** A game folder copied from your Vita carries its own. For PKGs, Nativ fetches the licence from your
  Vita over Wi-Fi, once.
- **The PS Vita system software update file**, free from Sony's website. Nativ uses the system libraries inside it and
  builds every game against them.

Nothing you add leaves your computer, and Nativ itself contains no Sony or game code.

## Installing

**On a Mac**

1. Unzip the download and move the `ProjectNativ` folder wherever you like, such as your Applications folder.
2. Tell macOS you trust it. Nativ isn't notarised by Apple yet, so macOS blocks a downloaded copy. Open Terminal, type
   `xattr -dr com.apple.quarantine ` (with the space at the end), drag the `ProjectNativ` folder into the window, and
   press Return. You only do this once per download.
3. Double-click **Project Nativ.app**.
4. If your Mac doesn't have Apple's free Command Line Tools, Nativ asks you to install them. Nativ uses them to build
   each game on your Mac. Install them from the window that opens, then start Nativ again.

**On Windows**

1. Unzip the download anywhere you like, outside `Program Files`.
2. Double-click **Project Nativ.exe**. Nativ isn't signed yet, so Windows may say it protected your PC: choose
   **More info**, then **Run anyway**. You only do this once per download.

**On Linux**

1. Unzip the download anywhere you like.
2. Run `Project Nativ.sh`. On a Steam Deck, add it to Steam as a non-Steam game and start it from Game Mode.

Nativ keeps your games, builds and saves in its library, a folder apart from the install, so replacing the install never
touches them: `~/Library/Application Support/ProjectNativ/Nativ/library` on a Mac,
`%LOCALAPPDATA%\ProjectNativ\Nativ\library` on Windows and `~/.local/share/ProjectNativ/Nativ/library` on Linux.

**Updating:** when a new version is out, the launcher says so above your library, with a link to its download page.
Replace the `ProjectNativ` folder with the new one; your library stays where it is. On a Mac, run the Terminal command
from step 2 on the new folder. Each game rebuilds once with the new version the next time you play it.

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

## Building and playing

The first time you play a game, Nativ builds it into a native program on your computer. That takes a few minutes:
about two for Shovel Knight, and longer for the bigger games, Killzone the longest. After that it starts straight away.

After you update Nativ, the next Play brings that game's build up to date before it starts. That's usually under a
minute; when an update changes how games are translated, it's as long as the first build.

The first few minutes of play can stutter while Nativ prepares the game's shaders for your GPU. It keeps them, so
later sessions run smoothly.

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

## Settings

**Settings**, at the top right of the launcher, holds the display, controls and sound for every game. Each game's card
has a menu with that game's own options. Some worth knowing:

- **Render scale**: draws the game at up to six times the Vita's resolution.
- **Aspect ratio**: native, 21:9, 32:9 or your window's shape. The HUD keeps its own shape on wide screens.
- **Frame rate**: 30 or 60. 60 is experimental: parts of a game timed in fixed 1/30 second steps can run fast.
- **Field of view** (Killzone, Unit 13 and Uncharted).

## Online play

Killzone: Mercenary plays online (multiplayer and Botzone), and Unit 13 plays two-player co-op. The **Play online**
switch is on by default, in Settings, the first-run setup and the F10 menu's Game page. Unit 13's Network Pass counts
as owned.

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
