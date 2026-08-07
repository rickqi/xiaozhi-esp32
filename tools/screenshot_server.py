#!/usr/bin/env python3
"""HTTP server that receives screenshots from xiaozhi MCP self.screen.snapshot
and saves them to the local output/ directory.

Usage:
    python tools/screenshot_server.py [port]

Default port: 8899

On the device, ask the AI (or use the web console) to call:
    self.screen.snapshot with url=http://<this-machine-ip>:8899

The JPEG will be saved to output/screenshot_YYYYMMDD_HHMMSS.jpg
"""

import os
import sys
import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler

OUTPUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "output")


class ScreenshotHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)

        jpeg_data = self._extract_jpeg(body)
        if jpeg_data is None:
            self.send_error(400, "No JPEG data found")
            return

        os.makedirs(OUTPUT_DIR, exist_ok=True)
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        filepath = os.path.join(OUTPUT_DIR, f"screenshot_{ts}.jpg")
        with open(filepath, "wb") as f:
            f.write(jpeg_data)

        print(f"[saved] {filepath} ({len(jpeg_data)} bytes)")
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"OK")

    @staticmethod
    def _extract_jpeg(body):
        start = body.find(b"\xff\xd8\xff")
        if start < 0:
            return None
        end = body.rfind(b"\xff\xd9")
        if end < 0:
            return None
        return body[start:end + 2]

    def log_message(self, fmt, *args):
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    print(f"Screenshot server: http://0.0.0.0:{port}")
    print(f"Output directory: {OUTPUT_DIR}")
    print("Waiting for screenshots...")
    HTTPServer(("0.0.0.0", port), ScreenshotHandler).serve_forever()


if __name__ == "__main__":
    main()
