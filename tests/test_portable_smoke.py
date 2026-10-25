#!/usr/bin/env python3
# Copyright (c) 2026 ais2api contributors
# SPDX-License-Identifier: MPL-2.0
"""WebUI packaging checks against local HTTP fixtures; no service or browser needed."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading
import unittest
import urllib.request

from portable_smoke import Failure, NoRedirect, PortableSmoke


PAGE = b'''<!doctype html><html><head>
<script type="module" src="/assets/app.js"></script>
<link rel="modulepreload" href="/assets/shared.js">
<link rel="stylesheet" href="/assets/app.css">
</head><body><div id="app"></div></body></html>'''


class WebuiSmokeTests(unittest.TestCase):
    def setUp(self):
        self.routes = {
            "/": (200, "text/html; charset=utf-8", PAGE),
            "/login": (200, "text/html; charset=utf-8", PAGE),
            "/assets/app.js": (200, "text/javascript; charset=utf-8", b"export default {};"),
            "/assets/shared.js": (200, "text/javascript; charset=utf-8", b"export const shared = 1;"),
            "/assets/app.css": (200, "text/css; charset=utf-8", b"body { margin: 0; }"),
        }
        self.requests = []
        routes, requests = self.routes, self.requests

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                status, content_type, body = routes.get(self.path, (404, "text/plain", b"missing"))
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                if status == 302:
                    self.send_header("Location", "/unexpected-redirect")
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

        self.addCleanup(stop)
        self.smoke = PortableSmoke.__new__(PortableSmoke)
        self.smoke.port = server.server_port
        self.smoke.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
        self.smoke.secrets = []
        self.smoke.checks = []
        self.smoke.current = "initialize"

    def test_pages_and_referenced_assets_are_checked_in_each_location(self):
        for prefix in ("original", "relocated"):
            self.smoke.webui(prefix)
            self.assertIn(prefix + "_webui_login", self.smoke.checks)
        self.assertEqual(self.requests, ["/", "/login", "/assets/app.js", "/assets/shared.js", "/assets/app.css"] * 2)

    def test_missing_webui_fails(self):
        self.routes["/"] = (404, "application/json", b'{"error":"not_found"}')
        with self.assertRaisesRegex(Failure, "fixture_webui_home$"):
            self.smoke.webui("fixture")

    def test_missing_preloaded_module_fails(self):
        del self.routes["/assets/shared.js"]
        with self.assertRaisesRegex(Failure, "fixture_webui_asset_1$"):
            self.smoke.webui("fixture")

    def test_missing_login_route_fails(self):
        del self.routes["/login"]
        with self.assertRaisesRegex(Failure, "fixture_webui_login$"):
            self.smoke.webui("fixture")

    def test_page_without_scripts_or_styles_fails(self):
        self.routes["/"] = (200, "text/html", b"<html>empty console</html>")
        with self.assertRaisesRegex(Failure, "fixture_webui_home_entry_assets$"):
            self.smoke.webui("fixture")

    def test_broken_resources_fail_even_when_the_homepage_works(self):
        for response in ((404, "text/plain", b"missing"),
                         (200, "text/html", PAGE),
                         (200, "text/javascript", b"")):
            with self.subTest(response=response[:2]):
                self.routes["/assets/app.js"] = response
                with self.assertRaisesRegex(Failure, "fixture_webui_asset_0$"):
                    self.smoke.webui("fixture")

    def test_references_cannot_request_remote_assets(self):
        self.routes["/"] = (200, "text/html", PAGE.replace(b"/assets/app.js", b"https://example.invalid/app.js"))
        with self.assertRaisesRegex(Failure, "fixture_webui_asset_0_local$"):
            self.smoke.webui("fixture")
        self.assertEqual(self.requests, ["/", "/login"])

    def test_resources_cannot_redirect_to_another_location(self):
        self.routes["/assets/app.js"] = (302, "text/plain", b"redirect")
        with self.assertRaisesRegex(Failure, "fixture_webui_asset_0$"):
            self.smoke.webui("fixture")
        self.assertNotIn("/unexpected-redirect", self.requests)


if __name__ == "__main__":
    unittest.main()
