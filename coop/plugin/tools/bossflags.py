"""bossflags.py <dvdroot_ps4>: every boss's defeat flag and the Insight cost of the bells,
read from the 1.09 dump (the sources of addrs.hpp's kBossFlags and of the free-bells switch).

A boss's defeat flag is the id of the map event that runs HandleBossDefeat (EMEVD 2003[12]).
The bells' cost is EquipParamGoods.consumeHeroPoint (offset 0x38)."""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402

root = sys.argv[1]
for path in sorted(glob.glob(os.path.join(root, 'event', 'm*_00_00.emevd.dcx'))):
    events = souls.emevd_events(souls.dcx(open(path, 'rb').read()))
    hits = [eid for eid, ins in events if (2003, 12) in ins]
    if hits:
        print(os.path.basename(path).split('.')[0], hits)
params = souls.bnd4(souls.dcx(open(os.path.join(root, 'param', 'gameparam', 'gameparam.parambnd.dcx'), 'rb').read()))
goods = next(v for k, v in params.items() if k.endswith('EquipParamGoods.param'))
rows = souls.param_rows(goods)
print('goods with an Insight cost:', [(rid, goods[off + 0x38]) for rid, off in rows.items() if goods[off + 0x38]])
