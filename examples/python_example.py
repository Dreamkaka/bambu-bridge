#!/usr/bin/env python3
# ============================================================================
# python_example.py - talk to bambu-bridge over HTTP/REST JSON.
#
#   ./build/bambu-bridge --plugin-dir ./open_bambu_networking_plugin/bin/linux_x64 --country CN \
#                        --http 8080 --no-stdio
#   python3 examples/python_example.py
#
# Login:
#   1. Open http://127.0.0.1:8080/login in a browser
#   2. Complete Bambu account login
#   3. Callback hits /sign-in/callback and installs the session
# ============================================================================
import json
import sys
import requests

BASE = "http://127.0.0.1:8080"


def rpc(method, params=None, timeout=30.0):
    r = requests.post(BASE + "/rpc", json={"method": method, "params": params or {}},
                      timeout=timeout)
    r.raise_for_status()
    return r.json()


if __name__ == "__main__":
    try:
        print("health:", requests.get(BASE + "/health", timeout=5).json())
        print("status:", rpc("status"))
        print("version:", rpc("version"))
        print("is_user_login:", rpc("is_user_login"))

        st = rpc("status").get("result") or {}
        if not st.get("logged_in"):
            login = requests.get(BASE + "/login-url", timeout=5).json()
            print("not logged in. open this URL in a browser:")
            print(" ", login.get("url"))
            print("then re-run this script.")
            sys.exit(0)

        print("user:", rpc("get_user_name"), rpc("get_user_id"))
        printers = rpc("list_printers")
        print("printers:", printers)

        body = ((printers.get("result") or {}).get("http_body")) or "{}"
        devices = []
        try:
            devices = json.loads(body).get("devices") or []
        except Exception:
            pass
        if not devices:
            print("no cloud printers bound to this account")
            sys.exit(0)

        serial = devices[0].get("dev_id")
        print("using:", serial, devices[0].get("dev_name"))
        print("add_subscribe:", rpc("add_subscribe", {"dev_list": [serial]}))
        print("set_user_selected_machine:", rpc("set_user_selected_machine", {"dev_id": serial}))
        push = rpc("pushall", {"dev_id": serial}, timeout=20.0)
        print("pushall payload:", (push.get("result") or {}).get("payload"))
        print("pause:", rpc("pause", {"dev_id": serial}, timeout=20.0))
    except Exception as e:
        print("ERROR:", e)
        sys.exit(1)
