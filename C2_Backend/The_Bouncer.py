import socket
import threading
import os
import sys
import colorama
from colorama import Fore, Style

colorama.init()

def forward_traffic(source, destination):
    try:
        while True:
            data = source.recv(8192)
            if not data: break
            destination.sendall(data)
    except: pass
    finally:
        try: source.close()
        except: pass
        try: destination.close()
        except: pass

def handle_connection(client_socket, flask_port, shell_port):
    try:
        # Peek at the data to decide where to route
        first_bytes = client_socket.recv(4, socket.MSG_PEEK)
        
        # If it looks like HTTP, send to Flash. Otherwise, send to our Raw Shell.
        if first_bytes.startswith(b'POST') or first_bytes.startswith(b'GET ') or first_bytes.startswith(b'PUT ') or first_bytes.startswith(b'HEAD'):
            target_port = flask_port
        else:
            target_port = shell_port
            
        local_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        local_server.connect(('127.0.0.1', target_port))
        
        # Cross-wire the connections
        threading.Thread(target=forward_traffic, args=(client_socket, local_server), daemon=True).start()
        threading.Thread(target=forward_traffic, args=(local_server, client_socket), daemon=True).start()
    except Exception:
        try: client_socket.close()
        except: pass

def main():
    if len(sys.argv) < 4:
        print("[!] Usage: python The_Bouncer.py <PUBLIC_PORT> <FLASK_PORT> <SHELL_PORT>")
        return
        
    router_port = int(sys.argv[1])
    flask_port = int(sys.argv[2])
    shell_port = int(sys.argv[3])

    os.system(f"title [SHATTERED MIRROR] TRAFFIC BOUNCER - PORT {router_port}")
    print(f"{Fore.RED}=====================================================================")
    print(f"  SHATTERED MIRROR TRAFFIC BOUNCER: PORT {router_port}")
    print(f"  Routing: HTTP -> {flask_port} | RAW -> {shell_port}")
    print(f"====================================================================={Style.RESET_ALL}\n")

    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', router_port))
    server.listen(100)

    while True:
        client, addr = server.accept()
        threading.Thread(target=handle_connection, args=(client, flask_port, shell_port), daemon=True).start()

if __name__ == "__main__":
    main()
