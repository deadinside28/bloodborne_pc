#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""bbcoop-server: a private Bloodborne online server for two bbhost players on one LAN.

Nothing here talks to any public service. It answers, from memory:

  TCP 20443  the game's server-info file (/bb-us/ss.info and the other regions). bbhost
             rewrites the official *.scej-network.jp URL to online.host, keeping port and path.
  TCP 18671  the game's FromSoftware API (/basic_utils/login, /summon_messenger/..., every
             other endpoint answered with an empty success), bbhost's matchmaking service
             (/mp/matching2/*, /np/signaling/resolve, /np/events/poll and /ack) and the
             play-log uploads bbhost sends to <host>:18671.
  UDP 3478   STUN Binding: tells each client the address its P2P port is seen at.

Summons: the guest's Small Resonant Bell posts a sign (summon_messenger/create, repeated);
the host's Beckoning Bell lists signs (summon_messenger/get) and claims one
(summon_messenger/request). The claim reaches the guest in the answer to its next sign post
and as a bbhost guest_invite event; the two games then meet in a matchmaking room and
talk directly over UDP. With only two players every co-op sign in the host's area matches,
whatever the level or password.

Python 3 standard library only. Logs to server.log next to this file (size-capped).
"""
import argparse
import base64
import collections
import datetime
import http.server
import json
import logging
import logging.handlers
import os
import secrets
import socket
import socketserver
import struct
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
log = logging.getLogger('bbcoop')

# ---- the game's API: path -> the name its MessageId carries -------------------------------

# The order is the game's own (the <api_*> tags of ss.info, read by index 0..36).
API = [
    ('basic_utils/login', 'Login'),
    ('basic_utils/get_datetime', 'ServerTimeGet'),
    ('basic_utils/sync_chara_id', 'SyncCharaId'),
    ('basic_utils/get_normal_notice', 'NoticeNormalGet'),
    ('basic_utils/get_emergency_notice', 'NoticeEmergencyGet'),
    ('basic_utils/get_user_agreement', 'UserAgreementGet'),
    ('blood_messenger/create', 'BloodMessCreate'),
    ('blood_messenger/message_area', 'BloodMessGetList'),
    ('blood_messenger/evaluate_message', 'BloodMessEvaluate'),
    ('blood_messenger/evaluation', 'BloodMessGetEvaluate'),
    ('blood_messenger/delete', 'BloodMessRemove'),
    ('blood_messenger/exist_messages', 'BloodMessSearchAdd'),
    ('channel/upload', 'ChannelUpload'),
    ('channel/share', 'ChannelShare'),
    ('channel/search', 'ChannelSearch'),
    ('channel/word_search', 'ChannelWordSearch'),
    ('channel/get_details_info', 'ChannelGetDetailsInfo'),
    ('channel/get_info', 'ChannelGetInfo'),
    ('channel/random_join', 'ChannelRandomJoin'),
    ('channel/add_material', 'ChannelAddMaterial'),
    ('channel/notify_add_material_complete', 'ChannelAddMaterialCompleteNotify'),
    ('messenger_shell/upload', 'MessengerShellUpload'),
    ('penalty/notify_multi_play_error', 'MultiPlayNetError'),
    ('penalty/notify_user_properties_move_count', 'UserPropertiesMoveCount'),
    ('penalty/check_user_priority_move_count', 'UserPropertiesMoveCountCheck'),
    ('summon_messenger/create', 'SummonDataCreate'),
    ('summon_messenger/get', 'SummonDataGetList'),
    ('summon_messenger/delete', 'SummonDataRemove'),
    ('summon_messenger/request', 'SummonDataSummon'),
    ('chair_messenger/get', 'ChairMessGetList'),
    ('chair_messenger/update', 'ChairMessRespawnPointNotice'),
    ('tomb_messenger/create', 'TombMessCreate'),
    ('tomb_messenger/message_area', 'TombMessGetList'),
    ('tomb_messenger/death_vision_get', 'DeathVisionGet'),
    ('tomb_messenger/delete', 'TombMessRemove'),
    ('wandering_ghost/create', 'WanderingGhostCreate'),
    ('wandering_ghost/get', 'WanderingGhostGet'),
]
API_BY_PATH = dict(API)

# Empty but well-formed parts of the answers whose list the game reads (key names from the
# game's own response parser). ChannelSearch is the exception: the game's four dungeon searches
# while a character loads must find something, and an empty ChannelList counts as "failed to
# connect to Bloodborne server" (error 4601, back to the title menu). One entry without fields
# makes the search fail quietly instead (eboot 1.09 vaddr 0x1e88db4: only results 0x80000001/3/5
# raise it; an incomplete entry gives 0x80000004).
EMPTY_PARTS = {
    'NoticeNormalGet': {'NoticeList': []},
    'NoticeEmergencyGet': {'NoticeList': []},
    'UserAgreementGet': {'UserAgreementText': ''},
    'BloodMessGetList': {'BloodMessList': []},
    'BloodMessGetEvaluate': {'BloodMessEvaluationList': []},
    'BloodMessSearchAdd': {'BloodMessEvaluationList': [], 'LostBloodMessIdList': []},
    'ChannelSearch': {'ChannelList': [{}]},
    'ChannelWordSearch': {'ChannelList': []},
    'ChannelGetDetailsInfo': {'ChannelList': []},
    'ChannelGetInfo': {'ChannelInfoList': [], 'LostChannelIdList': []},
    'ChannelRandomJoin': {'ChannelList': []},
    'ChairMessGetList': {'ChairMessList': [], 'ChairMessCount': 0},
    'TombMessGetList': {'TombMessList': []},
    'DeathVisionGet': {'TombMessList': []},
    'WanderingGhostGet': {'WanderingGhostList': []},
    'UserPropertiesMoveCountCheck': {'CompleteFlag': 1},
}

SIGN_TTL = 45.0           # a sign its game has not re-posted (every 30 s) for this long is gone
CLAIM_TTL = 20.0          # a claimed sign whose guest has not joined by then is up again
MEMBER_TIMEOUT = 30.0     # a room member without a heartbeat for this long has left
INVITE_WAIT = 30.0        # a summon waits this long for the host's room


def now():
    return time.monotonic()


# ---- state --------------------------------------------------------------------------------

class State:
    def __init__(self, data_dir, anywhere):
        self.lock = threading.RLock()
        self.anywhere = anywhere
        self.users_path = os.path.join(data_dir, 'users.json')
        self.live_path = os.path.join(data_dir, 'live.json')  # peers and logins, kept over a restart
        self.users = {}       # "ip|account" -> {UserId, CharaIds}
        self.sessions = {}    # login SessionId -> {UserId, ip, account}
        self.peers = {}       # online id -> endpoint record
        self.rooms = {}       # room id -> room
        self.room_by_sid = {}
        self.events = collections.defaultdict(collections.deque)  # online id -> [(id, name, payload)]
        self.event_seq = int(time.time())  # above any cursor a game kept from an earlier run
        self.signs = {}       # "SessionId:UserId" -> sign record
        self.pending_invites = {}  # host online id -> (guest online id, at)
        self.next_room = 1001
        try:
            with open(self.users_path) as f:
                self.users = json.load(f)
        except (OSError, ValueError):
            self.users = {}
        try:
            with open(self.live_path) as f:
                live = json.load(f)
            self.peers, self.sessions = live['peers'], live['sessions']
        except (OSError, ValueError, KeyError):
            pass

    def save_users(self):
        tmp = self.users_path + '.tmp'
        with open(tmp, 'w') as f:
            json.dump(self.users, f, indent=1)
        os.replace(tmp, self.users_path)

    def save_live(self):
        """Running games register once (at their start and login); a restarted server that forgot
        them could not tell one player where the other is."""
        tmp = self.live_path + '.tmp'
        with self.lock, open(tmp, 'w') as f:
            json.dump({'peers': self.peers, 'sessions': self.sessions}, f, indent=1)
        os.replace(tmp, self.live_path)

    # -- the game's users

    def login(self, ip, account):
        key = f'{ip}|{account}'
        with self.lock:
            user = self.users.get(key)
            if user is None:
                taken = {u['UserId'] for u in self.users.values()}
                uid = 1001
                while uid in taken:
                    uid += 1
                user = {'UserId': uid, 'CharaIds': []}
                self.users[key] = user
                self.save_users()
            sid = secrets.token_hex(16)
            self.sessions[sid] = {'UserId': user['UserId'], 'ip': ip, 'account': str(account)}
            self.save_live()
            return sid, user['UserId']

    def chara_ids(self, user_id, count):
        with self.lock:
            user = next((u for u in self.users.values() if u['UserId'] == user_id), None)
            if user is None:
                return list(range(1, count + 1))
            ids = user['CharaIds']
            while len(ids) < count:
                ids.append(user_id * 100 + len(ids) + 1)
            self.save_users()
            return ids[:count]

    def online_id_of_user(self, user_id):
        """The bbhost online id of a game user: the peer whose account name is the login's
        account, else the one peer that registered from the same address."""
        with self.lock:
            sess = [s for s in self.sessions.values() if s['UserId'] == user_id]
            if not sess:
                return None
            s = sess[-1]
            if s['account'] in self.peers:
                return s['account']
            same_ip = [o for o, p in self.peers.items() if p['ObservedAddr'] == s['ip']]
            return same_ip[0] if len(same_ip) == 1 else None

    # -- events

    def push(self, online_id, name, payload):
        with self.lock:
            self.event_seq += 1
            self.events[online_id].append((self.event_seq, name, dict(payload)))
            log.info('event #%d %s -> %s %s', self.event_seq, name, online_id, short(payload))

    def poll(self, online_id, cursor):
        with self.lock:
            q = self.events.get(online_id)
            if q:
                for eid, name, payload in q:
                    if eid > cursor:
                        return eid, name, payload
            return None, self.event_seq if not q else max(cursor, q[-1][0]), None

    def ack(self, online_id, cursor):
        with self.lock:
            q = self.events.get(online_id)
            while q and q[0][0] <= cursor:
                q.popleft()

    # -- peers

    def register_peer(self, online_id, ip, local_addr, local_port, mapped_addr, mapped_port):
        with self.lock:
            p = self.peers.get(online_id, {})
            p.update({
                'OnlineId': online_id, 'ObservedAddr': ip,
                'LocalAddr': local_addr or p.get('LocalAddr', ''),
                'LocalPort': int(local_port or p.get('LocalPort', 0) or 0),
                'MappedAddr': mapped_addr if mapped_addr is not None else p.get('MappedAddr', ''),
                'MappedPort': int((mapped_port if mapped_port is not None else p.get('MappedPort', 0)) or 0),
                'seen': now(),
            })
            self.peers[online_id] = p
            self.save_live()
            return p


def reach(p):
    """Where other players send to this peer: the STUN-mapped address when the server got one,
    else the configured one, else (a client left at 127.0.0.1) the address it called from."""
    if p.get('MappedAddr') and p.get('MappedPort'):
        return p['MappedAddr'], p['MappedPort']
    local = p.get('LocalAddr', '')
    if not local or local.startswith('127.'):
        return p.get('ObservedAddr', local), p.get('LocalPort', 0)
    return local, p.get('LocalPort', 0)


def member_record(mid, p):
    addr, port = reach(p)
    return {'MemberId': mid, 'OnlineId': p['OnlineId'], 'Addr': addr, 'Port': port,
            'LocalAddr': p.get('LocalAddr', ''), 'LocalPort': p.get('LocalPort', 0),
            'MappedAddr': p.get('MappedAddr', ''), 'MappedPort': p.get('MappedPort', 0)}


def short(obj, limit=400):
    text = obj if isinstance(obj, str) else json.dumps(obj, separators=(',', ':'))
    return text if len(text) <= limit else text[:limit] + f'...({len(text)} chars)'


# ---- ss.info ------------------------------------------------------------------------------

def build_ssinfo(api_base):
    """The server-info file. The game finds <ss>, then <msgM><langL>, <gameurlM> with one
    <api_*> base URL per endpoint, and optional <...IntervalM> settings, where M is a server
    index and L a language index this server does not know: every index is written."""
    out = ['<ss>0</ss>\n']
    langs = ''.join(f'<lang{lang}></lang{lang}>' for lang in range(32))
    for m in range(32):
        out.append(f'<msg{m}>{langs}</msg{m}>\n')
    apis = ''.join(f'<api_{name}>{api_base}</api_{name}>' for _, name in API)
    for m in range(32):
        out.append(f'<gameurl{m}>{apis}</gameurl{m}>\n')
    # Co-op level range: the game's default is the host's level +-(20 + 20%) (eboot defaults in the
    # parser, vaddr 0x1eb65e0), which keeps a level-16 guest from a level-162 host. Both bounds as
    # wide as they go, whether the game takes the larger or the smaller of the two limits.
    # Sign intervals (s): the guest's game re-posts its sign every CreateInterval (a claim also
    # reaches it that way), the host's searches every GetListInterval while its bell is on. The
    # game's own server used 20 / 10; a LAN server answers at once, so the join is that much faster.
    # Co-op guard timer rates (%): each co-op session that ends in an error (a guest's game closed,
    # a timeout) raises a per-summon-type count, and the game then holds off for that count's times
    # from its table (eboot 0x01878b30, table 0x04734490) scaled by these; the next join waited
    # about a minute. 0: no holding off.
    settings = {'SummonDataCreateInterval': 2, 'SummonDataGetListInterval': 1,
                'SummonDataCoopMatchingLevelUpperAbs': 999, 'SummonDataCoopMatchingLevelUpperRel': 1000,
                'SummonDataCoopMatchingLevelLowerAbs': -999, 'SummonDataCoopMatchingLevelLowerRel': -1000,
                'SummonDataGetListCoopGuardTimerObserveTimeRate': 0,
                'SummonDataGetListCoopGuardTimerLimitationTimeRate': 0}
    for key, value in settings.items():
        out.append(''.join(f'<{key}{m}>{value}</{key}{m}>' for m in range(32)) + '\n')
    return ''.join(out)


# ---- summon signs (after the game's own server, as shadNet's broker reproduces it) ----------

def sign_data_available(adv):
    """The sign blob as a search answer carries it: byte 0x79 (the count of results the host's
    game may take) is 0 in the guest's post and must be 1 in the list, or the host drops it."""
    blob = adv.get('SummonData')
    if adv.get('SummonDataVersion') != 3 or not isinstance(blob, str):
        return blob
    try:
        raw = bytearray(base64.b64decode(blob, validate=True))
    except ValueError:
        return blob
    if len(raw) != 0xE0 or raw[0x79] != 0:
        return blob
    raw[0x79] = 1
    return base64.b64encode(bytes(raw)).decode()


def sign_matches(st, req, adv):
    if adv.get('UserId') == req.get('UserId') or adv.get('SessionId') == req.get('SessionId'):
        return False
    for key in ('SummonDataVersion', 'SummonMethod'):
        if req.get(key) is not None and req.get(key) != adv.get(key):
            return False
    if not st.anywhere:
        for key in ('AreaId', 'AreaRegionId'):
            if req.get(key) is not None and req.get(key) != adv.get(key):
                return False
    types = {f.get('SummonType') for f in req.get('SummonTypeList') or [] if isinstance(f, dict)}
    return not types or adv.get('SummonType') in types


def summon_create(st, req):
    key = f"{req.get('SessionId')}:{req.get('UserId')}"
    with st.lock:
        purge_signs(st)
        rec = st.signs.get(key)
        if rec is None or rec['state'] == 'consumed':
            st.signs[key] = {'state': 'up', 'adv': req, 'claim': None, 'at': now()}
            log.info('sign up: user %s type %s area %s', req.get('UserId'), req.get('SummonType'),
                     req.get('AreaId'))
            return None
        rec['adv'], rec['at'] = req, now()
        if rec['state'] == 'claimed':
            rec['state'] = 'delivered'
            log.info('summon delivered to user %s (claim by user %s)', req.get('UserId'),
                     rec['claim'].get('UserId'))
        if rec['state'] == 'delivered':
            return rec['claim']
        return None


def summon_search(st, req):
    with st.lock:
        purge_signs(st)
        found, near = [], 0
        for rec in sorted(st.signs.values(), key=lambda r: -r['at']):
            if rec['state'] != 'up':
                continue
            if sign_matches(st, req, rec['adv']):
                entry = dict(rec['adv'])
                entry['SummonData'] = sign_data_available(rec['adv'])
                if st.anywhere:
                    for k in ('AreaId', 'AreaRegionId', 'ChannelId', 'MatchingLevel', 'PosX', 'PosY', 'PosZ'):
                        if k in req:
                            entry[k] = req[k]
                found.append(entry)
            elif rec['adv'].get('UserId') != req.get('UserId'):
                near += 1
        limit = req.get('GetMaxCount', 20)
        if not isinstance(limit, int) or limit <= 0:
            limit = 20
        if near and not found:
            log.info('sign search by user %s: %d sign(s) up elsewhere (other area or type)',
                     req.get('UserId'), near)
        return found[:limit]


def summon_claim(st, req):
    target_user = req.get('TargetUserId')
    target_chara = req.get('TargetCharaId')
    with st.lock:
        purge_signs(st)
        best = None
        for rec in st.signs.values():
            adv = rec['adv']
            if adv.get('UserId') == req.get('UserId'):
                continue
            hit = (target_user is not None and adv.get('UserId') == target_user
                   and (target_chara is None or adv.get('CharaId') == target_chara))
            hit = hit or (target_user is None and adv.get('SessionId') == req.get('SessionId'))
            if hit and rec['state'] != 'consumed' and (best is None or rec['at'] > best['at']):
                best = rec
        if best is None:
            log.warning('summon request by user %s: no sign of user %s', req.get('UserId'), target_user)
            return None
        if best['state'] in ('claimed', 'delivered'):
            log.info('summon request repeated for user %s', best['adv'].get('UserId'))
            return best
        # 'at' stays the guest's last post: a claim must not keep a sign alive whose game is gone.
        best['state'], best['claim'], best['claimed'] = 'claimed', req, now()
        log.info('summon: user %s claims the sign of user %s', req.get('UserId'), best['adv'].get('UserId'))
        return best


def summon_remove(st, req):
    with st.lock:
        n = 0
        for rec in st.signs.values():
            adv = rec['adv']
            if req.get('SessionId') not in (None, adv.get('SessionId')):
                continue
            if req.get('UserId') not in (None, adv.get('UserId')):
                continue
            if rec['state'] != 'consumed':
                rec['state'], rec['at'] = 'consumed', now()
                n += 1
        return n


def purge_signs(st):
    t = now()
    for key in [k for k, r in st.signs.items() if t - r['at'] > SIGN_TTL]:
        del st.signs[key]
    # A summon the guest never took up (her game did not join the room) would keep the sign
    # from every later search for as long as her game runs.
    for rec in st.signs.values():
        if rec['state'] in ('claimed', 'delivered') and t - rec.get('claimed', t) > CLAIM_TTL:
            log.info('sign of user %s up again: the claim by user %s was not taken up', rec['adv'].get('UserId'),
                     rec['claim'].get('UserId'))
            rec['state'], rec['claim'] = 'up', None


# ---- matchmaking rooms (bbhost's service) --------------------------------------------------

def room_of(st, sid):
    rid = st.room_by_sid.get(sid)
    return st.rooms.get(rid) if rid else None


def close_room(st, room, reason, skip=()):
    for mid, m in room['members'].items():
        if m['OnlineId'] not in skip:
            st.push(m['OnlineId'], 'room_closed', {'RoomId': room['RoomId'], 'Reason': reason})
    st.rooms.pop(room['RoomId'], None)
    st.room_by_sid.pop(room['SessionId'], None)
    log.info('room %d closed (%s)', room['RoomId'], reason)


def remove_member(st, room, mid, reason, tell_kicker=None):
    m = room['members'].pop(mid, None)
    if m is None:
        return
    if mid == room['OwnerMemberId']:
        close_room(st, room, 'host_left' if reason == 'leave_room' else 'host_' + reason, skip=(m['OnlineId'],))
        return
    for other in room['members'].values():
        if other['OnlineId'] != tell_kicker:
            st.push(other['OnlineId'], 'room_member_left', {'RoomId': room['RoomId'], 'MemberId': mid,
                                                            'OnlineId': m['OnlineId'], 'Reason': reason})
    log.info('room %d: member %d (%s) left (%s)', room['RoomId'], mid, m['OnlineId'], reason)


def send_invite(st, host_id, guest_id):
    """bbhost's guest_invite: the guest learns the host's room and address (it seeds its peer
    table and opens its NAT toward the host; the game's own invite follows over P2P)."""
    room = next((r for r in st.rooms.values() if r['owner'] == host_id), None)
    if room is None:
        st.pending_invites[host_id] = (guest_id, now())
        log.info('summon: %s has no room yet; the invite to %s waits for it', host_id, guest_id)
        return
    p = st.peers.get(host_id, {'OnlineId': host_id})
    addr, port = reach(p)
    extra = room['extra']
    st.push(guest_id, 'guest_invite', {
        'RoomId': room['RoomId'], 'HostOnlineId': host_id, 'HostAddr': addr, 'HostPort': port,
        'HostLocalAddr': p.get('LocalAddr', ''), 'HostLocalPort': p.get('LocalPort', 0),
        'HostMappedAddr': p.get('MappedAddr', ''), 'HostMappedPort': p.get('MappedPort', 0),
        'MemberTag': extra.get('MemberTag', 1), 'HostArea': extra.get('HostArea', 0),
        'HostLevel': extra.get('HostLevel', 0), 'HostPos': extra.get('HostPos', [0, 0, 0])})


def reap(st):
    """Members whose heartbeats stopped (a crashed game) leave their rooms."""
    while True:
        time.sleep(5)
        with st.lock:
            t = now()
            for room in list(st.rooms.values()):
                for mid, m in list(room['members'].items()):
                    if t - m['beat'] > MEMBER_TIMEOUT and room['RoomId'] in st.rooms:
                        remove_member(st, room, mid, 'timeout')
            for host, (guest, at) in list(st.pending_invites.items()):
                if t - at > INVITE_WAIT:
                    del st.pending_invites[host]
            purge_signs(st)


# ---- HTTP ---------------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    server_version = 'bbcoop'
    sys_version = ''
    st = None  # set at start

    def log_message(self, fmt, *args):  # the default writes every request to stderr
        pass

    # -- plumbing

    def read_body(self):
        if self.headers.get('Transfer-Encoding', '').lower() == 'chunked':
            data = b''
            while True:
                size = int(self.rfile.readline().split(b';')[0].strip() or b'0', 16)
                if size == 0:
                    self.rfile.readline()
                    return data
                data += self.rfile.read(size)
                self.rfile.readline()
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n > 0 else b''

    def reply(self, status, body, ctype='application/json'):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, separators=(',', ':'))
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(data)

    def handle_any(self):
        url = urllib.parse.urlsplit(self.path)
        path = url.path
        query = urllib.parse.parse_qs(url.query)
        raw = self.read_body()
        body = None
        if raw:
            for enc in ('utf-8', 'utf-16'):
                try:
                    body = json.loads(raw.decode(enc))
                    break
                except (UnicodeDecodeError, ValueError):
                    continue
        try:
            status, out, ctype, quiet = self.route(path, query, raw, body)
        except Exception:
            log.exception('error on %s %s', self.command, self.path)
            status, out, ctype, quiet = 500, {'ResKind': 1, 'Error': 'server error'}, 'application/json', False
        if not quiet:
            log.info('%s %s %s <- %s | -> %d %s', self.client_address[0], self.command, self.path,
                     short(body if body is not None else raw[:200].decode('latin-1')), status,
                     short(out) if ctype == 'application/json' else f'{len(out)} bytes')
        self.reply(status, out, ctype)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = handle_any

    # -- routing

    def route(self, path, query, raw, body):
        st = self.st
        ip = self.client_address[0]
        req = body if isinstance(body, dict) else {}
        if path.endswith('/ss.info') or path.endswith('ss.info.xml'):
            host = (self.headers.get('Host') or '').rsplit(':', 1)[0] or self.server.server_address[0]
            base = self.server.public_api or f'http://{host}:{self.server.api_port}'
            # The game base64-decodes the download before parsing it (eboot 0x1ea92e0).
            return 200, base64.b64encode(build_ssinfo(base).encode()).decode(), 'text/plain', False
        p = path.strip('/')
        if p.startswith('mp/matching2/') or p.startswith('np/'):
            return self.matchmaking(p, query, req, ip)
        if p in API_BY_PATH:
            return self.game_api(API_BY_PATH[p], query, req, ip)
        if p.startswith('auth/'):
            log.warning('ACCOUNT CALL %s from %s: this server has no accounts (set require_account = false)',
                        path, ip)
            return 200, {'ResKind': 1, 'Error': 'no accounts on this server'}, 'application/json', False
        if self.command == 'PUT':  # a play-log upload (bbhost sends the S3 PUTs to <host>:18671)
            log.info('play log %s from %s (%d bytes) dropped', path, ip, len(raw))
            return 200, b'', 'text/plain', True
        if path in ('/', '/status'):
            with st.lock:
                info = {'ok': True, 'service': 'bbcoop-server', 'peers': sorted(st.peers),
                        'rooms': len(st.rooms), 'signs': sum(r['state'] == 'up' for r in st.signs.values())}
            return 200, info, 'application/json', True
        log.warning('UNKNOWN ENDPOINT %s %s from %s: %s', self.command, self.path, ip,
                    short(body if body is not None else raw[:300].decode('latin-1')))
        name = str(req.get('MessageId', '')).replace('Request', 'Response')
        return 200, {'ResKind': 0, 'MessageId': name}, 'application/json', False

    def game_api(self, name, query, req, ip):
        st = self.st
        out = {'ResKind': 0, 'MessageId': name + 'Response'}
        out.update(json.loads(json.dumps(EMPTY_PARTS.get(name, {}))))
        if name == 'Login':
            account = req.get('PlatformAccountId', req.get('OnlineId', ''))
            sid, uid = st.login(ip, account)
            out.update({'SessionId': sid, 'UserId': uid, 'UserStatus': 0})
            if 'LanguageId' in req:
                out['LanguageId'] = req['LanguageId']
            log.info('login: %s account %s -> user %d', ip, account, uid)
            gone = summon_remove(st, {'UserId': uid})  # signs of the game's last run
            if gone:
                log.info('sign down: user %d logged in again (%d removed)', uid, gone)
        elif name == 'ServerTimeGet':
            t = datetime.datetime.now(datetime.timezone.utc)
            out['ServerTime'] = t.strftime('%Y-%m-%dT%H:%M:%S')
            out['CheckTime'] = out['ServerTime']
        elif name == 'SyncCharaId':
            uid = req.get('UserId') or int((query.get('user_id') or ['0'])[0] or 0)
            count = req.get('CharaIdNum', 10)
            count = count if isinstance(count, int) and 0 < count <= 64 else 10
            out['PublishCharacterIdList'] = [{'PublishCharaId': c} for c in st.chara_ids(uid, count)]
        elif name == 'SummonDataCreate':
            claim = summon_create(st, req)
            if claim is not None:
                for k, v in claim.items():
                    if k not in ('MessageId', 'ResKind'):
                        out[k] = v
        elif name == 'SummonDataGetList':
            out = {'SummonDataList': summon_search(st, req), 'ResKind': 0,
                   'MessageId': 'SummonDataGetListResponse'}
        elif name == 'SummonDataRemove':
            n = summon_remove(st, req)
            log.info('sign down: user %s (%d removed)', req.get('UserId'), n)
        elif name == 'SummonDataSummon':
            rec = summon_claim(st, req)
            if rec is not None:
                host = st.online_id_of_user(req.get('UserId'))
                guest = st.online_id_of_user(rec['adv'].get('UserId'))
                if host and guest:
                    with st.lock:
                        send_invite(st, host, guest)
                else:
                    log.warning('summon: cannot tell the players\' online ids (host %s, guest %s); '
                                'the game\'s own invite has to do', host, guest)
        elif name in ('BloodMessCreate', 'TombMessCreate'):
            out['BloodMessId' if name == 'BloodMessCreate' else 'TombMessId'] = secrets.randbelow(10 ** 9) + 1
        return 200, out, 'application/json', False

    def matchmaking(self, p, query, req, ip):
        st = self.st
        ok = {'ResKind': 0}
        oid = req.get('OnlineId') or (query.get('OnlineId') or [''])[0]
        with st.lock:
            if p == 'np/events/poll':
                st.peers.get(oid, {})['seen'] = now()
                eid, name_or_next, payload = st.poll(oid, int(req.get('Cursor') or 0))
                if eid is None:
                    return 200, {'ResKind': 0, 'HasEvent': 0, 'NextCursor': name_or_next}, 'application/json', True
                ev = dict(payload)
                ev.update({'ResKind': 0, 'HasEvent': 1, 'EventId': eid, 'Name': name_or_next,
                           'NextCursor': eid, 'Payload': payload})
                return 200, ev, 'application/json', False
            if p == 'np/events/ack':
                st.ack(oid, int(req.get('Cursor') or 0))
                return 200, ok, 'application/json', True
            if p == 'np/signaling/resolve':
                peer = st.peers.get(oid)
                if not peer:
                    return 200, {'ResKind': 1, 'Error': 'not registered'}, 'application/json', False
                addr, port = reach(peer)
                return 200, {'ResKind': 0, 'OnlineId': oid, 'Addr': addr, 'Port': port}, 'application/json', False
            if p == 'mp/matching2/context_start':
                st.register_peer(oid, ip, req.get('SignalingAddr'), req.get('SignalingPort'),
                                 req.get('MappedAddr'), req.get('MappedPort'))
                st.events.pop(oid, None)  # a fresh client: nothing from before is news to it
                ruleset = self.headers.get('X-BBHost-Ruleset', '')
                st.peers[oid]['Ruleset'] = ruleset
                st.save_live()
                log.info('online: %s at %s (local %s:%s, mapped %s:%s, ruleset %s)', oid, ip,
                         req.get('SignalingAddr'), req.get('SignalingPort'), req.get('MappedAddr'),
                         req.get('MappedPort'), ruleset or '-')
                return 200, ok, 'application/json', False
            if p == 'mp/matching2/signaling_update':
                peer = st.peers.get(oid)
                if peer:
                    peer['MappedAddr'] = req.get('MappedAddr', '')
                    peer['MappedPort'] = int(req.get('MappedPort') or 0)
                    st.save_live()
                    room = room_of(st, req.get('SessionId'))
                    if room:
                        mid = int(req.get('MemberId') or 0)
                        for omid, m in room['members'].items():
                            if omid != mid:
                                st.push(m['OnlineId'], 'room_member_joined',
                                        dict(member_record(mid, peer), RoomId=room['RoomId']))
                return 200, ok, 'application/json', False
            if p == 'mp/matching2/create_room':
                peer = st.register_peer(oid, ip, req.get('LocalAddr'), req.get('LocalPort'),
                                        req.get('MappedAddr'), req.get('MappedPort'))
                for old in [r for r in st.rooms.values() if r['owner'] == oid]:
                    close_room(st, old, 'host_left', skip=(oid,))
                rid = st.next_room
                st.next_room += 1
                max_members = int(req.get('MaxMembers') or 5)
                room = {'RoomId': rid, 'SessionId': secrets.token_hex(12), 'owner': oid, 'OwnerMemberId': 1,
                        'MaxMembers': max_members, 'Ruleset': self.headers.get('X-BBHost-Ruleset', ''),
                        'extra': {k: req[k] for k in ('HostArea', 'HostLevel', 'HostPos', 'MemberTag') if k in req},
                        'members': {1: {'OnlineId': oid, 'beat': now()}}}
                st.rooms[rid] = room
                st.room_by_sid[room['SessionId']] = rid
                log.info('room %d created by %s (max %d)', rid, oid, max_members)
                pend = st.pending_invites.pop(oid, None)
                if pend:
                    send_invite(st, oid, pend[0])
                return 200, {'ResKind': 0, 'RoomId': rid, 'SessionId': room['SessionId'], 'MemberId': 1,
                             'OwnerMemberId': 1, 'MaxMembers': max_members}, 'application/json', False
            if p == 'mp/matching2/join_room':
                peer = st.register_peer(oid, ip, req.get('LocalAddr'), req.get('LocalPort'),
                                        req.get('MappedAddr'), req.get('MappedPort'))
                room = st.rooms.get(int(req.get('RoomId') or 0))
                if room is None:
                    return 200, {'ResKind': 1, 'Error': 'no such room'}, 'application/json', False
                mid = next((m for m, v in room['members'].items() if v['OnlineId'] == oid), None)
                if mid is None:
                    mid = next((m for m in range(2, room['MaxMembers'] + 1) if m not in room['members']), None)
                    if mid is None:
                        return 200, {'ResKind': 1, 'Error': 'room full'}, 'application/json', False
                room['members'][mid] = {'OnlineId': oid, 'beat': now()}
                members = [member_record(m, st.peers.get(v['OnlineId'], {'OnlineId': v['OnlineId']}))
                           for m, v in sorted(room['members'].items()) if m != mid]
                for m, v in room['members'].items():
                    if m != mid:
                        st.push(v['OnlineId'], 'room_member_joined', dict(member_record(mid, peer), RoomId=room['RoomId']))
                log.info('room %d: %s joined as member %d', room['RoomId'], oid, mid)
                out = {'ResKind': 0, 'RoomId': room['RoomId'], 'SessionId': room['SessionId'], 'MemberId': mid,
                       'OwnerMemberId': room['OwnerMemberId'], 'MaxMembers': room['MaxMembers'], 'Members': members}
                if room['Ruleset']:
                    out['HostRuleset'] = room['Ruleset']
                return 200, out, 'application/json', False
            if p == 'mp/matching2/leave_room':
                room = room_of(st, req.get('SessionId'))
                if room:
                    remove_member(st, room, int(req.get('MemberId') or 0), 'leave_room')
                return 200, ok, 'application/json', False
            if p == 'mp/matching2/heartbeat':
                room = room_of(st, req.get('SessionId'))
                m = room['members'].get(int(req.get('MemberId') or 0)) if room else None
                if m is None:
                    return 200, {'ResKind': 0, 'InRoom': 0}, 'application/json', False
                m['beat'] = now()
                return 200, {'ResKind': 0, 'InRoom': 1}, 'application/json', True
            if p == 'mp/matching2/kick_member':
                room = room_of(st, req.get('SessionId'))
                mid = int(req.get('MemberId') or 0)
                if room and mid in room['members'] and int(req.get('KickerMemberId') or 0) == room['OwnerMemberId']:
                    kicked = room['members'][mid]['OnlineId']
                    st.push(kicked, 'room_member_kicked', {'RoomId': room['RoomId'], 'MemberId': mid,
                                                           'OptData': req.get('OptData', '')})
                    remove_member(st, room, mid, 'kicked', tell_kicker=room['owner'])
                return 200, ok, 'application/json', False
            if p == 'mp/matching2/session_blob':
                return 200, {'ResKind': 0}, 'application/json', False
        log.warning('UNKNOWN ENDPOINT /%s from %s: %s', p, ip, short(req))
        return 200, ok, 'application/json', False


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def server_bind(self):  # HTTPServer's own asks DNS for our name: seconds on the Ally's Wi-Fi
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[:2]


# ---- STUN ---------------------------------------------------------------------------------

def stun_loop(sock):
    """Binding Request -> Binding Response with MAPPED-ADDRESS and XOR-MAPPED-ADDRESS. bbhost's
    relay HELLO attribute is ignored: no relay, so the clients send to each other directly."""
    while True:
        try:
            data, (ip, port) = sock.recvfrom(2048)
        except OSError:
            continue
        if len(data) < 20 or data[0:2] != b'\x00\x01':
            continue
        txid = data[4:20]
        a = socket.inet_aton(ip)
        mapped = struct.pack('!HHBBH4s', 0x0001, 8, 0, 1, port, a)
        cookie = txid[:4]
        xport = port ^ struct.unpack('!H', cookie[:2])[0]
        xaddr = bytes(x ^ y for x, y in zip(a, cookie))
        xmapped = struct.pack('!HHBBH4s', 0x0020, 8, 0, 1, xport, xaddr)
        attrs = mapped + xmapped
        resp = struct.pack('!HH', 0x0101, len(attrs)) + txid + attrs
        try:
            sock.sendto(resp, (ip, port))
        except OSError:
            pass


# ---- main ---------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--ssinfo-port', type=int, default=20443)
    ap.add_argument('--api-port', type=int, default=18671)
    ap.add_argument('--stun-port', type=int, default=3478)
    ap.add_argument('--public-api', default='', help='API base URL put in ss.info '
                    '(default: http://<the Host the game asked>:<api port>)')
    ap.add_argument('--anywhere', action='store_true', help='match co-op signs from any area')
    ap.add_argument('--log', default=os.path.join(HERE, 'server.log'))
    ap.add_argument('--data', default=HERE)
    a = ap.parse_args()

    fmt = logging.Formatter('%(asctime)s %(levelname)s %(message)s')
    fh = logging.handlers.RotatingFileHandler(a.log, maxBytes=4 * 1024 * 1024, backupCount=1)
    fh.setFormatter(fmt)
    sh = logging.StreamHandler(sys.stderr)
    sh.setFormatter(fmt)
    log.addHandler(fh)
    log.addHandler(sh)
    log.setLevel(logging.INFO)

    st = State(a.data, a.anywhere)
    Handler.st = st
    servers = []
    for port in (a.ssinfo_port, a.api_port):
        srv = Server((a.bind, port), Handler)
        srv.api_port = a.api_port
        srv.public_api = a.public_api.rstrip('/')
        servers.append(srv)
        threading.Thread(target=srv.serve_forever, daemon=True, name=f'http-{port}').start()
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    us.bind((a.bind, a.stun_port))
    threading.Thread(target=stun_loop, args=(us,), daemon=True, name='stun').start()
    threading.Thread(target=reap, args=(st,), daemon=True, name='reap').start()
    log.info('bbcoop-server up: ss.info tcp %d, api tcp %d, stun udp %d on %s%s', a.ssinfo_port, a.api_port,
             a.stun_port, a.bind, ' (signs match from any area)' if a.anywhere else '')
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
