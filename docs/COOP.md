# Online co-op (work in progress)

Two players on one LAN play Bloodborne together, seamless-style: one machine is the
**host** (the world the two play in), the other the **guest**. The game's own summoning
does the joining; a private server on the LAN stands in for the official one, and a plugin
on each machine keeps the two together (it re-joins after every load or boss, moves the
guest to the host, shares drops, pickups, lamps and world progress, and turns deaths into
Elden Ring style "down, waiting to be picked up"). Nothing talks to Sony, FromSoftware or any
public server.

**Status.** Work in progress. Joining, the bells, travel to the host and pickups have been
played on a desktop PC (host) with a Steam Deck–class handheld (guest). The latest changes
(the reworked down/revive, the in-game co-op menu, world sharing and the catch-up, and a
clean-up and speed pass over the plugin) pass every offline check (`verify`, 64 of 64 in
the simulator) but have **not been played yet**; they will be tested soon.

## The pieces

| Path | What it is |
|---|---|
| `gpu/shim/online/` | The game's network calls: NP sign-in, Matching2 rooms, signaling, HTTP, STUN and the P2P sockets, answered by bbport itself (adapted from [bbhost](https://github.com/droogie/bbhost)). Resolved before the offline stubs (`bbgpu_online_resolve`, `src/runtime.c`), and only with `online=1` and `online.host` set; the official `*.scej-network.jp` addresses go to `online.host`, and any other host is refused. |
| `gpu/shim/plugins/` | bbhost's plugin API (v11) on bbport: loads every `*.so` from `<bb-probe dir>/plugins` and `<data>/plugins`, gives it reads, writes, hooks, calls and its own options (`<plugin>.<option>` in `bbport.ini`, shown in the in-game overlay). `plugins.<name>=1` turns one on. |
| `gpu/shim/bbport_overlay.cpp`, `launcher/` | The overlay's *Online co-op* section (Insert or L3+R3) and the launcher's co-op group (server, name, plugin, role, partner). |
| `scripts/patches.py` | With `online=1` the game's own online/offline choice stays (it is skipped otherwise). |
| `coop/server/` | The private server: Python 3, standard library only. The game's server-info file and API (TCP 20443, TCP 18671), bbhost-style matchmaking (TCP 18671) and STUN (UDP 3478). It keeps `users.json`, `live.json` and `server.log` next to itself (never commit them). |
| `coop/plugin/` | **bbcoop**, the co-op itself, as a plugin: options, design and checks in [its README](../coop/plugin/README.md). |

How the plugin works, in short:

- **The game's code, at known addresses.** Every byte patch, hooked or called function and
  table it uses is in `coop/plugin/addrs.hpp`, as an address in the 1.09 executable with the
  bytes expected there and where it was learned. The US (CUSA00900) and EU (CUSA03173) 1.09
  executables are the same code. `tools/verify.cpp` checks all of them against a decrypted
  1.09 ELF.
- **Its own link.** The two plugins send each other a heartbeat over UDP 9311 (position,
  map, state, rules, what each has had of the other's world) and a few messages (item
  questions, world changes). Each kind has a four-letter tag (`BBCC`, `BBQ4`/`BBA4`,
  `BBW2`) that changes whenever its layout does, so two different versions ignore each
  other instead of misreading.
- **The host decides.** Rules both games must apply alike (`downed`, `pick_up`,
  `lamp_revive`, `lamps`, `share_pickups`, `party_health`) come from the host's heartbeat;
  the guest's own settings count only while it hears nobody.
- **The guest's own world.** While together the guest is in the host's world; what should
  last in the guest's own (bosses killed, lamps lit, drops, pickups, the host's world
  changes) is kept in files in its plugin folder (`<data>/plugins/bbcoop/`) and applied when
  the guest is next home.
- **The game thread stays cheap.** The plugin's frame work is byte compares and cached
  lookups; files are written when something changed, as a new file renamed over the old
  one, never synced.
- **Game files come from your own copy.** The menu/text mod and the "no zone walls" mod are
  generated from the player's dump (`coop/plugin/tools/coop_assets.cpp`, `no_walls.py`);
  nothing of the game is in the repository.

## Setup

You need two machines on one LAN, each with bbport and the game set up as in the
[README](../README.md) (1.09), Python 3 on the one that runs the server, and GCC on the one
that builds the plugin.

### 1. bbport from this branch

Build it as usual (`bash build.sh`, or `packaging/appimage.sh` for a Steam Deck). The
online code does nothing until `online=1` is set.

### 2. The server, on one of the two machines

```sh
python3 coop/server/bbcoop_server.py --anywhere   # --anywhere: signs match from any area
```

or as a user service: `coop/server/bbcoop-server.service` (set the path in it, then
`systemctl --user enable --now bbcoop-server`). The machine's firewall must let the LAN in
on TCP 20443, TCP 18671 and UDP 3478. To check it without the game, from the other
machine:

```sh
python3 coop/server/test_client.py --server <server address>   # a whole summon, both sides
```

### 3. `bbport.ini` on each machine

In the launcher's co-op group, or by hand (in the data folder: `~/.local/share/bbport` for
the AppImage, `BB_DATA_DIR`, or the bbport folder when run from source):

