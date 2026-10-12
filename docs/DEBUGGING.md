# Catching a crash with a debugger

By default a fatal fault or a GPU-library assertion ends the process at once: the log keeps
one line (`Host fault`, `Guest fault`, `STOP: GPU library assertion failed`) and nothing else.
`BB_FAULT_WAIT=1` keeps the process alive at that point, stopped inside its handler, so a
debugger can look at the faulting thread with its registers, stack and memory still there.

1. Launcher → *Mode → Extra environment*: `BB_FAULT_WAIT=1` (or export it before `run.sh`).
   Nothing changes while the game runs; it costs nothing.
2. When it crashes, the game freezes, a desktop notification says so, and the log ends with
   `bbport: ... the process stays alive for a debugger: bash tools/debug_attach.sh <pid>`.
3. Run that line. gdb attaches, writes `logs/crash-<time>-<pid>.txt` and kills the process.
   With `--keep` it detaches instead, so `gdb -p <pid>` can be used by hand afterwards.

The report has every thread's stack, and for the faulting thread its registers, the
instructions around `pc` and the top of its stack; for a GPU-library assertion the locals of
the asserting thread (`bt full`). bbport's own frames carry file and line (the build is
RelWithDebInfo); libc's come from debuginfod when it is reachable. Game code shows as a
*guest offset* (pc minus the image base, as in the log's `Guest fault` line); the report gives
an `objdump` command that disassembles `out/boot-linked.bin` around it.

GPU hangs are different: the device is lost and the GPU breadcrumbs in the log name the stuck
draw or dispatch; the assertion that follows is caught the same way, but the state of interest
is on the GPU.
