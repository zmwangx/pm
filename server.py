#!/usr/bin/env python3

import argparse
import http.server
import json
import logging
import os
import shutil
import signal
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

def open_url(url):
    # Redirect stdout and stderr to /dev/null on the OS level so that
    # the browser process cannot write to the tty.
    stdoutfd = os.dup(1)
    stderrfd = os.dup(2)
    os.close(1)
    os.close(2)
    devnullfd = os.open(os.devnull, os.O_RDWR)
    os.dup2(devnullfd, 1)
    os.dup2(devnullfd, 2)
    try:
        webbrowser.open(url)
    except webbrowser.Error:
        logger.error('Error: Failed to open %s in your browser.', url)
        os.kill(os.getpid(), signal.SIGTERM)
    finally:
        os.close(devnullfd)
        os.dup2(stdoutfd, 1)
        os.dup2(stderrfd, 2)

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
            socket_closed_by_other_end = False
            while True:
                should_wakeup.wait()
                events_lock.acquire()
                if should_update.is_set():
                    updated_content = {
                        'content': extract_manpage_content(self.file_path)
                    }
                    try:
                        self.__write('event: update\n')
                        self.__write('data: ' + json.dumps(updated_content) + '\n\n')
                    except BrokenPipeError:
                        socket_closed_by_other_end = True
                    should_update.clear()
                if server_shutting_down.is_set():
                    try:
                        self.__write('event: bye\n')
                        self.__write('data: {}\n\n')
                    except BrokenPipeError:
                        socket_closed_by_other_end = True
                    self.close_connection = True
                    break
                should_wakeup.clear()
                events_lock.release()
                if socket_closed_by_other_end:
                    logger.warning('Warning: Socket closed by the other end.')
                    break
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

    httpd = PMHTTPServer(('localhost', 0), args.file)
    port = httpd.server_port
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

    signal.signal(signal.SIGUSR1, update_content)

    logger.info('HTTP server listening on %s', url)
    httpd_thread = threading.Thread(target=httpd.serve_forever)
    httpd_thread.start()

    open_url(url)

if __name__ == '__main__':
    main()
