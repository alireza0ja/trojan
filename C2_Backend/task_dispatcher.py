import sqlite3
import argparse

def add_command(target_id, command):
    # In a real app, connect to the shared DB used by the listener
    print(f"[+] Tasking {target_id} with command: {command}")
    print(f"[*] Command added to queue. Will be picked up on next beacon.")
    
    # Mocking DB insert
    # db = sqlite3.connect('c2_state.db')
    # cursor = db.cursor()
    # cursor.execute("INSERT INTO commands (target_id, command) VALUES (?, ?)", (target_id, command))
    # db.commit()
    # db.close()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Shattered Mirror Task Dispatcher")
    parser.add_argument('--target', required=True, help="HWID or Target ID of the implant")
    parser.add_argument('--cmd', required=True, help="Command to queue (e.g. KEYLOG_FLUSH, EXFIL_SYSINFO)")
    
    args = parser.parse_args()
    
    print("=== Shattered Mirror Dispatcher ===")
    add_command(args.target, args.cmd)
