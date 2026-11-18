# Arisen Together

Online co-op for **Dragon's Dogma: Dark Arisen** on PC: 2 to 4 players, each on their own PC with their own
character, together in the host's world.

> **Alpha.** Keep copies of your saves (the launcher makes one at every launch).

## What it does

- **Up to four Arisen.** The host plays their game; up to three friends join with their own characters, each one
  taking a pawn's place in the host's party.
- **A shared world.** Everyone goes where they want. Quests, gear, loot and experience are shared; what a guest
  earns is added to their own save afterwards.
- **The same world on every screen.** Monsters, human foes and townsfolk are where the host's game has them, with
  the same health, the same deaths, and the same actions (see below).
- **Playing together.** Revive a downed friend, grab each other, give items, watch cutscenes together, move crates
  and barrels that everyone sees.
- **Steam or GOG.** Over Steam, friends find each other with no address to set. GOG, and Steam players who want it,
  join by IP address (local network or UDP port 27101).
- **A launcher** that finds the game, backs up your saves, installs the mod, shows your party and connection, and
  keeps a log to send when something goes wrong. In English, French, Italian, German and Spanish.

## Monsters: a different approach

Arisen Together grew from [DDDA Coop](https://github.com/clepix21/ddda-coop) by clepix21 and keeps its core:
the network, the shared world, the launcher's bones. Where it goes its own way is the monsters. The goal is that
every player meets **the same monsters, doing the same things, at the same time**.

DDDA Coop pairs each monster on a guest's screen with the host's monster of the same kind nearby, and from its
0.10.2 with the spawn entry both were placed from, read once after they appear. Each game still spawns its own
monsters, and the guest's monster replays the host's action numbers while its own AI keeps running.

Arisen Together starts earlier, from how the game spawns monsters:

- **The same monsters appear for everyone.** The game's random draws that decide which monsters appear
  (Bitterblack Isle's spawn lottery and its stage variants, respawns) are made once, by the first player to reach a
  place, and every other game uses that result. The game leading a group of players decides which monsters exist
  around them; the other games create the same ones and remove any extra, so a monster never exists on one screen
  only.
- **An exact ID for every monster.** Each monster is known by its stage, map tile, group and spawn entry, the same on
  every PC, and read again every frame (the game hands monsters between entries long after they appear). Parts get
  IDs of their own (the Gazer's tentacles, heads, cut pieces), and every kind of monster is covered, including those
  with their own update code such as wolves.
- **One brain per monster.** The host's game decides what each monster does. The other games play the same action,
  on the same target, with the same jump points and the same grabs, about 10 ms after the host's game on top of the
  connection's delay. Their own monster AI still runs for everything the action needs, but no longer decides.
- **Places in world coordinates.** The game counts places from the map tile its own player is in; Arisen Together
  converts every place once, so players far apart still see monsters where they really are.

This was built and measured with two copies of the game side by side, over a simulated connection; play sessions
over the internet are under way. The monster work uses the Steam version's game code and is off on GOG for now.

## Install and play

1. Download the latest release zip and unzip it in a folder of its own (not the game folder).
2. Open `ArisenTogether.exe`. It finds the game (Steam or GOG) and checks its version.
3. Pick **Host**, **Guest** or **Solo**, then **Launch the game**.
4. Guests stay on the title screen until the launcher says **Connected!**, then load a game.

`README.txt` in the zip has the details: joining a specific host with a code, GOG and direct IP, the in-game
controls, and how a guest keeps what they earned.

Every player needs the same Arisen Together version. It does not play with DDDA Coop.

## Building from source

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (built with release 20260922, `ucrt-x86_64`);
- the [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 source.

```powershell
.\build.ps1 -Toolchain <llvm-mingw folder> -MinHook <minhook folder>
```

This makes `build\dinput8.dll` (the mod), `build\ArisenTogether.exe` (the launcher) and `build\ArisenTogether.zip`
(what a player downloads).

## Credits

- **clepix21**, author of [DDDA Coop](https://github.com/clepix21/ddda-coop). Arisen Together is built on its core,
  up to its version 0.11.0 (MIT licence).
- **FluffyQuack** (ARCtool) and the Dragon's Dogma modding community, for their research on the game's archives and
  spawn files.
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD 2-Clause licence).
- Developed with the help of **Claude** (Anthropic).

## Licence

MIT: see [LICENSE](LICENSE). MinHook's licence is in [dist/LICENCES.txt](dist/LICENCES.txt).

Unofficial mod, not affiliated with Capcom. Dragon's Dogma: Dark Arisen © Capcom.
