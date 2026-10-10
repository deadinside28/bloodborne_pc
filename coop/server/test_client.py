#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""test_client.py: plays a full co-op summon against bbcoop-server the way two bbhost games do.

  guest: logs in, registers its P2P port (STUN + context_start), puts up a co-op sign and
         re-posts it until the summon arrives; an INVITE datagram from the host makes it join
         the host's room.
  host:  logs in, registers, lists signs, creates its room, sends the summon request,
         resolves the guest and sends INVITE datagrams until the guest has joined.
  both:  once in the room, ping each other over UDP; then the guest leaves and the host
         closes the room.

  test_client.py --server 192.168.1.10                  host and guest in this process
  test_client.py --server ... --role guest --name G     one side only (the other elsewhere)
  test_client.py --server ... --role host --name H --guest G
Exit status 0 when every step happened.
"""
import argparse
import base64
import json
import socket
import struct
import sys
import threading
import time
import urllib.request

VPORT = 30
T0 = time.time()


def say(who, msg):
    print(f'{time.time() - T0:6.2f}s [{who}] {msg}', flush=True)


class Client:
    def __init__(self, server, name, p2p_port, local_ip, area):
        self.server, self.name, self.area = server, name, area
        self.api = f'http://{server}:18671'
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(('0.0.0.0', p2p_port))
        self.p2p_port = p2p_port
        self.local_ip = local_ip
        self.mapped = None
        self.stun_wait = {}
        self.lock = threading.Lock()
        self.events = []
        self.cursor = 0
        self.room = self.room_sid = self.member_id = 0
        self.peers = {}        # online id -> (addr, port)
        self.heard = {}        # online id -> pongs received
        self.invited_room = 0
        self.claim = None
        self.running = True
        threading.Thread(target=self.udp_loop, daemon=True).start()

    # -- HTTP
    def post(self, path, body, query=''):
        url = self.api + path + (('?' + query) if query else '')
        req = urllib.request.Request(url, data=json.dumps(body).encode(), method='POST',
                                     headers={'Content-Type': 'application/json',
                                              'X-BBHost-Ruleset': 'vanilla'})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read() or b'{}')

    # -- UDP: STUN answers and P2P datagrams ([0xff][0x40][src vport][dst vport] payload)
    def udp_loop(self):
        while self.running:
            try:
                data, src = self.sock.recvfrom(4096)
            except OSError:
                return
            if len(data) >= 20 and data[:2] == b'\x01\x01':
                ev = self.stun_wait.get(data[4:20])
                if ev:
                    at, ln = 20, struct.unpack('!H', data[2:4])[0]
                    while at + 4 <= 20 + ln:
                        t, al = struct.unpack('!HH', data[at:at + 4])
                        if t == 0x0001:
                            port = struct.unpack('!H', data[at + 6:at + 8])[0]
                            self.mapped = (socket.inet_ntoa(data[at + 8:at + 12]), port)
                        at += 4 + ((al + 3) & ~3)
                    ev.set()
                continue
            if len(data) < 4 or data[0] != 0xff:
                continue
            try:
                kind, who, arg = data[4:].decode().split(' ', 2)
            except ValueError:
                continue
            if kind == 'INVITE' and not self.invited_room:
                self.invited_room = int(arg)
                say(self.name, f'INVITE datagram from {who} at {src[0]}:{src[1]} for room {arg}')
            elif kind == 'PING':
                self.send(src, f'PONG {self.name} {arg}')
            elif kind == 'PONG':
                with self.lock:
                    self.heard[who] = self.heard.get(who, 0) + 1

    def send(self, addr, text):
        self.sock.sendto(bytes([0xff, 0x40, VPORT, VPORT]) + text.encode(), addr)

    def stun(self):
        txid = struct.pack('!I', 0x2112A442) + bytes(12)
        txid = txid[:4] + struct.pack('!d', time.time())[:8] + self.name.encode()[:4].ljust(4, b'_')
        ev = threading.Event()
        self.stun_wait[txid] = ev
        for _ in range(3):
            self.sock.sendto(struct.pack('!HH', 1, 0) + txid, (self.server, 3478))
            if ev.wait(1.0):
                return True
        return False

    # -- the session service
    def start(self):
        r = self.post('/basic_utils/login', {'MessageId': 'LoginRequest', 'AuthorizationCode': 'DUMMY',
                                              'PlatformAccountId': self.name, 'ApplicationVersion': 109,
                                              'IssuerId': 10, 'LanguageId': 1, 'NatType': 2, 'RegionId': 1})
        self.session_id, self.user_id = r['SessionId'], r['UserId']
        say(self.name, f'login: user {self.user_id}, session {self.session_id[:8]}...')
        ok = self.stun()
        say(self.name, f'stun: {"sees our P2P port as %s:%d" % self.mapped if ok else "NO ANSWER"}')
        b = {'OnlineId': self.name, 'SignalingAddr': self.local_ip, 'SignalingPort': self.p2p_port}
        if self.mapped:
            b.update(MappedAddr=self.mapped[0], MappedPort=self.mapped[1])
        self.post('/mp/matching2/context_start', b)
        for fn in (self.poll_loop, self.heartbeat_loop, self.ping_loop):
            threading.Thread(target=fn, daemon=True).start()
        return ok

    def endpoint(self):
        m = self.mapped or ('', 0)
        return {'OnlineId': self.name, 'LocalAddr': self.local_ip, 'LocalPort': self.p2p_port,
                'PublicAddr': self.local_ip, 'PublicPort': self.p2p_port,
                'MappedAddr': m[0], 'MappedPort': m[1]}

    def poll_loop(self):
        while self.running:
            try:
                r = self.post('/np/events/poll', {'OnlineId': self.name, 'Cursor': self.cursor,
                                                  'Categories': ['matching2']})
            except OSError:
                time.sleep(0.5)
                continue
            if r.get('HasEvent'):
                self.on_event(r)
                self.post('/np/events/ack', {'OnlineId': self.name, 'Cursor': r['NextCursor']})
                self.cursor = r['NextCursor']
                continue
            self.cursor = max(self.cursor, r.get('NextCursor', self.cursor))
            time.sleep(0.25)

    def on_event(self, ev):
        name = ev.get('Name')
        with self.lock:
            self.events.append(name)
        if name == 'guest_invite':
            say(self.name, f'event guest_invite: room {ev["RoomId"]} from {ev["HostOnlineId"]} '
                           f'at {ev["HostAddr"]}:{ev["HostPort"]}')
            for _ in range(3):  # open our NAT toward the host, as bbhost's punch does
                self.sock.sendto(b'probe', (ev['HostAddr'], ev['HostPort']))
        elif name == 'room_member_joined':
            say(self.name, f'event room_member_joined: member {ev["MemberId"]} {ev["OnlineId"]} '
                           f'at {ev["Addr"]}:{ev["Port"]}')
            with self.lock:
                self.peers[ev['OnlineId']] = (ev['Addr'], ev['Port'])
        elif name == 'room_member_left':
            say(self.name, f'event room_member_left: member {ev["MemberId"]} {ev.get("OnlineId")} ({ev.get("Reason")})')
            with self.lock:
                self.peers.pop(ev.get('OnlineId'), None)
        else:
            say(self.name, f'event {name}: {json.dumps(ev.get("Payload", ev))}')

    def heartbeat_loop(self):
        while self.running:
            time.sleep(5)
            if self.room_sid:
                self.post('/mp/matching2/heartbeat', {'SessionId': self.room_sid, 'MemberId': self.member_id})

    def ping_loop(self):
        n = 0
        while self.running:
            time.sleep(0.5)
            n += 1
            with self.lock:
                targets = list(self.peers.values())
            for addr in targets:
                self.send(addr, f'PING {self.name} {n}')

    # -- the guest's game
    def sign(self):
        blob = bytearray(0xE0)
        blob[0x76] = 7  # a co-op sign
        struct.pack_into('<i', blob, 0xD0, self.user_id)
        r = self.post('/summon_messenger/create', {
            'MessageId': 'SummonDataCreateRequest', 'SessionId': self.session_id, 'UserId': self.user_id,
            'CharaId': 1, 'AreaId': self.area, 'AreaRegionId': 1, 'SummonType': 0, 'MatchingLevel': 40,
            'SummonData': base64.b64encode(bytes(blob)).decode(), 'SummonDataVersion': 3},
            f'user_id={self.user_id}')
        if 'TargetUserId' in r and not self.claim:
            self.claim = r
            say(self.name, f'sign post answered with the summon: user {r.get("UserId")} -> us')
        return r

    def join(self, room):
        r = self.post('/mp/matching2/join_room', dict(self.endpoint(), RoomId=room))
        if r.get('ResKind') != 0:
            say(self.name, f'join_room {room} FAILED: {r}')
            return False
        self.room, self.room_sid, self.member_id = room, r['SessionId'], r['MemberId']
        for m in r.get('Members', []):
            with self.lock:
                self.peers[m['OnlineId']] = (m['Addr'], m['Port'])
        say(self.name, f'joined room {room} as member {self.member_id}; members '
                       f'{[(m["OnlineId"], m["Addr"], m["Port"]) for m in r.get("Members", [])]}')
        return True

    def leave(self):
        if self.room_sid:
            self.post('/mp/matching2/leave_room', {'SessionId': self.room_sid, 'MemberId': self.member_id})
            say(self.name, f'left room {self.room}')
        self.room = self.room_sid = self.member_id = 0
        with self.lock:
            self.peers.clear()

    # -- the host's game
    def find_sign(self, guest_user):
        r = self.post('/summon_messenger/get', {
            'MessageId': 'SummonDataGetListRequest', 'SessionId': self.session_id, 'UserId': self.user_id,
            'AreaId': self.area, 'AreaRegionId': 1, 'GetMaxCount': 20, 'MatchingLevel': 120,
            'SummonTypeList': [{'SummonType': 0, 'GetLimitCount': 5}]}, f'user_id={self.user_id}')
        for s in r.get('SummonDataList', []):
            if guest_user is None or s.get('UserId') == guest_user:
                return s
        return None

    def create_room(self):
        r = self.post('/mp/matching2/create_room', dict(self.endpoint(), MaxMembers=5, HostArea=self.area,
                                                         HostLevel=120, HostPos=[1.0, 2.0, 3.0], MemberTag=1))
        self.room, self.room_sid, self.member_id = r['RoomId'], r['SessionId'], r['MemberId']
        say(self.name, f'room {self.room} created (member {self.member_id})')

    def summon(self, sign):
        self.post('/summon_messenger/request', {'MessageId': 'SummonDataSummonRequest', 'SessionId': self.session_id,
                                                 'UserId': self.user_id, 'TargetUserId': sign['UserId'],
                                                 'TargetCharaId': sign.get('CharaId')}, f'user_id={self.user_id}')
        say(self.name, f'summon request for user {sign["UserId"]} sent')


def wait(cond, seconds, step=0.2):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(step)
    return False


def run_guest(g, steps):
    steps['guest online'] = g.start()
    last = 0
    while not (g.invited_room and g.claim):  # the game re-posts its sign until summoned
        if time.time() - last > 2:
            g.sign()
            last = time.time()
        time.sleep(0.1)
        if time.time() - T0 > 90:
            return
    steps['guest got the summon in its sign answer'] = g.claim is not None
    steps['guest joined the host room'] = g.join(g.invited_room)
    host_name = next(iter(g.peers), None)
    steps['guest hears the host over UDP'] = wait(lambda: g.heard.get(host_name, 0) >= 3, 10)
    time.sleep(1)
    g.post('/summon_messenger/delete', {'MessageId': 'SummonDataRemoveRequest', 'SessionId': g.session_id,
                                        'UserId': g.user_id})
    g.leave()


def run_host(h, guest_name, steps):
    steps['host online'] = h.start()
    guest_user = None
    sign = None
    if not wait(lambda: h.find_sign(guest_user) is not None, 30, 1.0):
        say(h.name, 'no sign found')
        return
    sign = h.find_sign(guest_user)
    blob = base64.b64decode(sign['SummonData'])
    steps['sign listed with byte 0x79 = 1'] = blob[0x79] == 1
    say(h.name, f'found the co-op sign of user {sign["UserId"]} (blob byte 0x79 = {blob[0x79]})')
    h.create_room()
    h.summon(sign)
    r = h.post('/np/signaling/resolve', {'OnlineId': guest_name})
    steps['host resolved the guest'] = r.get('ResKind') == 0
    addr = (r['Addr'], r['Port'])
    say(h.name, f'resolved {guest_name} at {addr[0]}:{addr[1]}; sending INVITE datagrams')
    end = time.time() + 20
    while time.time() < end and guest_name not in h.peers:
        h.send(addr, f'INVITE {h.name} {h.room}')
        time.sleep(0.3)
    steps['host saw the guest join'] = guest_name in h.peers
    steps['host hears the guest over UDP'] = wait(lambda: h.heard.get(guest_name, 0) >= 3, 10)
    steps['host saw the guest leave'] = wait(lambda: 'room_member_left' in h.events, 15)
    h.leave()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='127.0.0.1')
    ap.add_argument('--role', choices=('both', 'host', 'guest'), default='both')
    ap.add_argument('--name', default='')
    ap.add_argument('--guest', default='Test-Guest', help='the guest\'s online id (host role)')
    ap.add_argument('--ip', default='', help='this machine\'s LAN address (default: the one routing to the server)')
    ap.add_argument('--port', type=int, default=0, help='P2P UDP port (default 19307 host, 19308 guest)')
    ap.add_argument('--area', type=int, default=0x18010000)
    a = ap.parse_args()
    if not a.ip:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((a.server, 3478))
        a.ip = s.getsockname()[0]
        s.close()
    steps = {}
    threads = []
    if a.role in ('both', 'guest'):
        g = Client(a.server, a.name if a.role == 'guest' and a.name else 'Test-Guest', a.port or 19308, a.ip, a.area)
        threads.append(threading.Thread(target=run_guest, args=(g, steps)))
    if a.role in ('both', 'host'):
        h = Client(a.server, a.name if a.role == 'host' and a.name else 'Test-Host', a.port or 19307, a.ip, a.area)
        guest_name = 'Test-Guest' if a.role == 'both' else a.guest
        threads.append(threading.Thread(target=run_host, args=(h, guest_name, steps)))
    for t in threads:
        t.start()
        time.sleep(0.5)
    for t in threads:
        t.join(120)
    print('\nresult:')
    for k, v in steps.items():
        print(f'  {"ok  " if v else "FAIL"} {k}')
    expected = {'both': 10, 'guest': 4, 'host': 6}[a.role]
    ok = all(steps.values()) and len(steps) == expected
    print('FULL SUMMON OK' if ok else 'SUMMON INCOMPLETE')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
