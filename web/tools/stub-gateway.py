#!/usr/bin/env python3
"""Stand-in for the gateway, for developing and testing the web UI without hardware.

Serves web/dist, answers the SPEC 9 routes with plausible data and emits a real SSE stream
(gear changes, scan and commissioning progress, results, log lines).

    npm --prefix web run build
    python3 web/tools/stub-gateway.py web/dist 8099

Failure modes are switched at runtime through /stub/<name>?<args>, so one browser session can be
walked through every path the UI has to survive without a restart:

    curl 'localhost:8099/stub/set?powered=0'        bus with no voltage
    curl 'localhost:8099/stub/set?force_busy=1'     every synchronous call answers bus_busy
    curl 'localhost:8099/stub/set?sse_full=1'       every stream attempt answers 503 (4th client)
    curl 'localhost:8099/stub/set?sse_error=1'      every stream attempt answers 500
    curl 'localhost:8099/stub/set?heartbeat_s=9999' stop the heartbeat, to trip the 45 s watchdog
    curl -XPOST localhost:8099/stub/sse_kill        drop every open stream
    curl 'localhost:8099/stub/absent?12'            toggle one address off and on the bus
    curl 'localhost:8099/stub/wipe'                 empty the registry
    curl 'localhost:8099/stub/nudge'                move a level from outside the browser
    curl 'localhost:8099/stub/log'                  emit a log event
    curl 'localhost:8099/stub/set?rx_rate=20'       foreign frames per second while listening
    curl 'localhost:8099/stub/burst?600'            600 frames as fast as the socket takes them

Nothing here ships: the firmware serves these routes for real.
"""
import json, os, queue, random, re, sys, threading, time
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'web/dist')
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8099

LOCK = threading.RLock()

STATE = {
    'powered': True,
    'busy': False,
    'operation': None,
    'progress': None,
    'last_scan': int(time.time()) - 420,
    'cancel': False,
    # failure switches
    'force_busy': False,      # every synchronous call answers bus_busy
    'sse_full': False,        # every stream attempt answers 503
    'sse_error': False,       # every stream attempt answers 500
    'sse_max': 3,
    'heartbeat_s': 15,
    # M5 passive listening. Runtime only, exactly as the firmware has it: never persisted.
    'listening': False,
    'rx_rate': 4,             # foreign frames per second while listening
}

BOOT = time.time()

NAMES = {
    0: 'Kitchen ceiling', 1: 'Kitchen worktop', 2: 'Hall downlights', 3: 'Living room north',
    5: 'Living room south', 8: 'Stair landing', 12: 'Cellar strip', 17: 'Garage bay',
}

def make_gear(addr, name, present=True, level=0, groups=(), status=0, dt=(6,)):
    return {
        'addr': addr, 'name': name, 'present': present,
        'last_seen': int(time.time()) - (5 if present else 90000),
        'device_types': list(dt), 'version': '2.0',
        'level': level, 'level_pct': lvl_to_pct(level), 'on': level > 0,
        'status': {'raw': status | (0x04 if level > 0 else 0)},
        'config': {
            'min': 85, 'max': 254, 'power_on': 254, 'system_failure': 254,
            'fade_time': 4, 'fade_rate': 7, 'physical_min': 85,
            'groups': list(groups),
            'scenes': [254, 0, 128, None, None, None, None, None,
                       None, None, None, None, None, None, None, None],
        },
        'identity': {'gtin': '4052899%06d' % (addr * 137), 'serial': '00000001%02d' % addr,
                     'bank0_version': 1},
    }

def lvl_to_pct(level):
    if level <= 0:
        return 0
    return 1 + round((min(level, 254) - 1) * 99 / 253)

def pct_to_lvl(pct):
    if pct <= 0:
        return 0
    return 1 + round((pct - 1) * 253 / 99)

