import hmac
import hashlib
import time

class PSKRotator:
    """
    Implements Time-Based Pre-Shared Key (PSK) generation.
    Both the Implant and C2 use a base seed (e.g., 32 bytes).
    The HMAC key is derived from the current time window (floor(time/60)).
    """

    def __init__(self, base_seed: bytes, window_size: int = 60):
        self.base_seed = base_seed
        self.window_size = window_size

    def _generate_hmac(self, timestamp_window: int) -> str:
        msg = str(timestamp_window).encode('utf-8')
        digest = hmac.new(self.base_seed, msg, hashlib.sha256).hexdigest()
        return digest

    def get_current_psk(self) -> str:
        current_window = int(time.time() // self.window_size)
        return self._generate_hmac(current_window)

    def validate_psk(self, client_psk: str) -> bool:
        """
        Validates against current, previous, and next window to account for 
        slight clock skew between the C2 and the implanted machine.
        """
        current_window = int(time.time() // self.window_size)
        
        valid_psks = [
            self._generate_hmac(current_window - 1),
            self._generate_hmac(current_window),
            self._generate_hmac(current_window + 1)
        ]
        
        return client_psk in valid_psks

# Example Usage
if __name__ == "__main__":
    seed = b"SuperSecretSeedForClient001"
    rotator = PSKRotator(seed)
    current = rotator.get_current_psk()
    print(f"Current PSK: {current}")
    print(f"Is valid? {rotator.validate_psk(current)}")
