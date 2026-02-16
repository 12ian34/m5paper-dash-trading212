"""Threaded HTTP file server for M5Paper dashboard.

python3 -m http.server is single-threaded and hangs if a client connects
but doesn't send a complete request (e.g. TCP probe, slow ESP32 boot).
This threaded version handles each connection in its own thread.
"""
from http.server import SimpleHTTPRequestHandler
from socketserver import ThreadingTCPServer
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

ThreadingTCPServer.allow_reuse_address = True
with ThreadingTCPServer(("0.0.0.0", PORT), SimpleHTTPRequestHandler) as httpd:
    print(f"Serving on port {PORT} (threaded)")
    httpd.serve_forever()
