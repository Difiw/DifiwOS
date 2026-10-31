#!/usr/bin/env python3
"""Local test stand: static files on :PORT and a minimal WebSocket echo ('hello') on :PORT+1."""
import base64, hashlib, http.server, socketserver, sys, threading, socket, os
port = int(sys.argv[1]); os.chdir(os.path.dirname(os.path.abspath(__file__)))
def ws():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', port + 1)); s.listen(5)
    while True:
        c, _ = s.accept(); d = c.recv(4096).decode(errors='ignore')
        k = [l.split(': ')[1].strip() for l in d.split('\r\n') if l.lower().startswith('sec-websocket-key')]
        if not k: c.close(); continue
        a = base64.b64encode(hashlib.sha1((k[0] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        c.send(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + a + '\r\n\r\n').encode())
        c.send(b'\x81\x05hello')
threading.Thread(target=ws, daemon=True).start()
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
socketserver.TCPServer.allow_reuse_address = True
socketserver.TCPServer(('127.0.0.1', port), H).serve_forever()
