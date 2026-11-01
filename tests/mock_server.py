#!/usr/bin/env python3
"""Mock OpenAI-compatible HTTP server used by the network unit tests.

Scenarios are selected by the first path segment, e.g. a request to
"/status500/v1/chat/completions" triggers the "status500" scenario:

  ok           - 200 with a valid OpenAI chat completion JSON (POST);
                 requires a valid Authorization header and a JSON body
                 containing "model" and "messages" (400 otherwise)
  status500    - 500 with a plain error body
  noauth       - 401 regardless of the Authorization header
  badjson      - 200 with a body that is not valid JSON
  emptychoices - 200 with a valid JSON containing an empty "choices" array
  nocontent    - 200 with choices[0].message lacking the "content" field
  noauthkey    - 200 with a valid chat completion JSON (POST) only when
                 NO Authorization header is sent; otherwise 401
  temp<value>  - 200 with a valid chat completion JSON (POST) only when
                 the JSON body carries "temperature" equal to <value>
                 (e.g. temp0.42); otherwise 400
  sse          - 200 with Content-Type text/event-stream and two
                 "data: " events (GET)
  ssecrlf      - 200 with Content-Type text/event-stream and two
                 "data: " events separated by CRLF (GET)
  ssespace     - 200 with Content-Type text/event-stream and two
                 "data:" events without a space after the colon (GET)

The server binds to an ephemeral port on 127.0.0.1 and prints the port
number on stdout (flushed immediately) so the test runner can export it.
"""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_KEY = "Bearer test-key-123"

VALID_RESPONSE = {
    "id": "chatcmpl-mock-1",
    "object": "chat.completion",
    "choices": [
        {
            "index": 0,
            "message": {"role": "assistant", "content": "Hello from mock!"},
            "finish_reason": "stop",
        }
    ],
}

SSE_BODY = b"data: first\n\ndata: second\n\n"
SSE_BODY_CRLF = b"data: first\r\n\r\ndata: second\r\n\r\n"
SSE_BODY_NOSPACE = b"data:first\ndata:second\n\n"


def scenario_of(path):
    """Return the scenario name: first segment of the request path."""
    return path.lstrip("/").split("/", 1)[0]


class MockHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _respond(self, code, body, content_type="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        scenario = scenario_of(self.path)
        if scenario == "sse":
            self._respond(200, SSE_BODY, "text/event-stream")
        elif scenario == "ssecrlf":
            self._respond(200, SSE_BODY_CRLF, "text/event-stream")
        elif scenario == "ssespace":
            self._respond(200, SSE_BODY_NOSPACE, "text/event-stream")
        else:
            self._respond(404, b"unknown scenario")

    def do_POST(self):
        scenario = scenario_of(self.path)
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length) if length > 0 else b""

        if scenario == "status500":
            self._respond(500, b"Internal Server Error", "text/plain")
            return

        if scenario == "noauthkey":
            # Succeeds only when NO Authorization header is sent.
            if self.headers.get("Authorization") is not None:
                self._respond(401, b'{"error": "unexpected authorization header"}')
            else:
                self._respond(200, json.dumps(VALID_RESPONSE).encode())
            return

        if scenario == "noauth" or \
                self.headers.get("Authorization") != API_KEY:
            self._respond(401, b'{"error": "unauthorized"}')
            return

        try:
            payload = json.loads(body)
            if "model" not in payload or "messages" not in payload:
                raise ValueError("missing model/messages")
        except (ValueError, json.JSONDecodeError):
            self._respond(400, b'{"error": "invalid payload"}')
            return

        if scenario == "ok":
            self._respond(200, json.dumps(VALID_RESPONSE).encode())

        elif scenario.startswith("temp"):
            # temp<value>: require the payload to carry that temperature.
            try:
                expected = float(scenario[4:])
            except ValueError:
                self._respond(404, b"unknown scenario")
                return
            got = payload.get("temperature")
            if isinstance(got, (int, float)) and abs(float(got) - expected) < 1e-9:
                self._respond(200, json.dumps(VALID_RESPONSE).encode())
            else:
                self._respond(400, b'{"error": "missing or wrong temperature"}')
        elif scenario == "badjson":
            self._respond(200, b"this is not json {{{")
        elif scenario == "emptychoices":
            self._respond(200, b'{"choices": []}')
        elif scenario == "nocontent":
            self._respond(
                200,
                b'{"choices": [{"index": 0, '
                b'"message": {"role": "assistant"}, "finish_reason": "stop"}]}',
            )
        else:
            self._respond(404, b"unknown scenario")

    def log_message(self, format, *args):  # silence request logging
        pass


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
    print(server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
