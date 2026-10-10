# bbcoop

Seamless-style co-op for two players, as a plugin for bbport's online build (bbhost's
plugin API 11, Bloodborne 1.09). Both machines run it: one is the
**host** (you play in that world), the other the **guest**. The two plugins talk
directly over UDP on the LAN; the game itself uses only the private server.

| # | Switch | Default | What it does |
|---|---|---|---|
| 1 | `free_bells` | on, live | Beckoning and Old Hunter's Bells cost no Insight (EquipParamGoods 200/201 `consumeHeroPoint` 1 -> 0) |
| 1 | `give_bells` | on, live | A character has the Beckoning Bell (with the Silencing Blank the game gives along) and the Small Resonant Bell as soon as it wakes up in the clinic (the game gives them later, in the Hunter's Dream, with Insight), given in its own world once no cutscene plays (flag 9180 off); once per load it gets any it lacks. The game then gives no second one: the messenger's gift and the shop's bell are marked given (flags 6622, 6610) |
| 1 | `auto_ring` | on, live | Rings the right bell by itself while the two are apart |
| 2 | `summon_anywhere` | on, next start | Wozzardman's 12 patches: bells work where the boss is dead and across maps |
| 2 | `follow_host` | on, live | Before ringing, the guest is moved to the host's map and spot (the game's forced-summon warp). Never out of the Hunter's Dream or a chalice dungeon |
| 2 | `join_at_host` | on, live | As she joins, the guest is moved to the host's side (the game puts her where she rang) |
| 3 | `auto_rejoin` | on, live | After any drop the guest comes back by itself (off: only the first join is automatic) |
| 3 | `keep_after_boss` | off, live | A boss kill no longer sends the guest home |
| 3 | `lamps` | on, live | Lamps work while together, for both: one lit by either is lit in both worlds (in the guest's once she is home). The guest's **Return to the Hunter's Dream** takes her out of the session and to her own Dream. Set it the same on both |
| 3 | `guest_talk` | on, live | In the host's world the guest talks to NPCs and uses lamps as at home |
| 4 | `boss_progress` | on, live | Bosses killed together are marked dead in the guest's own world when she is home (`extra_flags` adds more) |
| 4 | `share_drops` | on, live | The guest gets a boss's drop too (its item lot, or the repeat lot if she already had it), once per kill, even while down; the drop is marked taken in her own world |
| 4 | `share_rewards` | on, live | An item the game gives one of you while together (a boss's drop, an NPC's gift), the other gets too, as it is given; not one their own game gave them too, nor one she had in her own world. Set it on both |
| 4 | `share_world` | on, live | Every lasting change the host's world makes while together (doors, shortcuts, NPC quests, bosses, the time of day) goes into the guest's world as she is next home. Not item pickups, not chalice dungeons. A `catchup.txt` in the host's plugin folder sends his whole world once, at the next meeting (it replaces what she did alone). Set it on both |
| 4 | `share_insight` | on, live | The Insight the host's world gives him while together (not his own Insight items) the guest gets too, in place of the visitor's 1 for a boss. Set it on both |
| 4 | `guest_pickups` | on, live | The guest picks up too. In the host's world her game has its own copy of every item (seven byte patches, from her next load): each of you picks up your own, and what she picks up is marked taken in her own world. What she took in her own world stays taken in his (her game swaps in the host's flags, which have no item flags; the plugin removes those copies after each load). |
| 4 | `share_pickups` | off, live | Pickups copied to the other. Whatever one of you picks up while together (an item with a pickup flag), the other gets too: the partner's game picks up its own copy of the same item, or, when that one is not lying near them, is given its item lot (the host's world then marks it taken; the guest's copy goes as soon as it comes into view). Items her game places that the host's world had taken already are removed from her view (she asks the host about each item's flag). Not shared: random drops without a flag, the few NPC sets whose pieces have different flags, chalice-dungeon lots. Set it the same on both |
| 5 | `seamless` | off, next start | Experimental: loads no longer end the session; if the partner is not back 45 s after a load, re-join takes over |
| 6 | `downed` | on, live | Elden Ring style deaths while together: a killing blow leaves you **down** (1 HP, invincible, unseen and unheard by enemies, still) instead of dead, kneeling where you fell with no control (the game's own kneel animations 101260/70/80 and Lua EnableLogic off). A boss kill, a pick-up or a lamp brings you back at full HP next to the partner; the partner going down too, the session ending or **Give up** makes it a real death. Enemies on both screens ignore you (Player Hide, and the game's sight record says hidden) |
| 6 | `spectate` | off, live | While down, the camera follows the partner (only when their character here stands where their heartbeat says) |
| 6 | `pick_up` | on, live | Next to the one down (2.5 m across, 2 m up or down, by both players' own positions), the game's **Pick Up Item** prompt shows; pressed, it gets them up for 1 Blood Vial (greyed, nothing, without one; the vial goes once they are up). The one down stays in sight (off: hidden) |
| 6 | `lamp_revive` | on, live | Going to a lamp (its prompt showing) or lighting one while the partner is down gets them up, free |
| 3 | `party_health` | 120, live | While together every enemy has this % of its health (max HP and HP scaled in place, as the game's own Lua max-HP rate does), and its own again 5 s after you are apart. Not in the Dream or chalice dungeons (the game scales those bosses itself). Set it the same on both |

Also: `role` (host/guest), `partner` (the other machine's IP, `:port` optional),
`port` (UDP, 9311), `rejoin_delay`, `pause_seconds`, `banners`, and the in-game
(Insert / L3+R3) buttons **Leave now** (ends the session, pauses re-joining), **Join now** and
**Give up** (while down: die now).

The two plugins must be the same version (the link says so in the log when not).
What both games must do alike is the **host's** choice: while it hears from the host,
the guest plays by its `downed`, `pick_up`, `lamp_revive`, `lamps`, `share_pickups` and
`party_health`.

### In the game's own menu

With the mod **Co-op - menu and text** installed (below), the game's **System** menu has
four co-op pages - *Co-op: Joining*, *Deaths*, *Together*, *Items* - built from the game's
own option rows, so the controller sets them. While together it also has **Co-op: Leave
Session**, and while down **Co-op: Give Up**, each asking YES/NO as Exit Game does (which
stays last). What the menu sets is kept in `<data>/plugins/bbcoop/menu.txt` and wins over
`bbport.ini` until the overlay changes that setting again.

The same mod gives the game the co-op's own text: the revive prompt reads **Revive** (a
copy of the Pick Up Item prompt's ActionButtonParam row in the empty row 10000000), and
the co-op's messages show the way the game's own session messages do (`banners` off:
none). Without the mod all of this falls back to the Pick Up Item prompt and bbport's
banner.

The mod is made from the game's dump (after bbhost's menu generator, GPL-3.0):

```sh
g++ -std=c++17 -O2 -Wall -Wextra -o build/coop_assets tools/coop_assets.cpp -lz
build/coop_assets <dvdroot_ps4> tools/coop_messages.tsv build/mod
```

It writes `menu/optionsetting.gfx` (the four pages, cloned from Controls, and a scroll bar
for System) and every language's `msg/<lang>/menu.msgbnd.dcx` (the texts in
`tools/coop_messages.tsv`, English in all). `install.sh` copies `build/mod` as the mod.

## Install

```sh
./build.sh                # build/bbcoop.so: libc only, symbol versions <= GLIBC_2.38
./install.sh              # this machine
./install.sh deck         # another one, over ssh (a Steam Deck, say)
```

Both also install `build/mod` (when it is there) as the mod `Co-op - menu and text`;
the plugin and the mod take effect at the next start of the game. The data folder is
the AppImage's, `~/.local/share/bbport` (`BB_DATA_DIR=...` names another).

Then in `bbport.ini` on each machine (or the launcher's co-op group), with `online=1`
and `online.host` set:

```ini
plugins.bbcoop=1
bbcoop.role=host              # guest on the other machine
bbcoop.partner=192.168.1.20   # the other machine's LAN address
```

The game's walls between areas while two play together are taken out by a
loose-file mod, on both machines: `python3 tools/no_walls.py <dvdroot_ps4>
"$HOME/.local/share/bbport/mods/Co-op - no zone walls"`.

No firewall rule is needed for the plugins' link: both sides send to each other on
UDP 9311, so the other side's packets are replies.

## Checks without the game

```sh
./build.sh <decrypted 1.09 eboot.elf>   # also checks every patch and function byte (tools/verify.cpp)
g++ -std=c++17 -rdynamic -o build/sim tools/sim.cpp -ldl
cp build/bbcoop.so build/a.so && cp build/bbcoop.so build/b.so    # two files: dlopen loads a path once
build/sim build/a.so build/b.so <eboot.elf> "$(mktemp -u)"        # a new folder each run; two plugins, a pretend game, a virtual clock:
#   bells and travel, two matches, a boss kill, the guest down then revived by it, her drop,
#   enemies ignoring her only while down, pickups both ways (her copy picked up, or the lot given and her copy
#   removed when it shows; his copy picked up, or the lot and flag; what he took before she came hidden from her),
#   both down = both dead, lamps both ways and her Return to her own Dream, and picking her up
#   (greyed without a Blood Vial, one used, the last one's stack taken out) and a lamp getting her up, and the bells for a new
#   character and for one that lacks the Small Resonant Bell, the guest playing by the host's rules, Insight, gestures and
#   rewards for both, and her world taking on his (changes while together, as they part, the one-time catch-up) (64 checks)
python3 tools/bossflags.py <dvdroot_ps4>                 # where the boss flags and the bell cost come from
```

Addresses are in `addrs.hpp` (ELF addresses; the plugin API's are those + 0x400000).
