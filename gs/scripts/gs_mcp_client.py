import argparse
import json
import socket
import sys


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 17654


def send_request(sock, request):
    sock.sendall((json.dumps(request) + "\n").encode())
    response_bytes = bytearray()
    while True:
        chunk = sock.recv(65535)
        if not chunk:
            break
        response_bytes.extend(chunk)
        response = bytes(response_bytes).decode().strip()
        try:
            return json.loads(response)
        except json.JSONDecodeError:
            continue
    response = bytes(response_bytes).decode().strip()
    if not response:
        raise RuntimeError("Empty MCP response")
    return json.loads(response)


def extract_structured_content(response):
    if "result" not in response:
        return response
    result = response["result"]
    if isinstance(result, dict) and "structuredContent" in result:
        return result["structuredContent"]
    return result


def main():
    parser = argparse.ArgumentParser(description="Call the embedded GS MCP server from inside WSL.")
    parser.add_argument("command", choices=["ping", "snapshot", "menu-buffer", "tools-list", "press-key", "press-keys"])
    parser.add_argument("values", nargs="*", help="Key name or key names for press-key/press-keys.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        send_request(sock, {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}})

        if args.command == "ping":
            response = send_request(sock, {"jsonrpc": "2.0", "id": 2, "method": "ping"})
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

        if args.command == "tools-list":
            response = send_request(sock, {"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

        if args.command == "snapshot":
            request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": "gs_get_snapshot", "arguments": {}},
            }
            response = send_request(sock, request)
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

        if args.command == "menu-buffer":
            request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": "gs_get_menu_buffer", "arguments": {}},
            }
            response = send_request(sock, request)
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

        if args.command == "press-key":
            if len(args.values) != 1:
                raise SystemExit("press-key requires exactly one key name")
            request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": "gs_press_key", "arguments": {"key": args.values[0]}},
            }
            response = send_request(sock, request)
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

        if args.command == "press-keys":
            if not args.values:
                raise SystemExit("press-keys requires at least one key name")
            request = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/call",
                "params": {"name": "gs_press_keys", "arguments": {"keys": args.values}},
            }
            response = send_request(sock, request)
            print(json.dumps(extract_structured_content(response), indent=2, ensure_ascii=False))
            return 0

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
