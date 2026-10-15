"""Serve the production Web assets and proxy cmain to the UART test server."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse
import mimetypes
import signal
import socket
import subprocess
import tempfile
import threading


ROOT = Path(__file__).resolve().parents[2]
WWW = (ROOT / 'www').resolve()


class Gateway(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, _format, *_args):
        pass

    def _reply(self, status, content_type, body):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        request_path = unquote(urlparse(self.path).path)
        if request_path == '/favicon.ico':
            self._reply(204, 'image/x-icon', b'')
            return
        request_path = '/index.html' if request_path == '/' else request_path
        target = (WWW / request_path.lstrip('/')).resolve()
        if WWW not in target.parents or not target.is_file():
            self._reply(404, 'text/plain; charset=utf-8', b'not found')
            return
        content_type = mimetypes.guess_type(target.name)[0] or 'application/octet-stream'
        if content_type.startswith('text/') or content_type in ('application/javascript', 'application/json'):
            content_type += '; charset=utf-8'
        self._reply(200, content_type, target.read_bytes())

    def do_POST(self):
        if urlparse(self.path).path != '/cgi-bin/cmain':
            self._reply(404, 'text/plain; charset=utf-8', b'not found')
            return
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length)
        with socket.create_connection(('127.0.0.1', 29608), timeout=4) as backend:
            backend.sendall(body)
            backend.shutdown(socket.SHUT_WR)
            chunks = []
            while chunk := backend.recv(65536):
                chunks.append(chunk)
        self._reply(200, 'application/json; charset=utf-8', b''.join(chunks))


def main():
    with tempfile.TemporaryDirectory(prefix='linkg-uart-browser-') as temporary:
        process = subprocess.Popen(
            [str(ROOT / 'build/uart-test/uart_test_server'), str(Path(temporary) / 'user.json')],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            if process.stdout.readline().strip() != 'READY':
                raise RuntimeError('UART test server did not become ready')
            server = ThreadingHTTPServer(('127.0.0.1', 18080), Gateway)
            signal.signal(signal.SIGTERM, lambda *_args: threading.Thread(target=server.shutdown, daemon=True).start())
            print('READY http://127.0.0.1:18080', flush=True)
            server.serve_forever()
        finally:
            if process.poll() is None:
                process.stdin.write('quit\n')
                process.stdin.flush()
                process.wait(timeout=10)


if __name__ == '__main__':
    main()