GEARS = {
    0: make_gear(0, NAMES[0], level=254, groups=(0,)),
    1: make_gear(1, NAMES[1], level=128, groups=(0,)),
    2: make_gear(2, NAMES[2], level=0, groups=(0, 3)),
    3: make_gear(3, NAMES[3], level=190, groups=(1,)),
    5: make_gear(5, NAMES[5], level=0, groups=(1,), status=0x02),      # lamp failure
    8: make_gear(8, NAMES[8], level=64, groups=(3,)),
    12: make_gear(12, NAMES[12], level=0, present=False, groups=(3,)),  # absent
    17: make_gear(17, NAMES[17], level=254, groups=()),
}

GROUP_NAMES = {'0': {'name': 'Kitchen'}, '1': {'name': 'Living room'}, '3': {'name': 'Service'}}

CLIENTS = []   # list of queue.Queue

def bus_doc():
    return {
        'powered': STATE['powered'], 'busy': STATE['busy'],
        'operation': STATE['operation'], 'progress': STATE['progress'],
        'gear_count': sum(1 for g in GEARS.values() if g['present']),
        'last_scan': STATE['last_scan'],
    }

def compact(g):
    return {'addr': g['addr'], 'name': g['name'], 'present': g['present'],
            'level': g['level'], 'on': g['on'], 'status': {'raw': g['status']['raw']}}

def publish(name, data):
    payload = 'event: %s\ndata: %s\n\n' % (name, json.dumps(data))
    with LOCK:
        for q in list(CLIENTS):
            try:
                q.put_nowait(payload)
            except queue.Full:
                pass

def publish_gear(g):
    publish('gear', compact(g))

def publish_bus():
    publish('bus', bus_doc())

# ------------------------------------------------------------- foreign traffic

# What another master and a couple of DALI-2 input devices put on the bus. The gateway never
# reports its own frames, so nothing here is derived from what the UI sends.
FOREIGN = [
    ('FF00', 16),    # broadcast off
    ('FF05', 16),    # broadcast recall max level
    ('FF10', 16),    # broadcast go to scene 0
    ('8312', 16),    # group 1 go to scene 2
    ('06FE', 16),    # A3 direct level 254
    ('0A80', 16),    # A5 direct level 128
    ('0790', 16),    # A3 query status
    ('11A0', 16),    # A8 query actual level
    ('A500', 16),    # special: initialise, all gear
    ('B701', 16),    # special: program short address A0
    ('A900', 16),    # special: compare
    ('2F3B', 16),    # A23 with an opcode outside part 102
    ('D204', 16),    # reserved address byte
    ('FF', 8),       # backward frames
    ('91', 8),
    ('00', 8),
    ('018100', 24),  # part 103 input device event
    ('0181FE', 24),
]

def publish_rx(frame, bits):
    publish('rx', {'frame': frame, 'bits': bits, 'ts': int((time.time() - BOOT) * 1000)})

def run_traffic():
    """Idle foreign traffic, roughly the rate a small installation with wall panels produces."""
    while True:
        rate = max(STATE['rx_rate'], 1)
        time.sleep(1.0 / rate)
        if STATE['listening'] and CLIENTS:
            publish_rx(*random.choice(FOREIGN))

def run_burst(count):
    """Faster than the UI can render, to exercise the bounded buffer and the pause."""
    for _ in range(count):
        if not STATE['listening']:
            return
        publish_rx(*random.choice(FOREIGN))
        time.sleep(0.004)

# ------------------------------------------------------------------ operations

def run_scan(deep):
    total, found = 64, 0
    STATE.update(busy=True, operation='scan', cancel=False)
    publish_bus()
    for addr in range(total):
        if STATE['cancel']:
            STATE.update(busy=False, operation='scan' if False else None, progress=None)
            publish_bus()
            publish('result', {'action': 'scan', 'ok': False, 'error': 'cancelled',
                               'message': 'stopped after %d of %d addresses' % (addr, total)})
            return
        time.sleep(0.09 if deep else 0.035)
        if addr in GEARS and GEARS[addr]['present']:
            found += 1
        if addr % 2 == 0 or addr == total - 1:
            STATE['progress'] = {'operation': 'scan', 'done': addr + 1, 'total': total,
                                 'found': found}
            publish('progress', STATE['progress'])
    STATE.update(busy=False, operation=None, progress=None, last_scan=int(time.time()))
    publish_bus()
    publish('result', {'action': 'scan', 'ok': True, 'duration_ms': 2300,
                       'data': {'found': found, 'scanned': total, 'deep': deep}})

