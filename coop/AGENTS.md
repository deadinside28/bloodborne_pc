# Instructions for AI coding agents: online co-op

For work on `coop/`, `gpu/shim/online/`, `gpu/shim/plugins/` and the co-op parts of the
overlay and launcher. Read [docs/COOP.md](../docs/COOP.md) (setup, how it fits together, the
contributing rules) and [coop/plugin/README.md](plugin/README.md) (every option) first; the
rules there apply to you too. This file adds what an agent tends to get wrong.

## Never

- Contact or point anything at a public server: no Sony or FromSoftware hosts, no public
  co-op services, no fetching the official server-info file. Everything runs against the
  player's own server on their LAN.
- Guess an address, offset or byte pattern. Every one in `addrs.hpp` comes from the 1.09
  executable with its expected bytes; if you cannot check one against the ELF, say so and stop.
- Weaken a check to make it pass: not `verify`, not a simulator expectation, not the libc-only
  check in `build.sh`. A failing check means the change is wrong or the check needs a
  reasoned update, explained in the commit.
- Commit game files, dumps, saves, generated mods, decrypted executables, or the server's
  `users.json`, `live.json` and `server.log`.
- Launch or close the game, or run anything on the player's other machine, without asking.
  Never test against the player's own save or data folder: use a separate `BB_DATA_DIR` with
  copies.
- Change the system's audio, network or firewall settings from a script or a test.

## Always

- Build and check before calling a change done:

  ```sh
  cd coop/plugin
  ./build.sh <decrypted 1.09 eboot.elf>            # must end "0 mismatch(es)"
  g++ -std=c++17 -rdynamic -o build/sim tools/sim.cpp -ldl
  cp build/bbcoop.so build/a.so && cp build/bbcoop.so build/b.so
  build/sim build/a.so build/b.so <eboot.elf> "$(mktemp -u)"   # every line PASS, exit 0
  ```

  For `gpu/shim/` or `launcher/` changes, also bbport's own build and tests (README,
  "Repository layout").
- Add a simulator check for each behaviour you add or fix, and make sure the old code fails it.
- Report what you verified and how: "built, verify 0 mismatches, sim 64/64" is not "tested in
  the game". Only a person playing on two machines can say the latter.
- When you change anything one plugin sends the other, change that message's four-letter tag
  and the version in `bb_plugin_info`, and tell the user both machines need the new build and
  must restart together.
- Keep the game thread cheap: no blocking calls or syncs per frame; files written only on
  change, as a new file renamed over the old one.
- Edit in a separate git worktree when someone plays from the checkout: `run.sh` rebuilds at
  every start, so a half-done edit there breaks their next game.
- Match the code around you: plain comments that say what the game does and why, the
  existing names, no new dependencies (the plugin uses libc only; the server the Python
  standard library only).
- Keep a change to what was asked. If a fix needs more (a new option, a wire change, a
  dropped feature), ask first.
