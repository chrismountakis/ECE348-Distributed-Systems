import socket
import struct
import threading
import queue
import time
import json
import os

PING_SVC         = -1
ERROR_SVC        = -2
REPLY_ACK_SVC    = -3
CANCEL_SVC       = -4
LOAD_BALANCE_SVC = -5
DISCOVERY_SVC    = -6

HEADER_FMT  = "!iIIi"  # svcid, client_id, req_id, length
HEADER_SIZE = struct.calcsize(HEADER_FMT)

MAX_ATTEMPTS    = 3
TIMEOUT         = 3.0
LB_TIMEOUT      = 0.5
DISCOVERY_PORT  = 9999

_s = threading.local()

# Public API

def init(config_file=None, state_file="client_state.json"):
    # Initialize all state for THIS thread
    _s.state_file = state_file
    _s.packet_queue = queue.Queue()
    _s.active = threading.Event()
    _s.servers = []
    _s.server_ids = {}
    _s.req_id = 0
    _s.last_done_id = 0
    
    _s.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    _s.sock.settimeout(0.5)

    _s.servers = _load_servers(config_file) if config_file else _discover_servers()
    if not _s.servers:
        raise ValueError("No servers found")

    _recover_state()

    dispatcher_state = {
        'sock': _s.sock,
        'packet_queue': _s.packet_queue,
        'active': _s.active,
        'server_ids': _s.server_ids,
    }
    _s.dispatcher = threading.Thread(
        target=_dispatcher_loop, 
        args=(dispatcher_state,),
        daemon=True
    )
    _s.dispatcher.start()

    print(f"[Client] Initialized with {len(_s.servers)} server(s)")

def doRequestReply(svc_id, payload):
    _s.active.set()
    _drain_queue()

    try:
        server = _select_server(svc_id)
        _drain_queue()

        _s.req_id += 1
        rid = _s.req_id
        cid = _s.server_ids.get(server, 0)

        _save_pending(server, rid)
        packet = _pack_header(svc_id, cid, rid, len(payload)) + payload

        # Handshake
        cid = _handshake(server, packet, rid, cid)
        if cid is None:
            return "Error: Server failed to ACK"

        # Get result
        return _wait_result(server, rid, cid)

    finally:
        _s.active.clear()


# Helper functions

def _pack_header(svc, cid, rid, length):
    return struct.pack(HEADER_FMT, svc, cid, rid, length)

def _unpack_header(data):
    return struct.unpack(HEADER_FMT, data[:HEADER_SIZE])

def _send_ack(server, cid, rid):
    _s.sock.sendto(_pack_header(REPLY_ACK_SVC, cid, rid, 0), server)

def _recv(timeout):
    try:
        return _s.packet_queue.get(timeout=timeout)
    except queue.Empty:
        raise socket.timeout()

def _drain_queue():
    while not _s.packet_queue.empty():
        try:
            _s.packet_queue.get_nowait()
        except queue.Empty:
            break

def _handshake(server, packet, rid, cid):
    """
    Send request until server ACKs with PING.
    Returns updated client_id or None on failure.
    """
    for attempt in range(MAX_ATTEMPTS):
        print(f"[{rid}] Attempt {attempt + 1}")
        _s.sock.sendto(packet, server)

        deadline = time.time() + TIMEOUT
        while time.time() < deadline:
            try:
                data, _ = _recv(deadline - time.time())
                svc, res_cid, res_rid, _ = _unpack_header(data)

                # Stale - ACK and continue waiting
                if res_rid < rid:
                    if svc == REPLY_ACK_SVC:
                        _send_ack(server, cid, res_rid)
                    continue

                # Server ACK
                if res_rid == rid and svc == PING_SVC:
                    if cid == 0 and res_cid != 0:
                        _save_client_id(server, res_cid)
                        cid = res_cid
                    _s.sock.sendto(data, server)  # Pong
                    print(f"[{rid}] ACK received")
                    return cid

                # Early error
                if svc == ERROR_SVC:
                    _finalize(rid)
                    return None

            except socket.timeout:
                break

    return None

