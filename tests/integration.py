"""Black-box integration tests; only Python's standard library is required."""
import concurrent.futures
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

TOKEN = 'integration-test-secret-token'

class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            cls.port = sock.getsockname()[1]
        cls.log = tempfile.TemporaryFile()
        cls.server = subprocess.Popen([sys.argv[1]], env={**os.environ, 'API_TOKEN': TOKEN, 'PORT': str(cls.port)}, stdout=cls.log, stderr=cls.log)
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=.1):
                    return
            except OSError:
                if cls.server.poll() is not None:
                    break
                time.sleep(.02)
        raise RuntimeError('Server failed to start')

    @classmethod
    def tearDownClass(cls):
        cls.server.send_signal(signal.SIGTERM)
        cls.server.wait(timeout=5)
        cls.log.close()
        assert cls.server.returncode == 0

    def request(self, method, path, body=None, auth=True, headers=None):
        h = {'Authorization': 'Bearer ' + TOKEN} if auth else {}
        if body is not None:
            h['Content-Type'] = 'application/json'
            body = json.dumps(body)
        h.update(headers or {})
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=4)
        conn.request(method, path, body=body, headers=h)
        res = conn.getresponse()
        data = res.read()
        result = res.status, dict(res.getheaders()), json.loads(data) if data else None
        conn.close()
        return result

    def raw(self, data):
        with socket.create_connection(('127.0.0.1', self.port), timeout=4) as sock:
            sock.sendall(data)
            response = b''
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                response += chunk
            return int(response.split(b' ')[1])

    def test_01_auth_and_health(self):
        self.assertEqual(self.request('GET', '/health', auth=False)[0], 200)
        self.assertEqual(self.request('GET', '/api/v1/tasks', auth=False)[0], 401)
        self.assertEqual(self.request('GET', '/missing')[0], 404)
        self.assertEqual(self.request('PUT', '/api/v1/tasks')[0], 405)

    def test_02_crud_and_conflicts(self):
        status, headers, task = self.request('POST', '/api/v1/tasks', {'title': 'Build 🚀'})
        self.assertEqual(status, 201)
        path = headers['Location']
        self.assertEqual(self.request('GET', path)[2]['title'], 'Build 🚀')
        status, updated_headers, updated = self.request('PATCH', path, {'completed': True}, headers={'If-Match': headers['ETag']})
        self.assertEqual(status, 200)
        self.assertTrue(updated['completed'])
        self.assertEqual(updated['version'], 2)
        self.assertEqual(self.request('DELETE', path, headers={'If-Match': headers['ETag']})[0], 409)
        status, _, page = self.request('GET', '/api/v1/tasks?completed=true&limit=1&offset=0')
        self.assertEqual(status, 200)
        self.assertEqual(page['data'][0]['id'], task['id'])
        self.assertEqual(self.request('DELETE', path, headers={'If-Match': updated_headers['ETag']})[0], 204)
        self.assertEqual(self.request('GET', path)[0], 404)

    def test_03_validation(self):
        for body in ({}, {'title': ''}, {'title': 7}, {'title': 'x', 'extra': 1}, {'title': 'x', 'completed': 1}):
            self.assertEqual(self.request('POST', '/api/v1/tasks', body)[0], 422)
        self.assertEqual(self.request('POST', '/api/v1/tasks', [1])[0], 400)
        self.assertEqual(self.request('POST', '/api/v1/tasks')[0], 415)
        self.assertEqual(self.request('GET', '/api/v1/tasks?limit=0')[0], 400)
        self.assertEqual(self.request('GET', '/api/v1/tasks?limit=1&limit=2')[0], 400)
        self.assertEqual(self.request('GET', '/api/v1/tasks/nope')[0], 400)

    def test_04_http_defenses(self):
        for request in (
            b'GET /health HTTP/1.1\r\n\r\n',
            b'GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n',
            b'GET /health HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n',
            b'GET /health HTTP/1.1\r\nHost: x\r\nBad Header: x\r\n\r\n',
        ):
            self.assertEqual(self.raw(request), 400)
        self.assertEqual(self.raw(b'GET /health HTTP/1.1\r\nHost: x\r\nX: ' + b'a'*17000 + b'\r\n\r\n'), 431)
        malformed = b'{"title":"a","title":"b"}'
        req = b'POST /api/v1/tasks HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ' + TOKEN.encode() + b'\r\nContent-Type: application/json\r\nContent-Length: '+str(len(malformed)).encode()+b'\r\n\r\n'+malformed
        self.assertEqual(self.raw(req), 400)

    def test_05_concurrency(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: self.request('GET', '/health')[0], range(8)))
        self.assertEqual(results, [200]*8)

    def test_06_rate_limit(self):
        results = [self.request('GET', '/health')[0] for _ in range(65)]
        self.assertIn(429, results)

if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]], verbosity=2)