def run_commission(mode, start_addr):
    STATE.update(busy=True, operation='commission', cancel=False)
    publish_bus()
    assigned = []
    targets = 3 if mode == 'unaddressed' else len(GEARS)
    for i in range(targets):
        if STATE['cancel']:
            STATE.update(busy=False, operation=None, progress=None)
            publish_bus()
            publish('result', {'action': 'commission', 'ok': False, 'error': 'cancelled',
                               'message': 'stopped after %d fittings' % i})
            return
        time.sleep(0.7)
        assigned.append(start_addr + i)
        STATE['progress'] = {'operation': 'commission', 'done': i + 1, 'total': targets}
        publish('progress', STATE['progress'])
        publish('log', {'level': 'info', 'msg': 'assigned A%d' % (start_addr + i)})
    STATE.update(busy=False, operation=None, progress=None, last_scan=int(time.time()))
    publish_bus()
    publish('result', {'action': 'commission', 'ok': True, 'duration_ms': 8420,
                       'data': {'assigned': len(assigned), 'addresses': assigned}})

# --------------------------------------------------------------------- server

class Handler(SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def log_message(self, fmt, *args):
        pass

    # -- helpers
    def reply(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header('content-type', 'application/json')
        self.send_header('content-length', str(len(body)))
        self.send_header('cache-control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def fail(self, error, message, status=409):
        self.reply({'ok': False, 'error': error, 'message': message}, status)

    def body(self):
        n = int(self.headers.get('content-length') or 0)
        if n == 0:
            return {}
        try:
            return json.loads(self.rfile.read(n) or b'{}')
        except Exception:
            return {}

    def guard(self):
        """The two refusals every synchronous endpoint can make."""
        if not STATE['powered']:
            self.fail('bus_unpowered', 'the DALI bus has no voltage', 503)
            return False
        if STATE['force_busy'] or STATE['busy']:
            self.fail('bus_busy', 'the bus is running another operation', 409)
            return False
        return True

    # -- routes
    def do_GET(self):
        p = self.path.split('?')[0]
        if p == '/api/events':
            return self.sse()
        if p.startswith('/stub/'):
            return self.stub()
        if p == '/api/info':
            return self.reply({
                'status': {'state': 'online', 'fw': '0.3.0', 'idf': '6.0.2',
                           'ip': '192.168.1.42', 'rssi': -58,
                           'uptime_s': 7321, 'mac': 'a0:85:e3:11:22:33'},
                'mode': 'sta', 'free_heap': 148320, 'min_free_heap': 121004,
                'hostname': 'esp-dali-gw', 'device_id': 'a085e3112233',
                'reset_reason': 'POWERON',
                'build': {'version': '0.3.0', 'date': '2026-09-14', 'idf': '6.0.2',
                          'target': 'esp32c6', 'sha': '9f3c1ab'}})
        if p == '/api/config':
            return self.reply({
                'schema': 1,
                'device': {'name': 'Hall gateway', 'hostname': 'esp-dali-gw',
                           'timezone': 'Europe/Paris'},
                'wifi': {'ssid': 'Chantier', 'password': '***',
                         'static': {'enabled': False, 'ip': '', 'mask': '', 'gw': '', 'dns': ''},
                         'ap_password': '***', 'fallback_ap_timeout_s': 120},
                'mqtt': {'enabled': True, 'uri': 'mqtt://192.168.1.10', 'username': 'gw',
                         'password': '***', 'client_id': 'esp-dali-gw',
                         'base_topic': 'dali_gw/a085e3', 'keepalive_s': 60, 'qos': 1,
                         'retain_state': True,
                         'ha_discovery': {'enabled': True, 'prefix': 'homeassistant'}},
                'http': {'auth': {'enabled': False, 'username': '', 'password': '***'}},
                'dali': {'tx_gpio': 14, 'rx_gpio': 5, 'invert_tx': False, 'invert_rx': False,
                         'poll_interval_s': 30, 'scan_on_boot': True, 'identify_blink_ms': 500},
                'led': {'enabled': True, 'gpio': 8, 'brightness': 40},
                'gears': {str(a): {'name': g['name']} for a, g in GEARS.items()},
                'groups': GROUP_NAMES})
        if p == '/api/bus':
            return self.reply(bus_doc())
        if p == '/api/gears':
            return self.reply({'gears': [compact(g) for g in
                                         sorted(GEARS.values(), key=lambda g: g['addr'])]})
        m = re.fullmatch(r'/api/gears/(\d+)', p)
        if m:
            g = GEARS.get(int(m.group(1)))
            if g is None:
                return self.fail('not_present', 'no such short address', 404)
            return self.reply(g)
        return super().do_GET()

    def do_PATCH(self):
        p = self.path.split('?')[0]
        b = self.body()
        m = re.fullmatch(r'/api/gears/(\d+)', p)
        if m:
            g = GEARS.get(int(m.group(1)))
            if g is None:
                return self.fail('not_present', 'no such short address', 404)
            g['name'] = b.get('name', g['name'])
            publish_gear(g)
            return self.reply({'ok': True, 'action': 'rename'})
        m = re.fullmatch(r'/api/groups/(\d+)', p)
        if m:
            GROUP_NAMES[m.group(1)] = {'name': b.get('name', '')}
            return self.reply({'ok': True, 'action': 'rename'})
        return self.fail('invalid_arg', 'unknown path', 404)

    def do_POST(self):
        p = self.path.split('?')[0]
        b = self.body()
        if p.startswith('/stub/'):
            return self.stub()

        if p == '/api/bus/scan':
            if STATE['busy']:
                return self.fail('bus_busy', 'an operation is already running')
            if not STATE['powered']:
                return self.fail('bus_unpowered', 'the DALI bus has no voltage', 503)
            threading.Thread(target=run_scan, args=(bool(b.get('deep')),), daemon=True).start()
            return self.reply({'ok': True, 'action': 'scan', 'data': {'started': True}}, 202)

        if p == '/api/bus/commission':
            if b.get('mode') == 'all' and not b.get('confirm'):
                return self.fail('invalid_arg', 're-addressing everything needs confirm:true', 400)
            if STATE['busy']:
                return self.fail('bus_busy', 'an operation is already running')
            threading.Thread(target=run_commission,
                             args=(b.get('mode', 'unaddressed'), int(b.get('start_addr', 0))),
                             daemon=True).start()
            return self.reply({'ok': True, 'action': 'commission', 'data': {'started': True}}, 202)

        if p == '/api/bus/cancel':
            STATE['cancel'] = True
            return self.reply({'ok': True, 'action': 'cancel'})

        if p == '/api/bus/check':
            publish_bus()
            return self.reply({'ok': True, 'action': 'bus_check',
                               'data': {'powered': STATE['powered'], 'replies': True}})

        if p == '/api/bus/monitor':
            STATE['listening'] = bool(b.get('enabled'))
            return self.reply({'ok': True, 'listening': STATE['listening']})

        if p == '/api/bus/raw':
            if not self.guard():
                return
            frame = str(b.get('frame', ''))
            if not re.fullmatch(r'[0-9a-fA-F]{4}|[0-9a-fA-F]{6}', frame):
                return self.fail('invalid_arg', 'a frame is 4 or 6 hex digits', 400)
            reply = random.randint(0, 254) if b.get('expect_reply') else None
            return self.reply({'ok': True, 'action': 'raw',
                               'data': {'reply': reply, 'raw_frame': frame.upper()}})

        if p == '/api/bus/query':
            if not self.guard():
                return
            g = GEARS.get(int(b.get('addr', -1)))
            if g is None:
                return self.fail('not_present', 'nothing answers on that address', 404)
            if b.get('query') == 'status':
                value = g['status']['raw']
            elif b.get('query') == 'actual_level':
                value = g['level']
            else:
                value = random.randint(0, 254)
            return self.reply({'ok': True, 'action': 'query',
                               'data': {'reply': value, 'raw_frame': '%02XA0' % (g['addr'] * 2)}})

        m = re.fullmatch(r'/api/gears/(\d+)/set', p)
        if m:
            if not self.guard():
                return
            g = GEARS.get(int(m.group(1)))
            if g is None or not g['present']:
                return self.fail('not_present', 'nothing answers on that address', 404)
            apply_set(g, b)
            publish_gear(g)
            return self.reply({'ok': True, 'action': 'set_level'})

        m = re.fullmatch(r'/api/groups/(\d+)/set', p)
        if m:
            if not self.guard():
                return
            n = int(m.group(1))
            for g in GEARS.values():
                if n in g['config']['groups'] and g['present']:
                    apply_set(g, b)
                    publish_gear(g)
            return self.reply({'ok': True, 'action': 'set_level'})

        if p == '/api/broadcast/set':
            if not self.guard():
                return
            for g in GEARS.values():
                if g['present']:
                    apply_set(g, b)
                    publish_gear(g)
            return self.reply({'ok': True, 'action': 'set_level'})

        m = re.fullmatch(r'/api/gears/(\d+)/configure', p)
        if m:
            if not self.guard():
                return
            g = GEARS.get(int(m.group(1)))
            if g is None:
                return self.fail('not_present', 'nothing answers on that address', 404)
            report = {}
            for key in ('min', 'max', 'power_on', 'system_failure', 'fade_time', 'fade_rate'):
                if key in b:
                    g['config'][key] = b[key]
                    report[key] = 'ok'
            for index, value in (b.get('scene') or {}).items():
                g['config']['scenes'][int(index)] = value
                report['scene%s' % index] = 'ok'
            group = b.get('group') or {}
            for n in group.get('add', []):
                if n not in g['config']['groups']:
                    g['config']['groups'].append(n)
            g['config']['groups'] = sorted(
                n for n in g['config']['groups'] if n not in group.get('remove', []))
            if group:
                report['groups'] = 'ok'
            publish_gear(g)
            return self.reply({'ok': True, 'action': 'configure', 'data': report})

        m = re.fullmatch(r'/api/gears/(\d+)/identify', p)
        if m:
            if not self.guard():
                return
            return self.reply({'ok': True, 'action': 'identify'})

        m = re.fullmatch(r'/api/gears/(\d+)/address', p)
        if m:
            if not self.guard():
                return
            old, new = int(m.group(1)), int(b.get('new_addr', -1))
            if new in GEARS and GEARS[new]['present'] and new != old:
                return self.fail('address_in_use', 'A%d already answers on this bus' % new)
            g = GEARS.pop(old, None)
            if g is None:
                return self.fail('not_present', 'nothing answers on that address', 404)
            g['addr'] = new
            GEARS[new] = g
            publish_gear(g)
            publish_bus()
            return self.reply({'ok': True, 'action': 'set_short_address',
                               'data': {'addr': new}})

        m = re.fullmatch(r'/api/gears/(\d+)/remove_address', p)
        if m:
            if not self.guard():
                return
            g = GEARS.get(int(m.group(1)))
            if g is not None:
                g['present'] = False
                g['status']['raw'] |= 0x40
                publish_gear(g)
                publish_bus()
            return self.reply({'ok': True, 'action': 'remove_short_address'})

        return self.fail('invalid_arg', 'unknown path', 404)

    # -- SSE
    def sse(self):
        if STATE['sse_error']:
            return self.fail('internal', 'the event stream is broken', 500)
        if STATE['sse_full'] or len(CLIENTS) >= STATE['sse_max']:
            return self.fail('internal', 'too many event listeners', 503)
        q = queue.Queue(maxsize=200)
        with LOCK:
            CLIENTS.append(q)
        self.send_response(200)
        self.send_header('content-type', 'text/event-stream')
        self.send_header('cache-control', 'no-cache, no-transform')
        self.send_header('connection', 'keep-alive')
        self.end_headers()
        # An SSE body has no length: the client only learns the stream ended when the socket does,
        # and send_response() has just reset this to False for HTTP/1.1.
        self.close_connection = True
        last_beat = time.time()
        try:
            self.wfile.write(('event: bus\ndata: %s\n\n' % json.dumps(bus_doc())).encode())
            self.wfile.flush()
            while True:
                if q not in CLIENTS:      # killed by /stub/sse_kill
                    return
                try:
                    chunk = q.get(timeout=1.0)
                    self.wfile.write(chunk.encode())
                    self.wfile.flush()
                except queue.Empty:
                    pass
                if time.time() - last_beat >= STATE['heartbeat_s']:
                    last_beat = time.time()
                    self.wfile.write(b': ping\n\n')
                    self.wfile.flush()
        except Exception:
            return
        finally:
            with LOCK:
                if q in CLIENTS:
                    CLIENTS.remove(q)

    # -- control plane
    def stub(self):
        name = self.path.split('?')[0][len('/stub/'):]
        arg = self.path.split('?')[1] if '?' in self.path else ''
        if name == 'set':
            for pair in arg.split('&'):
                if '=' not in pair:
                    continue
                k, v = pair.split('=', 1)
                if k in STATE:
                    STATE[k] = (v == '1') if v in ('0', '1') else int(v)
            publish_bus()
        elif name == 'sse_kill':
            with LOCK:
                CLIENTS.clear()
        elif name == 'absent':
            g = GEARS.get(int(arg or 12))
            if g:
                g['present'] = not g['present']
                publish_gear(g)
                publish_bus()
        elif name == 'nudge':
            g = GEARS[0]
            g['level'] = 254 if g['level'] < 128 else 40
            g['level_pct'] = lvl_to_pct(g['level'])
            g['on'] = g['level'] > 0
            g['status']['raw'] = (g['status']['raw'] & ~0x04) | (0x04 if g['on'] else 0)
            publish_gear(g)
        elif name == 'wipe':
            GEARS.clear()
            publish_bus()
        elif name == 'burst':
            threading.Thread(target=run_burst, args=(int(arg or 600),), daemon=True).start()
        elif name == 'log':
            publish('log', {'level': 'warn', 'msg': 'no reply from A5 (QUERY STATUS)'})
        return self.reply({'ok': True, 'state': {k: STATE[k] for k in
                                                 ('powered', 'busy', 'force_busy', 'sse_full',
                                                  'heartbeat_s', 'listening', 'rx_rate')},
                           'clients': len(CLIENTS)})


def apply_set(g, b):
    if 'level_pct' in b:
        g['level'] = pct_to_lvl(int(b['level_pct']))
    elif 'level' in b:
        g['level'] = int(b['level'])
    elif 'on' in b:
        g['level'] = 254 if b['on'] else 0
    g['level_pct'] = lvl_to_pct(g['level'])
    g['on'] = g['level'] > 0
    g['status']['raw'] = (g['status']['raw'] & ~0x04) | (0x04 if g['on'] else 0)
    # A real DALI transaction is ~60 ms; the synchronous endpoints wait for it.
    time.sleep(0.06)


if __name__ == '__main__':
    threading.Thread(target=run_traffic, daemon=True).start()
    print('stub gateway on http://127.0.0.1:%d serving %s' % (PORT, ROOT))
    ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
