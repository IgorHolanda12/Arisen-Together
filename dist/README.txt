Arisen Together 0.12.0 (alpha) - online co-op for Dragon's Dogma: Dark Arisen (Steam or GOG, 2 to 4 players)
===============================================================================================================

Each player needs the game (Steam or GOG, current version) on their own PC, and this same mod
version (0.12.0).
- Steam: guests must be Steam friends with the host. No IP address or port to set.
- GOG: players join by IP address (direct IP, UDP port 27101). See "GOG and direct IP" below.

Arisen Together does not play with DDDA Coop: every player needs Arisen Together 0.12.0.

Files: ArisenTogether.exe (the launcher), dinput8.dll (the mod), coop.ini (settings). Keep them
together, in a folder of their own, not the game folder (the launcher copies what the game needs).
The launcher is in English, French, Italian, German and Spanish (globe button, top right).

Play
----
1. Open ArisenTogether.exe. It finds the game (Steam or GOG). Both installed? "Settings" > "Game version":
   Steam or GOG. Not found: "Settings" > "Change…", pick the folder with DDDA.exe.
2. In "Play", pick a role:
   - "Host": loads their game; friends join them.
   - "Guest": joins the host with their own character, which takes a pawn's place.
   - "Solo": your normal game; what you earned in co-op is added to it.
3. Click "Launch the game". Your saves are first copied to Documents\Arisen Together\Backups.
4. Guest: stay on the title screen until the launcher says "Connected!".
   Then press Start and load a game: you arrive in the host's world.
Several hosts at once on Steam? The host copies the code ("Lobby" > "Copy the code"); the guest pastes
it in their "Lobby" before launching. Without a code, a guest joins the first friend who hosts.

GOG and direct IP
-----------------
The GOG version has no Steam, so players join by address. "Play without Steam" (Settings) is always
on for GOG. Steam players can turn it on too, to play with a GOG friend or on a local network.
1. Host: launch the game. "Lobby" shows your addresses: "Copy the address" and send it to your friends.
2. Guest: "Guest", paste that address in "Lobby" (for example 192.168.1.20), then launch the game.
3. Same network (same router): nothing else to do.
   Over the internet: the host forwards UDP port 27101 on their router to their PC, or you all use a
   VPN that makes a local network, and the address it gives.
GOG only:
- Keep GOG Galaxy open and signed in: without it the game may stay on "Accessing server...".
- When you load a game, the GOG game may show "Connection failed" (GOG's pawn server): press A or
  Enter, the game loads.
- Your GOG Galaxy saves are copied at each launch to Documents\Arisen Together\Backups\<date>\GOG.
- The monster work of Arisen Together (below) uses the Steam version's game code and is off on GOG
  for now; GOG has not been tested with this version.

In game
-------
- Help a downed player up: hold B next to them.
- Explore alone, or come back to the host: hold LB + RB + Y for 1.5 s (or F7).
- Grab a player or a small monster: RT.
- The Rift is closed during co-op. Avoid the game's online features.

After playing
-------------
- Guest: your own save is not loaded or saved. Experience, discipline and items your character
  earned wait in the game folder (coop_progress). To add them: "Solo", launch, load your game, save.
- "Remove the mod" restores the original game.
- A problem? "Log" > "Export the log", then send the file (Documents\Arisen Together\Logs).

Good to know
------------
- Alpha: keep copies of your saves.
- If the game folder already has another dinput8.dll (another mod), it is not replaced: remove it first.
  A DDDA Coop dinput8.dll is replaced.
- The exported log holds your Steam name and Steam ID and your friends': send it only to people you trust.

Arisen Together 0.12.0
----------------------
The first version of Arisen Together. It grew from DDDA Coop 0.11.0 by clepix21 and takes its own
road on monsters: every player should meet the same monsters, doing the same things, at the same time.
- The same monsters appear for everyone. The game's random draws that decide which monsters appear
  (Bitterblack Isle's spawn lottery and its stage variants, respawns) are made once, by the first player
  in a place, and shared. The game leading a group of players decides which monsters exist; the others
  create the same ones and remove any extra, so a monster never exists on one screen only.
- Each monster has an exact ID, the same on every PC: its stage, map tile, group and spawn entry, read
  again every frame. Monster parts (the Gazer's tentacles, heads, cut pieces) have IDs too, and every
  kind of monster is covered, wolves included.
- One brain per monster: the host's game decides what each monster does. The other games play the same
  action, on the same target, with the same jumps and grabs, about 10 ms after the host's game (plus
  the connection's delay). host_ai=0 in coop.ini lets each game run its own monsters again.
- New look for the launcher, with a party row showing who is in the game.
- Discord Rich Presence removed.
- Kept from DDDA Coop 0.11.0: the shared world, GOG and direct IP, crates and barrels, bow and staff aim,
  a player caught by a monster, co-op in the background.

Licence: MIT for the mod's own code (DDDA Coop by clepix21 up to 0.11.0, and Arisen Together);
MinHook: see LICENCES.txt.
Unofficial mod, not affiliated with Capcom. Dragon's Dogma: Dark Arisen (c) Capcom.
