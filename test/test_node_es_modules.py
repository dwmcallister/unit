import pytest

from distutils.version import LooseVersion
from unit.applications.lang.node import TestApplicationNode
from unit.applications.websockets import TestApplicationWebsocket


class TestNodeESModules(TestApplicationNode):
    prerequisites = {
        'modules': {
            'node': lambda v:  LooseVersion(v) >= LooseVersion("14.16.0")
        }
    }

    es_modules = True
    ws = TestApplicationWebsocket()

    def assert_basic_application(self):
        resp = self.get()
        assert resp['headers']['Content-Type'] == 'text/plain', 'basic header'
        assert resp['body'] == 'Hello World\n', 'basic body'

    def test_node_es_modules_require_shim_http(self):
        self.load('require_shim/es_modules_http', name="app.mjs")

        self.assert_basic_application()

    def test_node_es_modules_require_shim_http_indirect(self):
        self.load('require_shim/es_modules_http_indirect', name="app.js")

        self.assert_basic_application()

    def test_node_es_modules_require_shim_websockets(self):
        self.load('require_shim/es_modules_websocket', name="app.mjs")

        message = 'blah'

        _, sock, _ = self.ws.upgrade()

        self.ws.frame_write(sock, self.ws.OP_TEXT, message)
        frame = self.ws.frame_read(sock)

        assert message == frame['data'].decode('utf-8'), 'mirror'

        self.ws.frame_write(sock, self.ws.OP_TEXT, message)
        frame = self.ws.frame_read(sock)

        assert message == frame['data'].decode('utf-8'), 'mirror 2'

        sock.close()