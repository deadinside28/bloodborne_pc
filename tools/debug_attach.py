# gdb script of tools/debug_attach.sh: runs attached to a bb-probe stopped by BB_FAULT_WAIT=1.
import os
import gdb

out = open(os.environ.get("BB_DEBUG_OUT", "/dev/stdout"), "w")


def run(command):
    try:
        return gdb.execute(command, to_string=True)
    except gdb.error as error:
        return f"({command}: {error})\n"


def section(title, text):
    out.write(f"\n==== {title}\n{text}")
    out.flush()


gdb.execute("set pagination off")
gdb.execute("set confirm off")
gdb.execute("set print frame-arguments scalars")
gdb.execute("set debuginfod enabled on")
gdb.execute("set style enabled off")

section("process", run("info inferiors") + run("info proc cmdline"))

# The guest image (eboot.bin) as mapped by bb-probe: guest offsets are pc - image.
image = 0
try:
    image = int(gdb.parse_and_eval("(unsigned long)image"))
except gdb.error:
    pass
GUEST_SPAN = 0x10000000
section("guest image", f"base {image:#x}\n" if image else "unknown\n")

section("threads", run("info threads"))
section("all stacks", run("thread apply all bt 40"))

# Faulting threads: the frame below the signal trampoline. Asserting threads: locals.
inferior = gdb.selected_inferior()
for thread in inferior.threads():
    thread.switch()
    try:
        frame = gdb.newest_frame()
    except gdb.error:
        continue
    names = []
    faulting = None
    walker = frame
    while walker is not None:
        if walker.type() == gdb.SIGTRAMP_FRAME and faulting is None:
            faulting = walker.older()
        names.append(walker.name() or "?")
        walker = walker.older()
    asserting = any(name and "assert_fail" in name for name in names)
    if faulting is None and not asserting:
        continue
    label = f"thread {thread.num} ({thread.name or '?'})"
    if faulting is not None:
        faulting.select()
        pc = int(faulting.pc())
        text = f"pc {pc:#x}"
        if image and image <= pc < image + GUEST_SPAN:
            text += f" = guest offset {pc - image:#x} (objdump -D -b binary -m i386:x86-64 --adjust-vma={image:#x} --start-address={pc - 0x40:#x} --stop-address={pc + 0x40:#x} out/boot-linked.bin)"
        text += "\n" + run("info symbol $pc") + run("info registers")
        text += run("x/12i $pc-24") + run("x/16gx $rsp")
        section(f"{label}: faulting frame", text)
    if asserting:
        section(f"{label}: asserting thread, locals", run("bt full 16"))

section("shared libraries", run("info sharedlibrary"))
out.close()
if os.environ.get("BB_DEBUG_KEEP") == "1":
    gdb.execute("detach")
else:
    gdb.execute("kill")
