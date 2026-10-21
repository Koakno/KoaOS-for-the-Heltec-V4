#!/usr/bin/env python3
"""KoaOS repository server.

Usage:  python repo_server.py [folder] [port]
Serves every file in the folder (default ./repo) on port 8000 (default),
and builds /index.txt automatically so `install -l` works on the V4.
On the V4:  install -s <this-computer-ip>:8000   then   install hello
"""
import os, socket, sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

folder = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "repo")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
os.makedirs(folder, exist_ok=True)


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=folder, **k)

    def do_GET(self):
        if self.path == "/index.txt":
            # packages (name.esp) are listed by name; "install name" fetches name.esp first, then a plain script
            names = sorted({
                f[:-4] if f.endswith(".esp") else f
                for f in os.listdir(folder)
                if os.path.isfile(os.path.join(folder, f)) and not f.startswith(".") and f != "index.txt"
            })
            body = "".join(f"{n}\n" for n in names).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            super().do_GET()


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


print(f"Serving {folder}")
print(f"On the V4 run:  install -s {lan_ip()}:{port}")
ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()