```ini
online=1
online.host=192.168.1.10      # the server
online.name=Hunter            # optional: the name the other sees (default: the character's)
plugins.bbcoop=1
bbcoop.role=host              # guest on the other machine
bbcoop.partner=192.168.1.20   # the other machine
```

Optional: `online.p2p_addr` (this machine's address toward the other; found by itself),
`online.p2p_port` (9307), `online.stun_server` (`off` to skip STUN). The game itself
connects the two on UDP 9307: a machine with a firewall must let the LAN in there. The
plugins' own link (UDP 9311) needs no rule.

### 4. The plugin and its two mods

`<dvdroot_ps4>` is the game's data folder in your own dump.

```sh
cd coop/plugin
./build.sh <decrypted 1.09 eboot.elf>    # build/bbcoop.so, and checks every address
g++ -std=c++17 -O2 -o build/coop_assets tools/coop_assets.cpp -lz
build/coop_assets <dvdroot_ps4> tools/coop_messages.tsv build/mod   # the "menu and text" mod
./install.sh                             # the plugin and that mod, on this machine
./install.sh deck                        # the same on another one, over ssh
python3 tools/no_walls.py <dvdroot_ps4> "$HOME/.local/share/bbport/mods/Co-op - no zone walls"   # on each
```

New mod folders are on by default (the launcher's mod list). Without the menu and text mod
the co-op still works, with bbport's banners and the plain "Pick Up Item" prompt in place of
the game's own.

### 5. Play

Start both games and choose **Play online** at the title. With the defaults, the bells ring
by themselves while the two are apart: the guest is moved to the host and joins. The
co-op options are in the game's **System** menu (with the mod) and in the overlay. Both
machines must run the same plugin version (the log says so when they do not).

The server's settings for the game (its server-info file) are read at the game's start:
restart both games after changing the server.

## Contributing

These follow the decisions the code is built on.

- **Private servers only.** Nothing may reach Sony, FromSoftware or a public co-op service,
  or fetch the official server-info file. `online/http.cpp` refuses any host that is not
  `online.host`; keep it that way.
- **Offline play does not change.** All online code stays out of the way unless `online=1`
  and `online.host` are set.
- **1.09 only, every address checked.** New game knowledge goes into `addrs.hpp`: the 1.09
  ELF address, the bytes expected there, a byte patch the same length as what it replaces,
  for a hooked function first 16 bytes that are whole instructions, none rip-relative (the
  hook moves them), and a comment saying what it is and how it was found. `coop/plugin/build.sh <eboot.elf>` must
  report 0 mismatches. Plugin API addresses are ELF addresses + 0x400000.
- **The plugin needs libc only.** C++17 without the C++ library (no `std::`, exceptions,
  RTTI or thread-safe statics), symbol versions up to GLIBC 2.38 (the Steam Deck build's), the
  warnings in `build.sh` as errors. `build.sh` fails a build that breaks this.
- **Keep the frame cheap.** No blocking calls, syncs or full scans of the game's
  characters per frame; cache, and write files only when they change (a new file renamed
  over the old one).
- **A layout change changes the tag.** Changing what the two plugins send each other means a
  new four-letter tag for that message and a new version in `bb_plugin_info`: both machines
  then need the new build, restarted together.
- **The host decides shared rules.** A new rule both games must apply alike goes in the
  heartbeat's rules (`kRuled`), not in each machine's settings.
- **Every option is documented.** A new switch goes in the plugin's option table, the README's
  table (default; whether it applies live or at the next start), and, when it belongs in the
  game's menu, the co-op pages and `tools/coop_messages.tsv`.
- **Every behaviour has a check in the simulator.** `coop/plugin/tools/sim.cpp` runs two
  plugins against a pretend game on a virtual clock; add a scripted check for what you
  change, and all must pass:

  ```sh
  cd coop/plugin
  g++ -std=c++17 -rdynamic -o build/sim tools/sim.cpp -ldl
  cp build/bbcoop.so build/a.so && cp build/bbcoop.so build/b.so
  build/sim build/a.so build/b.so <eboot.elf> "$(mktemp -u)"
  ```

  The simulator is not the game: say in the pull request what was played and what was
  only checked offline.
- **Logs.** The plugin's lines start with `bbcoop:`, once per event, never per frame.
- **No game data.** Generated mods, dumps, saves and the server's data files stay out of the
  repository; tools generate them from the player's own copy.
- **Write like the code around it.** Plain comments that say what the game does and why,
  the repository's names and formatting.

AI coding agents: see [coop/AGENTS.md](../coop/AGENTS.md).

## Licence note

`coop/` is MIT ([coop/LICENSE](../coop/LICENSE)), except
`coop/plugin/tools/coop_assets.cpp`, which is adapted from bbhost and stays
GPL-3.0-or-later. No warranty of any kind.

`gpu/shim/online/` and `gpu/shim/plugins/` are adapted from
[bbhost](https://github.com/droogie/bbhost) and are **GPL-3.0-or-later**, while bbport is
GPL-2.0-or-later. Linked together, the combined program can only be distributed under
GPL-3.0. This needs the maintainer's decision before merging (accept GPL-3.0 for builds with
online play, keep the online parts optional, or replace them with GPL-2.0-compatible code).
