#!/usr/bin/env python3
# A path style S3 store in memory for the tests of the s3:// backend: PUT, GET with Range, HEAD, DELETE and ListObjectsV2 with pages. It checks the access key of the Authorization header
# against the one the test sets and answers 403 ExpiredToken to any other. The test steers it with GET /__ctl/<command>/<argument>:
#   fail/<status>/<n>   the next n object requests answer <status>
#   range200/<0|1>      a GET with a Range that starts past byte 0 answers 200 and the bytes from byte 0
#   key/<id>            the access key that is valid now ("" accepts any)
#   page/<n>            keys per page of a listing
#   token/<n>           the length of the continuation token of a listing
#   put/<key>           stores a small object at <key> (a path below the bucket)
#   keys                the keys that are stored, one per line
#   count/<method>      how many requests of the method came
import re
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

lock = threading.Lock()
objects = {}
state = {"fail": [], "range200": False, "key": "", "page": 1000, "token": 0, "count": {}}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", headers=None, length=None):
        self.send_response(code)
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body) if length is None else length))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def control(self, path):
        parts = path[len("/__ctl/"):].split("/")
        cmd, args = parts[0], parts[1:]
        with lock:
            if cmd == "fail":
                state["fail"] += [int(args[0])] * int(args[1])
            elif cmd == "range200":
                state["range200"] = args[0] == "1"
            elif cmd == "key":
                state["key"] = args[0] if args else ""
            elif cmd == "page":
                state["page"] = int(args[0])
            elif cmd == "token":
                state["token"] = int(args[0])
            elif cmd == "put":
                objects["/" + "/".join(args)] = b"orphan"
            elif cmd == "keys":
                return self.reply(200, "".join(k + "\n" for k in sorted(objects)).encode())
            elif cmd == "count":
                return self.reply(200, str(state["count"].get(args[0], 0)).encode())
        self.reply(200, b"ok")

    def listing(self, query):
        prefix = query.get("prefix", [""])[0]
        bucket = self.path.split("?")[0].rstrip("/")
        keys = sorted(k for k in objects if k.startswith(bucket + "/") and k[len(bucket) + 1:].startswith(prefix))
        start = int(query["continuation-token"][0].split("x")[0][1:]) if "continuation-token" in query else 0
        page = keys[start:start + state["page"]]
        more = start + state["page"] < len(keys)
        xml = "<ListBucketResult>" + "".join("<Contents><Key>%s</Key></Contents>" % k[len(bucket) + 1:] for k in page)
        xml += "<IsTruncated>%s</IsTruncated>" % ("true" if more else "false")
        if more:
            token = "T%d" % (start + state["page"])
            xml += "<NextContinuationToken>%s</NextContinuationToken>" % (token + "x" * max(0, state["token"] - len(token)))
        self.reply(200, (xml + "</ListBucketResult>").encode(), {"Content-Type": "application/xml"})

    def handle_any(self):
        path = self.path.split("?")[0]
        if path.startswith("/__ctl/"):
            return self.control(path)
        query = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        with lock:
            state["count"][self.command] = state["count"].get(self.command, 0) + 1
            if state["fail"]:
                return self.reply(state["fail"].pop(0), b"<Error><Code>SlowDown</Code></Error>")
            key = re.search(r"Credential=([^/]+)/", self.headers.get("Authorization", ""))
            if state["key"] and (key is None or key.group(1) != state["key"]):
                return self.reply(403, b"<Error><Code>ExpiredToken</Code></Error>")
            if self.command == "PUT":
                objects[path] = body
                return self.reply(200, b"", {"ETag": '"%x"' % hash(body)})
            if self.command == "DELETE":
                objects.pop(path, None)
                return self.reply(204)
            if self.command == "GET" and "list-type" in query:
                return self.listing(query)
            data = objects.get(path)
            if data is None:
                return self.reply(404, b"<Error><Code>NoSuchKey</Code></Error>")
            if self.command == "HEAD":
                return self.reply(200, b"", {"ETag": '"%x"' % hash(data)}, len(data))
            rng = re.match(r"bytes=(\d+)-(\d+)", self.headers.get("Range", ""))
            if rng is None:
                return self.reply(200, data)
            lo, hi = int(rng.group(1)), int(rng.group(2))
            if lo >= len(data):
                return self.reply(416)
            if state["range200"] and lo > 0:
                return self.reply(200, data[:hi - lo + 1])
            return self.reply(206, data[lo:hi + 1], {"Content-Range": "bytes %d-%d/%d" % (lo, min(hi, len(data) - 1), len(data))})

    do_GET = do_PUT = do_HEAD = do_DELETE = handle_any


ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
