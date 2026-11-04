#!/bin/sh
# End-to-end checks for user-visible routing behavior: run the compiled
# network test binaries against the local mock OpenAI server and verify the
# streaming/non-streaming request routing through the real HTTP client.
# Run after `make` (uses build/tests/*). Exits 0 when everything passes.
set -u

MOCK_PID=""
cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" >/dev/null 2>&1
    fi
}
trap cleanup EXIT INT TERM

status=0

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP e2e: python3 not found" >&2
    exit 0
fi

: > build/mock_port.txt
python3 tests/mock_server.py > build/mock_port.txt 2> build/mock_server.log &
MOCK_PID=$!

tries=0
while [ ! -s build/mock_port.txt ] && [ "$tries" -lt 50 ]; do
    sleep 0.1
    tries=$((tries + 1))
done
if [ ! -s build/mock_port.txt ]; then
    echo "FAIL e2e: mock server did not start (see build/mock_server.log)" >&2
    exit 1
fi
PORT=$(cat build/mock_port.txt)

# 1. Non-streaming routing: full chat completion through the client suite.
./build/tests/test_openai_client || status=1
./build/tests/test_agent_loop || status=1

# 2. The mock server really serves an OpenAI-style SSE stream on the
#    scenario the streaming tests use (server-side routing check).
if command -v curl >/dev/null 2>&1; then
    body=$(curl -s "http://127.0.0.1:${PORT}/aistream/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer test-key-123" \
        -d '{"model":"test-model","messages":[{"role":"user","content":"Hi"}],"stream":true}')
    case "$body" in
        *"chat.completion.chunk"*"data: [DONE]"*) : ;;
        *) echo "FAIL e2e: unexpected aistream body from mock server" >&2
           status=1 ;;
    esac
fi

# 3. Tool-call round-trip: the toolround scenario must answer with a tool
#    call first and with the final text after a role "tool" message.
if command -v curl >/dev/null 2>&1; then
    body=$(curl -s "http://127.0.0.1:${PORT}/toolround/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer test-key-123" \
        -d '{"model":"test-model","messages":[{"role":"user","content":"Hi"}]}')
    case "$body" in
        *"tool_calls"*"read_file"*) : ;;
        *) echo "FAIL e2e: toolround did not return a tool call first" >&2
           status=1 ;;
    esac

    body=$(curl -s "http://127.0.0.1:${PORT}/toolround/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer test-key-123" \
        -d '{"model":"test-model","messages":[{"role":"user","content":"Hi"},{"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{}"}}]},{"role":"tool","tool_call_id":"call_1","content":"data"}]}')
    case "$body" in
        *"Hello from mock!"*) : ;;
        *) echo "FAIL e2e: toolround did not finish after the tool result" >&2
           status=1 ;;
    esac
fi

# 4. Reasoning: the non-streaming reasoning scenario must carry
#    message.reasoning_content and the streaming one reasoning deltas.
if command -v curl >/dev/null 2>&1; then
    body=$(curl -s "http://127.0.0.1:${PORT}/reasoning/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer test-key-123" \
        -d '{"model":"test-model","messages":[{"role":"user","content":"Hi"}]}')
    case "$body" in
        *"reasoning_content"*"Thinking hard"*) : ;;
        *) echo "FAIL e2e: reasoning scenario lacks reasoning_content" >&2
           status=1 ;;
    esac

    body=$(curl -s "http://127.0.0.1:${PORT}/ssereasoning/v1/chat/completions" \
        -H "Content-Type: application/json" \
        -H "Authorization: Bearer test-key-123" \
        -d '{"model":"test-model","messages":[{"role":"user","content":"Hi"}],"stream":true}')
    case "$body" in
        *"reasoning_content"*"Step one."*"Hello"*"data: [DONE]") : ;;
        *) echo "FAIL e2e: unexpected ssereasoning body from mock server" >&2
           status=1 ;;
    esac
fi

if [ "$status" -eq 0 ]; then
    echo "e2e: OK"
fi
exit $status
