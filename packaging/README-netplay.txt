melonDS 1.1 with internet netplay (KH 358/2 Days co-op build)
===============================================================

This is melonDS 1.1 with one addition: System > Multiplayer > Netplay (internet)...
Every PC runs every player's DS; only controller input goes over the internet,
so the DS link between the consoles never leaves your PC.

What every player needs
- This same build (all players on the same version).
- The same patched ROM: apply days_coop.bps to your own copy of the game.
- Your usual melonDS settings are used (keys, etc.). Netplay picks its own BIOS,
  firmware, clock and renderer for the session, so those settings do not matter.

Playing
1. Open melonDS and load the patched ROM (File > Open ROM). Use one window only.
2. System > Multiplayer > Netplay (internet)...
   Host: choose "Host a session", type your name, keep UDP port 7065 and input
   delay 4, press Host. Your router must forward UDP port 7065 to your PC.
   Give the other players your public IP address (search "what is my IP").
   Guests: choose "Join a session", type your name and the host's address, press Join.
3. When everyone is in the player list, the host presses Start. Every window
   restarts the game together. Then play the co-op mod as its player guide says.

Good to know
- Input delay: 4 frames covers players up to about 65 ms apart (one way). If the
  game keeps stopping for a moment, the host can raise it before starting.
- During a session, pause, reset, save states and cheats are off.
- If a window says "the consoles went out of sync", the PCs no longer agree;
  leave and start a new session (and please report it).
- Leaving: System > Multiplayer > Netplay (internet)... > Leave, or close melonDS.

License
melonDS is free software under the GNU GPL version 3 (LICENSE.txt). The source
code of this build, including the netplay changes, is available at:
  (link to be added when the source is published)
