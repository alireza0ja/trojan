"""
SHATTERED MIRROR — Traffic Bouncer (Multiplexer)
ULTRA DEBUG VERSION – Logs every step to file.
"""

import socket
import threading
import os
import sys
import time
import traceback

try:
    import colorama
    from colorama import Fore, Style
    colorama.init()
except ImportError:
    class _Dummy:
        RED = GREEN = CYAN = YELLOW = MAGENTA = WHITE = ""
        RESET_ALL = BRIGHT = ""
    Fore = Style = _Dummy()

script_dir = os.path.dirname(os.path.abspath(__file__))
log_dir = os.path.join(script_dir, "..", "log")
if not os.path.exists(log_dir):
    os.makedirs(log_dir)
LOG_FILE = os.path.join(log_dir, "bouncer_debug.log")

def log(tag, msg, color=Fore.WHITE):
    ts = time.strftime("%H:%M:%S")
    line = f"{color}[{ts}] [{tag:^6}] {msg}{Style.RESET_ALL}"
    print(line, flush=True)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(f"[{ts}] [{tag:^6}] {msg}\n")
        f.flush()

def forward_traffic(source, dest, label=""):
    bytes_total = 0
    log("FWD", f"{label} starting relay", Fore.CYAN)
    try:
        while True:
            data = source.recv(8192)
            if not data:
                log("FWD", f"{label} EOF after {bytes_total} bytes", Fore.YELLOW)
                break
            dest.sendall(data)
            bytes_total += len(data)
            log("FWD", f"{label} relayed {len(data)} bytes (total {bytes_total})", Fore.CYAN)
    except ConnectionResetError:
        log("CLOSE", f"{label} reset after {bytes_total} bytes", Fore.YELLOW)
    except ConnectionAbortedError:
        log("CLOSE", f"{label} aborted after {bytes_total} bytes", Fore.YELLOW)
    except OSError as e:
        if e.errno in (10053, 10054, 10038):
            log("CLOSE", f"{label} closed ({bytes_total} bytes)", Fore.YELLOW)
        else:
            log("ERROR", f"{label} OS error: {e}", Fore.RED)
    except Exception as e:
        log("ERROR", f"{label} error: {e}", Fore.RED)
    finally:
        try: source.close()
        except: pass
        try: dest.close()
        except: pass