def _wait_result(server, rid, cid):
    """Wait for reply, handle heartbeats."""
    missed = 0

    while missed < MAX_ATTEMPTS:
        deadline = time.time() + TIMEOUT
        got_valid = False

        while time.time() < deadline:
            try:
                data, _ = _recv(deadline - time.time())
                svc, _, res_rid, length = _unpack_header(data)

                # Stale
                if res_rid < rid:
                    if svc == REPLY_ACK_SVC:
                        _send_ack(server, cid, res_rid)
                    continue

                if res_rid != rid:
                    continue

                got_valid = True
                missed = 0

                if svc == PING_SVC:
                    _s.sock.sendto(data, server)

                elif svc == REPLY_ACK_SVC:
                    _send_ack(server, cid, rid)
                    _finalize(rid)
                    payload = data[HEADER_SIZE : HEADER_SIZE + length]
                    result = list(struct.unpack(f"!{length}?", payload))
                    print(f"[{rid}] Success")
                    return result

                elif svc == ERROR_SVC:
                    _finalize(rid)
                    return "Error: Server error"

                break

            except socket.timeout:
                break

        if not got_valid:
            missed += 1
            print(f"[{rid}] Timeout ({missed}/{MAX_ATTEMPTS})")

    return "Error: Connection lost"

# Load Balancing

def _select_server(svc_id):
    """Pick server with lowest pending; break ties with lowest active."""
    if len(_s.servers) == 1:
        return _s.servers[0]

    packet = _pack_header(LOAD_BALANCE_SVC, 0, svc_id, 0)
    for server in _s.servers:
        _s.sock.sendto(packet, server)

    results = {}
    deadline = time.time() + LB_TIMEOUT

    while len(results) < len(_s.servers) and time.time() < deadline:
        try:
            data, addr = _recv(deadline - time.time())
            svc, active, pending, _ = _unpack_header(data)

            if svc == LOAD_BALANCE_SVC:
                results[addr] = (pending, active)
                print(f"  [LB-Update] Server {addr}: {pending} pending, {active} active")
            else:
                _s.packet_queue.put((data, addr))

        except (socket.timeout, queue.Empty):
            break

    if not results:
        raise ConnectionError("No servers responded")

    # Comparison: first by pending, then by active as tiebreaker
    best = min(results, key=results.get)
    print(f"  [LB] Selected {best}")
    return best

# Server Discovery

def _load_servers(path):
    servers = []
    with open(path, 'r') as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                servers.append((parts[0], int(parts[1])))
    return servers

def _discover_servers():
    print("[Discovery] Broadcasting...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(1.0)

    try:
        sock.sendto(_pack_header(DISCOVERY_SVC, 0, 0, 0), ('<broadcast>', DISCOVERY_PORT))
        found = []
        while True:
            try:
                data, addr = sock.recvfrom(1024)
                svc, _, _, _ = _unpack_header(data)
                if svc == DISCOVERY_SVC and addr not in found:
                    found.append(addr)
                    print(f"  Found: {addr}")
            except socket.timeout:
                break
        return found
    finally:
        sock.close()

def _dispatcher_loop(state):
    """Route packets: queue if active, ACK stale if idle."""
    sock = state['sock']
    packet_queue = state['packet_queue']
    active = state['active']
    server_ids = state['server_ids']
    
    while True:
        try:
            data, addr = sock.recvfrom(4096)
            svc, _, rid, _ = _unpack_header(data)

            if active.is_set():
                packet_queue.put((data, addr))
            elif svc == REPLY_ACK_SVC:
                cid = server_ids.get(addr, 0)
                sock.sendto(_pack_header(REPLY_ACK_SVC, cid, rid, 0), addr)

        except socket.timeout:
            continue
        except Exception as e:
            print(f"[Dispatcher] {e}")

# Persistent Memory  

def _read_state():
    try:
        if os.path.exists(_s.state_file):
            with open(_s.state_file) as f:
                return json.load(f)
    except:
        pass
    return {}

def _write_state(state):
    with open(_s.state_file, 'w') as f:
        json.dump(state, f)


def _save_pending(server, rid):
    state = _read_state()
    state['pending'] = {'ip': server[0], 'port': server[1], 'rid': rid}
    _write_state(state)


def _finalize(rid):
    _s.last_done_id = rid
    state = _read_state()
    state.pop('pending', None)
    _write_state(state)

def _save_client_id(server, cid):
    _s.server_ids[server] = cid
    state = _read_state()
    servers = state.setdefault('servers', {})
    servers[f"{server[0]}:{server[1]}"] = cid
    _write_state(state)

def _recover_state():
    state = _read_state()

    # Restore client IDs
    for key, cid in state.get('servers', {}).items():
        ip, port = key.rsplit(':', 1)
        _s.server_ids[(ip, int(port))] = cid

    # Cancel pending request from crash
    pending = state.get('pending')
    if pending:
        server = (pending['ip'], pending['port'])
        rid = pending['rid']
        cid = _s.server_ids.get(server, 0)
        print(f"[Recovery] Cancelling {rid}")
        _s.sock.sendto(_pack_header(CANCEL_SVC, cid, rid, 0), server)
        _s.req_id = rid
        _finalize(rid)