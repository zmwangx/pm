#!/usr/bin/env python3

import argparse
import atexit
import http.server
import json
import logging
import os
import shutil
import signal
import socket
import socketserver
import threading
import webbrowser

try:
    import setproctitle
    setproctitle.setproctitle('pmserver')
except Exception:
    pass

# Set up logger
logging.basicConfig(
    format='[%(asctime)s] %(message)s',
    datefmt='%d/%b/%Y %H:%M:%S', # Match the logging format of http.server.HTTPServer
    level=logging.INFO,
)
logger = logging.getLogger()

# Events and the controlling lock
should_wakeup = threading.Event()
should_update = threading.Event()
server_shutting_down = threading.Event()
events_lock = threading.RLock()

def snatch_free_port():
    s = socket.socket()
    atexit.register(s.close)
    s.bind(("", 0))
    _, port = s.getsockname()
    return port

def extract_manpage_content(path):
    try:
        with open(path) as f:
            recording = False
            content = ''
            for line in f:
                if recording:
                    content += line
                if line == '<pre id="manpage">\n':
                    recording = True
                if line == '</pre>\n':
                    break
        return content
    except OSError:
        return ''

class PMHTTPRequestHandler(http.server.BaseHTTPRequestHandler):

    file_path = None

    def __write(self, msg):
        self.wfile.write(msg.encode('utf-8'))
        self.wfile.flush()

    def do_GET(self):
        if self.path == '/':
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            try:
                with open(self.file_path, 'rb') as f:
                    fs = os.fstat(f.fileno())
                    self.send_header("Content-Length", str(fs.st_size))
                    self.send_header("Last-Modified", self.date_time_string(fs.st_mtime))
                    self.end_headers()
                    shutil.copyfileobj(f, self.wfile)
            except OSError:
                self.send_header("Content-Length", "0")
                self.end_headers()
        elif self.path == '/events':
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            while True:
                should_wakeup.wait()
                events_lock.acquire()
                if should_update.is_set():
                    updated_content = {
                        'content': extract_manpage_content(self.file_path)
                    }
                    self.__write('event: update\n')
                    self.__write('data: ' + json.dumps(updated_content) + '\n\n')
                    should_update.clear()
                if server_shutting_down.is_set():
                    self.__write('event: bye\n')
                    self.__write('data: {}\n\n')
                    self.close_connection = True
                    break
                should_wakeup.clear()
                events_lock.release()
        else:
            self.send_error(404)

class PMHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):

    def __init__(self, server_address, file_path):
        socketserver.ThreadingMixIn.__init__(self)
        PMHTTPRequestHandler.file_path = file_path
        http.server.HTTPServer.__init__(self, server_address, PMHTTPRequestHandler)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('file', help='the file to be served and kept updated')
    args = parser.parse_args()

    port = snatch_free_port()
    httpd = PMHTTPServer(('localhost', port), args.file)
    url = 'http://localhost:%d' % port

    def shutdown_server(signum, stackframe):
        logger.info('Shutting down HTTP server...')
        events_lock.acquire()
        server_shutting_down.set()
        should_wakeup.set()
        events_lock.release()
        httpd.shutdown()

    signal.signal(signal.SIGINT, shutdown_server)
    signal.signal(signal.SIGTERM, shutdown_server)

    def update_content(signum, stackframe):
        logger.info('Updating content...')
        events_lock.acquire()
        should_update.set()
        should_wakeup.set()
        events_lock.release()

    signal.signal(signal.SIGTSTP, update_content)

    logger.info('HTTP server listening on %s', url)
    httpd_thread = threading.Thread(target=httpd.serve_forever)
    httpd_thread.start()

    try:
        webbrowser.open(url)
    except webbrowser.Error:
        logger.error('Error: Failed to open %s in your browser.', url)
        os.kill(os.getpid(), signal.SIGTERM)

if __name__ == '__main__':
    main()
