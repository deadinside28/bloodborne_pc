# Attach to an already hung bb-probe:
# gdb -nx -q -batch -ex 'set auto-solib-add off' -p PID \
#     -x scripts/diagnose_boot.gdb > bb-boot-state.txt 2>&1
# This reads guest state at three frame boundaries. It does not call guest
# functions, change flags, or skip initialization. Detaches when finished.
set pagination off
set confirm off
set print thread-events off
# The renderer deliberately handles protection faults for guest write tracking.
# Let its handler receive them while advancing to the next frame boundary.
handle SIGSEGV nostop noprint pass
handle SIGUSR2 nostop noprint pass
python
import gdb

BASE = 0x800000000
FRAME = BASE + 0x2018e27
inf = gdb.selected_inferior()
bp = None

def read(address, size):
    return bytes(inf.read_memory(address, size))

def u64(address):
    return int.from_bytes(read(address, 8), 'little')

def label(address):
    if BASE <= address < BASE + 0x10000000:
        return '0x%x (guest +0x%x)' % (address, address - BASE)
    return '0x%x' % address

def object_dump(name, address, size=0x80):
    print('\nOBJECT %s @ %s' % (name, label(address)))
    if not address:
        return
    try:
        data = read(address, size)
        for offset in range(0, size, 32):
            values = [int.from_bytes(data[k:k+8], 'little')
                      for k in range(offset, min(offset+32, size), 8)]
            print('  +%03x: %s' % (offset, ' '.join('%016x' % v for v in values)))
    except gdb.error as error:
        print('  unreadable: %s' % error)

def snapshot(index):
    print('\n===== BOOT FRAME SAMPLE %d =====' % index)
    roots = {}
    for name, offset, size in [
        ('window', 0x5540500, 0x48), ('pacer', 0x55404f8, 0x40),
        ('sprj_task', 0x5540510, 0x80), ('task_table', 0x5540518, 0x268),
        ('dispatcher', 0x54b2e30, 0x60), ('application', 0x569fa30, 0x100),
        ('system', 0x56a54f8, 0x100),
    ]:
        address = u64(BASE + offset)
        roots[name] = address
        print('GLOBAL %s [+0x%x] = %s' % (name, offset, label(address)))
        object_dump(name, address, size)
    dispatcher = roots['dispatcher']
    if dispatcher:
        print('DISPATCHER in_update=%d (byte +0x38; sampled before dispatch)' %
              read(dispatcher + 0x38, 1)[0])
        for name, offset in [('before_queue', 0x10), ('after_queue', 0x18),
                             ('worker', 0x28), ('context', 0x30),
                             ('root', 0x40), ('task_storage', 0x48), ('task_order', 0x50)]:
            address = u64(dispatcher + offset)
            object_dump('dispatcher.' + name, address, 0x100)
            if name == 'root' and address:
                vtable = u64(address)
                object_dump('root.vtable', vtable, 0x40)
                for field in (0x48, 0x50, 0x58, 0x60, 0x68):
                    child = u64(address + field)
                    if BASE + 0x10000000 <= child < 0x1200000000:
                        object_dump('root.field+%x' % field, child, 0x100)
                method = u64(vtable + 0x30)
                print('ROOT UPDATE METHOD: %s' % label(method))
                if index == 1 and BASE <= method < BASE + 0x4000000:
                    print(gdb.execute('x/64i 0x%x' % method, to_string=True))
    table = roots['task_table']
    if table:
        # Slot 0 is the vtable; slots 1..76 are the registered game tasks.
        for slot in range(1, 77):
            address = u64(table + 8 * slot)
            object_dump('task[%d]' % (slot - 1), address, 0x100)
            if address:
                # A task wrapper commonly owns its state through these fields.
                for offset in (8, 0x10, 0x18, 0x20, 0x28, 0x30):
                    child = u64(address + offset)
                    if BASE + 0x10000000 <= child < 0x1200000000:
                        object_dump('task[%d].field+%x' % (slot - 1, offset), child, 0x80)

try:
    if not inf.pid:
        raise RuntimeError('No attached process (check the ptrace error above)')
    # Refuse a different guest revision instead of reading unrelated globals.
    if read(BASE + 0x2018e6f, 5) != bytes.fromhex('e82c840300'):
        raise RuntimeError('Unsupported guest: main-loop instruction signature differs')
    print('bbport boot-state diagnosis v1; inferior PID %d' % inf.pid)
    print(gdb.execute('info threads', to_string=True))
    main = next((t for t in inf.threads() if t.ptid[1] == inf.pid), None)
    if main is None:
        raise RuntimeError('Cannot identify the main Linux thread')
    main.switch()
    print(gdb.execute('bt 16', to_string=True))
    bp = gdb.Breakpoint('*0x%x' % FRAME, internal=True)
    bp.thread = main.global_num
    for index in range(1, 4):
        if index > 1:
            bp.ignore_count = 7
        gdb.execute('continue')
        if int(gdb.parse_and_eval('$pc')) != FRAME:
            raise RuntimeError('Unexpected stop before the frame checkpoint; see debugger output')
        snapshot(index)
    print('\nBOOT DIAGNOSIS COMPLETE: 3 frame boundaries, 8 frames between samples')
except (gdb.error, RuntimeError, StopIteration) as error:
    print('\nBOOT DIAGNOSIS INCOMPLETE: %s' % error)
finally:
    if bp is not None and bp.is_valid():
        bp.delete()
    if inf.pid:
        gdb.execute('detach')
end
