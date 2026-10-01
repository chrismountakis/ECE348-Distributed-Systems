import argparse
import struct
import time
import threading
import random
import client_api

def load_requests_as_binary(filename):
    raw_requests = []
    try:
        with open(filename, 'r') as f:
            for line in f:
                line = line.strip()
                if not line or ':' not in line:
                    continue
                try:
                    svc_part, values_part = line.split(':', 1)
                    svc_id = int(svc_part.strip())
                    values = [int(v.strip()) for v in values_part.strip().split(',')]
                    binary_payload = struct.pack(f"!{len(values)}I", *values)
                    raw_requests.append((svc_id, binary_payload))
                except ValueError:
                    continue
    except FileNotFoundError:
        print(f"[!] Error: Workload file '{filename}' not found.")
        return []
    return raw_requests

def client_worker(client_id, config_file, workload, max_start_delay, barrier):
    state_file = f"client_{client_id}.json"
    
    if max_start_delay > 0:
        time.sleep(random.uniform(0, max_start_delay))
    
    try:
        client_api.init(config_file=config_file, state_file=state_file)
    except Exception as e:
        print(f"[Client {client_id}] Init FAILED: {e}")
        barrier.wait() 
        return
    
    barrier.wait() 
    
    for svc_id, binary_payload in workload:
        client_api.doRequestReply(svc_id, binary_payload)

def main():
    parser = argparse.ArgumentParser(description="Calculate pure server processing time")
    parser.add_argument('--config', '-c', type=str, default=None)
    parser.add_argument('--requests', '-r', type=str, required=True)
    parser.add_argument('--clients', '-n', type=int, default=3)
    parser.add_argument('--max-start-delay', '-d', type=float, default=0.0)
    args = parser.parse_args()
    
    workload = load_requests_as_binary(args.requests)
    if not workload: return

    sync_barrier = threading.Barrier(args.clients + 1)
    threads = []

    print(f"[*] Initializing {args.clients} clients...")
    for client_id in range(args.clients):
        t = threading.Thread(
            target=client_worker,
            args=(client_id, args.config, workload, args.max_start_delay, sync_barrier)
        )
        threads.append(t)
        t.start()

    sync_barrier.wait()
    
    start_time = time.perf_counter()
    
    for t in threads:
        t.join()
        
    total_time = time.perf_counter() - start_time

    total_reqs = args.clients * len(workload)
    print("\n" + "="*50)
    print(f"TOTAL SERVER PROCESSING TIME: {total_time:.4f} seconds")
    print(f"TOTAL REQUESTS HANDLED:      {total_reqs}")
    print(f"THROUGHPUT:                  {total_reqs / total_time:.2f} req/s")
    print("="*50)

if __name__ == "__main__":
    main()