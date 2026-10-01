import argparse
import struct
import client_api

def load_requests_as_binary(filename):
    raw_requests = []
    with open(filename, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or ':' not in line: continue
            try:
                svc_part, values_part = line.split(':', 1)
                svc_id = int(svc_part.strip())
                values = [int(v.strip()) for v in values_part.strip().split(',')]
                
                binary_payload = struct.pack(f"!{len(values)}I", *values)
                raw_requests.append((svc_id, binary_payload, len(values)))
            except ValueError:
                continue
    return raw_requests

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--config', '-c', type=str, default=None)
    parser.add_argument('--requests', '-r', type=str, required=True)
    parser.add_argument('--state', '-s', type=str, default='client_state.json', 
                        help='Persistent memory file for the client')
    
    args = parser.parse_args()
    workload = load_requests_as_binary(args.requests)
    
    try:
        print(f"[*] Initializing client using: {args.config if args.config else 'Discovery'}")
        client_api.init(config_file=args.config, state_file=args.state)

    except ValueError as e:
        print(f"[!] Init Failed: {e}")
        return

    print(f"[*] Starting workload using memory file: {args.state}")

    try:
        for i, (svc_id, binary_payload, count) in enumerate(workload):
            print(f"\n[Task {i+1}] Sending Service {svc_id} ({count} values)")
            
            result = client_api.doRequestReply(svc_id, binary_payload)
            if isinstance(result, str):
                print(f"[Task {i+1}] FAILED: {result}")
            else:
                primes = sum(1 for x in result if x)
                print(f"[Task {i+1}] SUCCESS: Found {primes} primes.")

    except KeyboardInterrupt:
        print("\n[*] Interrupted by user. Closing...")
    finally:
        print("[*] Client shutdown complete.")

if __name__ == "__main__":
    main()