def handle_connection(client_socket, flask_port, shell_port, turbo_port, client_addr):
    client_ip = client_addr[0]
    client_port = client_addr[1]
    target_port = None

    log("ENTER", f"Thread started for {client_ip}:{client_port}", Fore.GREEN)

    try:
        client_socket.settimeout(10)
        log("PEEK", f"Peeking first 4 bytes from {client_ip}:{client_port}", Fore.CYAN)
        first_bytes = client_socket.recv(4, socket.MSG_PEEK)
        if not first_bytes:
            log("DROP", f"{client_ip}:{client_port} sent empty data", Fore.YELLOW)
            client_socket.close()
            return
        log("PEEK", f"Peeked: {first_bytes[:4].hex()}", Fore.CYAN)

        is_http = first_bytes.startswith((b'POST', b'GET ', b'PUT ', b'HEAD',
                                          b'DELE', b'PATC', b'OPTI'))
        is_turbo = first_bytes.startswith(b'[')
        
        log("DETECT", f"is_http={is_http}, is_turbo={is_turbo}", Fore.CYAN)

        if is_http:
            target_port = flask_port
            log(" HTTP", f"{client_ip}:{client_port} -> Flask:{target_port}", Fore.CYAN)

            first_data = client_socket.recv(65536)
            if not first_data:
                client_socket.close()
                return

            xff_header = f"X-Forwarded-For: {client_ip}\r\n".encode()
            first_line_end = first_data.find(b'\r\n')
            if first_line_end != -1:
                modified_data = (first_data[:first_line_end + 2]
                                 + xff_header
                                 + first_data[first_line_end + 2:])
            else:
                modified_data = first_data

            local_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            local_srv.settimeout(5)
            local_srv.connect(('127.0.0.1', target_port))
            local_srv.sendall(modified_data)

        elif is_turbo:
            target_port = turbo_port
            log("TURBO", f"{client_ip}:{client_port} -> Turbo TCP:{target_port}", Fore.GREEN)
            
            first_data = client_socket.recv(65536)
            if not first_data:
                client_socket.close()
                return
                
            local_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            local_srv.settimeout(5)
            local_srv.connect(('127.0.0.1', target_port))
            local_srv.sendall(first_data)
            
        else:
            # Multi-Shell Multiplexing: Dynamically assign a port and launch Shell_Session
            temp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            temp_sock.bind(('127.0.0.1', 0))
            target_port = temp_sock.getsockname()[1]
            temp_sock.close()
            
            log("  RAW", f"{client_ip}:{client_port} -> Dynamic Shell:{target_port}", Fore.MAGENTA)

            # Spawn the shell session on the dynamic port
            import subprocess
            shell_script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Shell_Session.py")
            cmd_str = f'start cmd /k python "{shell_script}" {target_port} 120'
            subprocess.Popen(cmd_str, shell=True)
            log("  RAW", f"Spawned Shell_Session on port {target_port}", Fore.MAGENTA)
            
            # FAST POLLING: Check for port readiness every 50ms instead of 1.5s fixed sleep
            local_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            local_srv.settimeout(2)
            connected = False
            for i in range(20): # Up to 1 second total
                try:
                    local_srv.connect(('127.0.0.1', target_port))
                    connected = True
                    log("  RAW", f"Connected to shell port {target_port} in {i*50}ms", Fore.GREEN)
                    break
                except:
                    time.sleep(0.05)
            
            if not connected:
                log(" ERROR", f"Failed to connect to dynamic shell port {target_port}", Fore.RED)
                client_socket.close()
                return

            # Read the full first chunk (including the peeked bytes) and forward to shell
            first_data = client_socket.recv(65536)
            if not first_data:
                client_socket.close()
                local_srv.close()
                return
            log("  RAW", f"First chunk: {len(first_data)} bytes", Fore.MAGENTA)

            local_srv.sendall(first_data)  # Forward the first chunk
            log("  RAW", f"Relayed first chunk to shell session", Fore.MAGENTA)

        client_socket.settimeout(None)
        local_srv.settimeout(None)

        if is_turbo:
            # Turbo streams are ONE-WAY (implant -> server).
            # Only relay implant->turbo, then gracefully shutdown so the
            # turbo listener gets a clean FIN (not RST from a dangling t2).
            label_out = f"{client_ip}->SRV"
            def turbo_relay(src, dst, label):
                bytes_total = 0
                log("FWD", f"{label} starting relay", Fore.CYAN)
                try:
                    while True:
                        data = src.recv(8192)
                        if not data:
                            log("FWD", f"{label} EOF after {bytes_total} bytes", Fore.YELLOW)
                            break
                        dst.sendall(data)
                        bytes_total += len(data)
                        log("FWD", f"{label} relayed {len(data)} bytes (total {bytes_total})", Fore.CYAN)
                except Exception as e:
                    log("FWD", f"{label} error after {bytes_total} bytes: {e}", Fore.YELLOW)
                finally:
                    try: dst.shutdown(socket.SHUT_WR)  # Clean FIN to turbo listener
                    except: pass
                    try: src.close()
                    except: pass
                    try: dst.close()
                    except: pass

            t1 = threading.Thread(target=turbo_relay,
                                  args=(client_socket, local_srv, label_out), daemon=True)
            t1.start()
            log("FWD", f"Turbo relay started (unidirectional)", Fore.GREEN)
        else:
            label_out = f"{client_ip}->SRV"
            label_in  = f"SRV->{client_ip}"
            t1 = threading.Thread(target=forward_traffic,
                                  args=(client_socket, local_srv, label_out), daemon=True)
            t2 = threading.Thread(target=forward_traffic,
                                  args=(local_srv, client_socket, label_in), daemon=True)
            t1.start()
            t2.start()
            log("FWD", f"Relay threads started", Fore.GREEN)

    except socket.timeout:
        log("TMOUT", f"{client_ip}:{client_port} timed out", Fore.YELLOW)
        try: client_socket.close()
        except: pass
    except Exception as e:
        log("ERROR", f"Handler crash: {type(e).__name__}: {e}", Fore.RED)
        log("TRACE", traceback.format_exc(), Fore.RED)
        try: client_socket.close()
        except: pass

def main():
    if len(sys.argv) < 5:
        print(f"{Fore.RED}[!] Usage: python The_Bouncer.py <PUBLIC_PORT> <FLASK_PORT> <SHELL_PORT> <TURBO_PORT>{Style.RESET_ALL}")
        return

    router_port = int(sys.argv[1])
    flask_port  = int(sys.argv[2])
    shell_port  = int(sys.argv[3])
    turbo_port  = int(sys.argv[4])

    if os.name == 'nt':
        os.system(f"title [SHATTERED MIRROR] TRAFFIC BOUNCER - PORT {router_port}")

    print(f"{Fore.RED}")
    print("=" * 65)
    print("  SHATTERED MIRROR — TRAFFIC BOUNCER")
    print(f"  PUBLIC : 0.0.0.0:{router_port}")
    print(f"  HTTP   -> 127.0.0.1:{flask_port}   (Flask telemetry)")
    print(f"  RAW    -> Dynamic        (Shell sessions)")
    print(f"  TURBO  -> 127.0.0.1:{turbo_port}   (Live Streams)")
    print("=" * 65)
    print(f"{Style.RESET_ALL}")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    server.bind(('0.0.0.0', router_port))
    server.listen(100)
    log("READY", f"Listening on 0.0.0.0:{router_port} — awaiting connections", Fore.GREEN)

    while True:
        try:
            client, addr = server.accept()
            log(" CONN", f"Accepted: {addr[0]}:{addr[1]}", Fore.GREEN)
            threading.Thread(target=handle_connection,
                             args=(client, flask_port, shell_port, turbo_port, addr),
                             daemon=True).start()
            log("THREAD", f"Handler thread started for {addr[0]}:{addr[1]}", Fore.GREEN)
        except KeyboardInterrupt:
            log(" STOP", "Bouncer shutting down.", Fore.YELLOW)
            break
        except Exception as e:
            log("ERROR", f"Accept error: {e}", Fore.RED)

if __name__ == "__main__":
    main()