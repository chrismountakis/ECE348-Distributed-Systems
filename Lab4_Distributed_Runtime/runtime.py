import threading
import queue
import time
import socket
import struct
import json
import sys
from program_state import SimpleScriptState
from interpreter import step
from tuple_space import TupleSpace

import math
import random

MULTICAST_GROUP   = '239.0.0.1'
MULTICAST_PORT    = 10000   # discovery

LB_CEIL           = 'ceil'
LB_FLOOR1         = 'floor+1'
LB_THRESHOLD_MODE = LB_CEIL  

class Runtime:
    def __init__(self, tcp_port: int):
        self.threads       = {}                 # tid -> SimpleScriptState
        self.next_tid      = 0
        self.cmd_queue     = queue.Queue()
        self.tuple_space   = TupleSpace()
        self.running       = True
        self.tcp_port      = tcp_port           
        self.my_ip         = self._get_local_ip()
        self.peers         = set()              # peers: (ip, port)
        self.peers_lock    = threading.Lock()
        self.registry      = []                 # list of blocked-thread registrations from peers
        self.registry_lock = threading.Lock()
        self._deadlock_warned = False

    # -- Network --
    def _get_local_ip(self) -> str:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(('8.8.8.8', 80))
            return s.getsockname()[0]
        finally:
            s.close()
    
    def _send_multicast(self, msg: dict):
        # Used only for discovery (HELLO / HELLO_REPLY).
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP) as s:
            s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
            s.sendto(json.dumps(msg).encode(), (MULTICAST_GROUP, MULTICAST_PORT))

    def _send_tcp(self, ip: str, port: int, msg: dict, read_response: bool = True) -> dict | None:
        """
        Send msg as newline-terminated JSON over TCP.
        If read_response is True, reads and returns the JSON reply; otherwise returns None.
        """
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(5.0)
                s.connect((ip, port))
                s.sendall((json.dumps(msg) + '\n').encode())
                if not read_response:
                    return None
                data = b''
                while b'\n' not in data:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                return json.loads(data.decode().strip()) if data else None
        except Exception:
            return None

    # -- Peer management --
    def _add_peer(self, ip: str, port: int):
        if (ip, port) == (self.my_ip, self.tcp_port):
            return
        with self.peers_lock:
            if (ip, port) in self.peers:
                return
            self.peers.add((ip, port))
            print(f"[Discovery] Peer found: {ip}:{port}")
        # register already blocked threads with new peer 
        threading.Thread(target=self._register_blocked_with, args=(ip, port), daemon=True).start()

    def _register_blocked_with(self, ip: str, port: int):
        for tid, state in list(self.threads.items()):
            if not state.registered or state.block_op is None:
                continue
            self._register_with_peers(tid, state, peers=[(ip, port)])

    # —- Background loops —-
    def discovery_loop(self):
        """
        Listen for HELLO / HELLO_REPLY on the multicast group.
        HELLO       — new peer joined; add it, reply so it learns about us.
        HELLO_REPLY — existing peer responding; just add it.
        Both replies go via multicast so all processes on the same machine
        receive them correctly.
        """
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        sock.bind(('', MULTICAST_PORT))
        mreq = struct.pack('4sL', socket.inet_aton(MULTICAST_GROUP), socket.INADDR_ANY)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        sock.settimeout(1.0)

        while self.running:
            try:
                data, addr = sock.recvfrom(1024)
                msg         = json.loads(data.decode())
                sender_ip   = addr[0]
                sender_port = msg.get('port')
                if (sender_ip, sender_port) == (self.my_ip, self.tcp_port):
                    continue                         # own message — ignore
                self._add_peer(sender_ip, sender_port)
                if msg.get('type') == 'HELLO':
                    self._send_multicast({'type': 'HELLO_REPLY', 'port': self.tcp_port})
            except socket.timeout:
                continue
            except Exception:
                continue

        sock.close()

    def _tcp_server(self):
        """
        Accept incoming TCP connections from peers.
        Each connection is handled in its own daemon thread
        """
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(('', self.tcp_port))
        srv.listen(10)
        srv.settimeout(1.0)

        while self.running:
            try:
                conn, _ = srv.accept()
                threading.Thread(
                    target=self._handle_connection,
                    args=(conn,),
                    daemon=True
                ).start()
            except socket.timeout:
                continue

        srv.close()

    # Scheduler loop
    def scheduler_loop(self):
        while self.running:
            while not self.cmd_queue.empty():
                try:
                    self.handle_command(self.cmd_queue.get_nowait())
                except queue.Empty:
                    break

            if not self.threads:
                time.sleep(0.01)
                continue

            finished = []
            for tid, state in self.threads.items():
                if state.finished:
                    finished.append(tid)
                    continue

                if state.sleep_until is not None:
                    if time.time() < state.sleep_until:
                        continue
                    state.sleep_until = None

                pc_before = state.pc
                step(state, thread_id=tid, tuple_space=self.tuple_space)

                if state.finished:
                    label = "ERROR" if state.error else "Finished"
                    suffix = f": {state.error}" if state.error else ""
                    print(f"[Thread {tid}] {label}{suffix}")
                    finished.append(tid)
                    continue

                # OUT: Notify remote peers whose registrations match.
                if state.last_out is not None:
                    out_tuple      = state.last_out
                    state.last_out = None
                    threading.Thread(
                        target=self._notify_peers_for_out,
                        args=(out_tuple,),
                        daemon=True
                    ).start()

                # Thread is blocked on RD/IN.
                # Register with peers once
                if state.pc == pc_before and not state.registered:
                    state.registered = True
                    threading.Thread(
                        target=self._register_with_peers,
                        args=(tid, state),
                        daemon=True
                    ).start()

                # Thread unblocked (PC advanced) while registered.
                # Cancel the registrations stored on peers.
                if state.pc != pc_before and state.needs_cancel:
                    pattern             = state.block_pattern
                    state.needs_cancel  = False
                    state.block_pattern = None
                    state.block_op      = None
                    threading.Thread(
                        target=self._send_cancel_to_peers,
                        args=(pattern, tid),
                        daemon=True
                    ).start()

            for tid in finished:
                del self.threads[tid]

            time.sleep(0.001) # Small sleep for clear output

    def load_balancer_loop(self, min_interval: float = 3.0, max_interval: float = 5.0,
                         only_running: bool = False):
        while self.running:
            time.sleep(random.uniform(min_interval, max_interval)) 
            self._load_balance(only_running)

    # —- Message handling —-
    def _handle_connection(self, conn: socket.socket):
        """
        Read one newline-terminated JSON message from the connection
        and dispatch to the appropriate handler.
        """
        try:
            data = b''
            while b'\n' not in data:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                data += chunk
            msg = json.loads(data.decode().strip())
            t = msg.get('type')
            if   t == 'REGISTER':   self._handle_register(msg, conn)
            elif t == 'NOTIFY':     self._handle_notify(msg)
            elif t == 'QUERY':      self._handle_query(msg, conn)
            elif t == 'MIGRATE':    self._handle_migrate(msg, conn)
            elif t == 'COUNT':      self._handle_count(msg, conn)
            elif t == 'REMOTE_OUT': self._handle_remote_out(msg)
            elif t == 'CANCEL':     self._handle_cancel(msg)
            elif t == 'GOODBYE':    self._handle_goodbye(msg)
        except Exception:
            pass
        finally:
            conn.close()

    def _handle_register(self, msg: dict, conn: socket.socket):
        """
        A peer's thread is blocked on RD/IN and asking for a match.

        Checks local tuple space:
            Match found for RD: reply with the tuple.
            Match found for IN: reply found=True - requester will QUERY to remove it.
            No match: store the registration to NOTIFY on a future OUT,
              and reply {found: false}.
        """
        op      = msg['op']
        pattern = msg['pattern']
        ip      = msg['ip']
        port    = msg['port']
        tid     = msg['tid']

        # Always rd, never remove during REGISTER.
        matched = self.tuple_space.rd(pattern)

        if matched is not None:
            response = {'found': True, 'tuple': list(matched)} if op == 'RD' else {'found': True}
            print(f"  [NET] REGISTER from {ip}:{port} tid={tid} op={op} pattern={pattern} -> found")
        else:
            with self.registry_lock:
                self.registry.append({
                    'op': op, 'pattern': pattern,
                    'ip': ip, 'port': port, 'tid': tid,
                })
            response = {'found': False}
            print(f"  [NET] REGISTER from {ip}:{port} tid={tid} op={op} pattern={pattern} -> stored")

        conn.sendall((json.dumps(response) + '\n').encode())

    def _handle_notify(self, msg: dict):
        """
        Peer did OUT and found matching tuple.

        RD: tuple is included in the message. RD never removes the tuple.

        IN: QUERY the peer to atomically remove from tuple space. 
            A separate thread handles this to avoid blocking the TCP server.
        """
        op  = msg['op']
        tid = msg['tid']

        state = self.threads.get(tid)
        # Ignore if thread finished or already satisfied.
        if state is None or state.finished or state.pending_tuple is not None:
            return

        if op == 'RD':
            print(f"  [NET] NOTIFY tid={tid} op=RD tuple={tuple(msg['tuple'])} -> unblocking")
            state.pending_tuple = tuple(msg['tuple'])

        elif op == 'IN':
            print(f"  [NET] NOTIFY tid={tid} op=IN -> querying {msg['from_ip']}:{msg['from_port']}")
            threading.Thread(
                target=self._query_peer,
                args=(state, msg['from_ip'], msg['from_port'], msg['pattern']),
                daemon=True
            ).start()

    def _handle_query(self, msg: dict, conn: socket.socket):
        """
        QUERY is only sent for IN (RD delivers the tuple directly via NOTIFY/REGISTER).
        The tuple_space lock guarantees only one concurrent QUERY wins.
        """
        pattern = msg['pattern']
        matched = self.tuple_space.in_(pattern)
        print(f"  [NET] QUERY pattern={pattern} -> {'found ' + str(matched) if matched else 'not found'}")
        response = {'tuple': list(matched) if matched is not None else None}
        conn.sendall((json.dumps(response) + '\n').encode())

    def _handle_migrate(self, msg: dict, conn: socket.socket):
        """Handle incoming migration of a thread from another peer."""
        tid   = msg['tid']                                                        
        state = SimpleScriptState.from_dict(msg)              
        self.threads[tid] = state                                                 
        conn.sendall((json.dumps({'ok': True}) + '\n').encode())
        print(f"[Thread {tid}] Arrived from {msg.get('from_ip')} {msg.get('from_port')}")

    def _handle_count(self, msg: dict, conn: socket.socket):
        only_running = msg.get('only_running', False)
        active = [s for s in self.threads.values() if not s.finished]
        if only_running:
            count = sum(1 for s in active if s.sleep_until is None and s.block_op is None)
        else:
            count = len(active)
        all_blocked = bool(active) and not any(s for s in active if s.block_op is None)
        conn.sendall((json.dumps({
            'count':       count,
            'all_blocked': all_blocked,
            'has_threads': bool(active),
        }) + '\n').encode())

    def _handle_remote_out(self, msg: dict):
        tup = tuple(msg['tuple'])
        self.tuple_space.out(tup)
        threading.Thread(target=self._notify_peers_for_out, args=(tup,), daemon=True).start()

    def _handle_cancel(self, msg: dict):
        """
        A peer's thread was satisfied — remove the registration from registry
        so we don't keep sending NOTIFY .
        """
        ip      = msg['ip']
        port    = msg['port']
        pattern = msg['pattern']
        tid     = msg['tid']
        print(f"  [NET] CANCEL from {ip}:{port} tid={tid} pattern={pattern}")
        with self.registry_lock:
            self.registry = [
                r for r in self.registry
                if not (r['ip'] == ip and r['port'] == port
                        and r['pattern'] == pattern and r['tid'] == tid)
            ]

    def _handle_goodbye(self, msg: dict):
        """
        A peer is leaving voluntarily. Remove it from peers and 
        drop any registry entries it registered here.
        """
        ip   = msg['ip']
        port = msg['port']
        with self.peers_lock:
            self.peers.discard((ip, port))
        with self.registry_lock:
            self.registry = [r for r in self.registry
                             if not (r['ip'] == ip and r['port'] == port)]
        print(f"[Discovery] Peer left: {ip}:{port}")

    # —- Distributed ops -—
    def _register_with_peers(self, tid: str, state, peers: list | None = None):
        """
        Send REGISTER to all peers (or a specific subset if peers is given).
        Each peer checks its local tuple space and responds:
            RD: {found: true, tuple}: peer had a match
            IN: {found: true}:        peer had a match - send QUERY to remove it
            {found: false}:           peer stored the registration, will NOTIFY on future OUT.

        Contact all peers so that peers which
        store the registration can be cancelled later.
        If multiple peers respond with a match, query first one.
        """
        if peers is None:
            with self.peers_lock:
                peers = list(self.peers)

        found_ip   = None
        found_port = None
        result_lock = threading.Lock()

        def contact_peer(ip, port):
            nonlocal found_ip, found_port
            if state.block_op is None:
                return
            response = self._send_tcp(ip, port, {
                'type':    'REGISTER',
                'op':      state.block_op,
                'pattern': state.block_pattern,
                'ip':      self.my_ip,
                'port':    self.tcp_port,
                'tid':     tid,
            })
            if not (response and response.get('found')):
                return
            with result_lock:
                if state.block_op == 'RD' and state.pending_tuple is None:
                    state.pending_tuple = tuple(response['tuple'])
                elif state.block_op == 'IN' and found_ip is None:
                    found_ip   = ip
                    found_port = port

        threads = [threading.Thread(target=contact_peer, args=(ip, port), daemon=True)
                   for ip, port in peers]
        for t in threads: t.start()
        for t in threads: t.join()

        # For IN: QUERY the first peer that had the tuple to atomically remove it.
        if found_ip is not None and state.pending_tuple is None:
            self._query_peer(state, found_ip, found_port, state.block_pattern)

    def _query_peer(self, state, ip: str, port: int, pattern: list):
        """
        Send a QUERY to the peer that sent us NOTIFY for IN. 
        If the peer no longer has the tuple (race), we wait 
        for the next NOTIFY.
        """
        if state.pending_tuple is not None:
            return     # already satisfied by a concurrent NOTIFY

        response = self._send_tcp(ip, port, {
            'type': 'QUERY', 'pattern': pattern,
        })

        if response and response.get('tuple') is not None:
            if state.pending_tuple is None:       # check again after round trip
                state.pending_tuple = tuple(response['tuple'])

    def _notify_peers_for_out(self, out_tuple: tuple):
        """
        After a local OUT, scan the registry for remote threads
        with matching patterns and send each one a NOTIFY.
            RD: include the tuple in the NOTIFY
            IN: send only our address so the peer can QUERY to remove it.
        """
        with self.registry_lock:
            matching = [
                r for r in self.registry
                if self.tuple_space.match(out_tuple, r['pattern'])
            ]

        for reg in matching:
            print(f"  [NET] NOTIFY -> {reg['ip']}:{reg['port']} tid={reg['tid']} op={reg['op']} tuple={out_tuple}")
            msg = {
                'type':    'NOTIFY',
                'op':      reg['op'],
                'tid':     reg['tid'],
                'pattern': reg['pattern'],
            }
            if reg['op'] == 'RD':
                msg['tuple'] = list(out_tuple)
            else:
                msg['from_ip']   = self.my_ip
                msg['from_port'] = self.tcp_port

            self._send_tcp(reg['ip'], reg['port'], msg, read_response=False)

    def _send_cancel_to_peers(self, pattern: list, tid: str):
        """Tell all peers to remove our registration"""
        with self.peers_lock:
            peers = list(self.peers)
        for ip, port in peers:
            self._send_tcp(ip, port, {
                'type':    'CANCEL',
                'ip':      self.my_ip,
                'port':    self.tcp_port,
                'pattern': pattern,
                'tid':     tid,
            }, read_response=False)

    def _broadcast_goodbye(self):
        """Notify peers that this runtime is leaving."""
        with self.peers_lock:
            peers = list(self.peers)
        for ip, port in peers:
            self._send_tcp(ip, port, {
                'type': 'GOODBYE',
                'ip':   self.my_ip,
                'port': self.tcp_port,
            }, read_response=False)

    # -- Load balancing -- 
    def _get_peer_counts(self, only_running: bool = False) -> dict:
        """{(ip, port): {'count', 'all_blocked', 'has_threads'}} for all reachable peers."""
        with self.peers_lock:
            peers = list(self.peers)
        results = {}
        lock = threading.Lock()

        def ask(ip, port):
            resp = self._send_tcp(ip, port, {'type': 'COUNT', 'only_running': only_running})
            if resp and 'count' in resp:
                with lock:
                    results[(ip, port)] = resp

        ts = [threading.Thread(target=ask, args=p, daemon=True) for p in peers]
        for t in ts: t.start()
        for t in ts: t.join()
        return results
    
    def _migrate_thread(self, tid: str, ip: str, port: int) -> bool:
        """Attempt to migrate tid to ip:port. Returns True on success."""
        state = self.threads.get(tid)
        if state is None:
            return False

        del self.threads[tid]
        remaining = (state.sleep_until - time.time()) if state.sleep_until else None

        payload = {
            'type':            'MIGRATE',
            'tid':             tid,
            'name':            state.name,
            'code':            [[i[0], i[1]] for i in state.code],
            'labels':          state.labels,
            'variables':       state.variables,
            'pc':              state.pc,
            'sleep_remaining': max(0, remaining) if remaining else None,
            'block_op':        state.block_op,
            'block_pattern':   state.block_pattern,
            'pending_tuple':   list(state.pending_tuple) if state.pending_tuple else None,
            'from_ip':         self.my_ip,
            'from_port':       self.tcp_port,
        }

        response = self._send_tcp(ip, port, payload)
        if response and response.get('ok'):
            print(f"[Thread {tid}] Migrated to {ip}:{port}")
            if state.registered:
                threading.Thread(target=self._send_cancel_to_peers,
                    args=(state.block_pattern, tid), daemon=True).start()
            return True
        else:
            self.threads[tid] = state   # put it back
            print(f"[Thread {tid}] Migration to {ip}:{port} failed")
            return False
        
    def _check_deadlock(self, peer_counts: dict):
        active = [s for s in self.threads.values() if not s.finished]
        local_all_blocked = bool(active) and not any(s for s in active if s.block_op is None)
        total_has_threads = bool(active) or any(r.get('has_threads') for r in peer_counts.values())

        in_deadlock = (
            total_has_threads
            and local_all_blocked
            and all(not r.get('has_threads') or r.get('all_blocked') for r in peer_counts.values())
        )

        if in_deadlock and not self._deadlock_warned:
            self._deadlock_warned = True
            print("[DEADLOCK] Potential deadlock. All threads across all machines are blocked.")
        elif not in_deadlock and self._deadlock_warned:
            self._deadlock_warned = False
            print("[DEADLOCK] Potential deadlock resolved.")

    def _load_balance(self, only_running: bool = False):
        peer_counts = self._get_peer_counts(only_running)
        if not peer_counts:
            return

        if only_running:
            local_count = sum(1 for s in self.threads.values()
                              if not s.finished
                              and s.sleep_until is None
                              and s.block_op is None)
        else:
            local_count = sum(1 for s in self.threads.values() if not s.finished)

        total = local_count + sum(r['count'] for r in peer_counts.values())
        machines = len(peer_counts) + 1

        if LB_THRESHOLD_MODE == LB_CEIL:
            threshold = math.ceil(total / machines)
        else:
            threshold = math.floor(total / machines) + 1

        surplus = local_count - threshold
        if surplus <= 0:
            self._check_deadlock(peer_counts)
            return

        print(f"    [LB] my={local_count} avg={total/machines:.1f} threshold={threshold} -> migrating {surplus}")

        def migration_priority(tid):
            s = self.threads[tid]
            if s.sleep_until is not None: return 0 # Sleeping -> high priority
            if s.block_op is not None:    return 1 # Blocked on RD/IN -> medium priority
            return 2                               # Running -> low priority

        candidates = sorted(
            [tid for tid, s in self.threads.items() if not s.finished], key=migration_priority
        )

        counts = {(ip, port): r['count'] for (ip, port), r in peer_counts.items()}
        for tid in candidates:
            if surplus <= 0:
                break
            (ip, port) = min(counts, key=lambda k: counts[k])
            if self._migrate_thread(tid, ip, port):
                surplus -= 1
                counts[(ip, port)] += 1

        self._check_deadlock(peer_counts)

    # —- Command handling —-
    def input_loop(self):
        while self.running:
            try:
                cmd = input(">> ")
                self.cmd_queue.put(cmd)
            except (EOFError, KeyboardInterrupt):
                self.cmd_queue.put("exit")
                break

    def handle_command(self, cmd_str: str):
        parts = cmd_str.strip().split()
        if not parts:
            return
        cmd = parts[0].lower()
        if   cmd == "run":      self.cmd_run(parts)
        elif cmd == "list":     self.cmd_list()
        elif cmd == "kill":     self.cmd_kill(parts)
        elif cmd == "peers":    self.cmd_peers()
        elif cmd == "migrate":  self.cmd_migrate(parts)
        elif cmd == "shutdown": self.cmd_shutdown()
        elif cmd == "exit":     self.cmd_exit()
        else: print(f"Unknown command: '{cmd}'")

    def cmd_run(self, parts: list[str]):
        if len(parts) < 2:
            print("Usage: run <progname> [args...]")
            return
        try:
            state = SimpleScriptState.from_file(parts[1], parts[2:])
        except (RuntimeError, FileNotFoundError) as e:
            print(f"Error loading '{parts[1]}': {e}")
            return
        tid = f"{self.my_ip}:{self.tcp_port}-{self.next_tid}"
        self.next_tid += 1
        self.threads[tid] = state
        print(f"[Thread {tid}] Started '{parts[1]}'")

    def cmd_list(self):
        if not self.threads:
            print("No active threads")
            return
        for tid, state in self.threads.items():
            print(f"  Thread {tid}: {state.name}")

    def cmd_kill(self, parts: list[str]):
        if len(parts) < 2:
            print("Usage: kill <thread_id>")
            return
        tid = parts[1]
        if tid not in self.threads:
            print(f"Thread {tid} not found")
            return
        del self.threads[tid]
        print(f"[Thread {tid}] Killed")

    def cmd_peers(self):
        with self.peers_lock:
            if not self.peers:
                print("No peers discovered yet")
                return
            for ip, port in self.peers:
                print(f"  {ip}:{port}")

    def cmd_migrate(self, parts: list[str]):
        if(len(parts) < 4):
            print("Usage: migrate <tid> <ip> <port>")                             
            return
        tid = parts[1] 
        ip = parts[2] 
        try:                                                  
            port = int(parts[3])
        except ValueError:
            print(f"Invalid port: '{parts[3]}'")                                  
            return
        if tid not in self.threads:
            print(f"Thread {tid} not found")
            return
        self._migrate_thread(tid, ip, port)

    def cmd_shutdown(self):
        with self.peers_lock:
            peers = list(self.peers)

        if not peers and self.threads:
            print("No peers available. Shutdown cancel\n")
            return

        peer_counts = self._get_peer_counts()
        counts = {p: peer_counts[p]['count'] if p in peer_counts else 0 for p in peers}

        for tid in list(self.threads.keys()):
            (ip, port) = min(counts, key=lambda k: counts[k])
            if self._migrate_thread(tid, ip, port):
                counts[(ip, port)] += 1

        for i, tup in enumerate(self.tuple_space.ret_tuples()):
            ip, port = peers[i % len(peers)]
            self._send_tcp(ip, port, {'type': 'REMOTE_OUT', 'tuple': list(tup)}, read_response=False)

        self._broadcast_goodbye()
        self.running = False
        print("Shutdown complete")

    def cmd_exit(self):
        self._broadcast_goodbye()
        self.running = False
        print("Shutting down...")
    
    # —- Entry —-
    def start(self):
        print(f"SimpleScript Runtime — {self.my_ip}:{self.tcp_port}")
        print("Commands: run <file> [args], list, kill <tid>, peers, exit")
        print()
        threading.Thread(target=self.discovery_loop, daemon=True).start()
        self._send_multicast({'type': 'HELLO', 'port': self.tcp_port})
        threading.Thread(target=self._tcp_server,    daemon=True).start()
        threading.Thread(target=self.scheduler_loop, daemon=True).start()
        threading.Thread(target=self.load_balancer_loop, daemon=True).start()
        self.input_loop()

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
    Runtime(port).start()