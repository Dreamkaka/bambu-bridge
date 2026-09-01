#!/usr/bin/env python3
# ============================================================================
# python_example.py - talk to bambu-bridge over HTTP/REST JSON.
#
#   ./build/bambu-bridge --plugin-dir ./linux_02.05.02.58 --country CN \
#                        --http 8080 --no-stdio
#   python3 examples/python_example.py
# ============================================================================
import json
import requests

BASE = "http://127.0.0.1:8080"


def rpc(method, params=None, timeout=20.0):
    r = requests.post(BASE + "/rpc", json={"method": method, "params": params or {}},
                      timeout=timeout)
    r.raise_for_status()
    return r.json()


if __name__ == "__main__":
    try:
        print("version:", rpc("version"))
        print("create_agent:", rpc("create_agent", {"log_dir": "/tmp"}))
        print("set_country_code:", rpc("set_country_code", {"country_code": "CN"}))
        print("connect_server:", rpc("connect_server"))
        print("is_server_connected:", rpc("is_server_connected"))
        print("is_user_login:", rpc("is_user_login"))
        print("user_id:", rpc("get_user_id"))
        print("user_name:", rpc("get_user_name"))

        serial = "22E8BJ630901079"
        print("add_subscribe:", rpc("add_subscribe", {"dev_list": [serial]}))

        # Ask printer to push full state (plugin signs it -> resolves mqtt verify failed)
        push = rpc("send_message_to_printer", {
            "dev_id": serial,
            "json_str": json.dumps({"pushing": {"sequence_id": "0", "command": "pushall"}}),
            "qos": 0,
            "flag": 0,
        })
        print("pushall:", push)

        # Pause
        pause = rpc("send_message_to_printer", {
            "dev_id": serial,
            "json_str": json.dumps({"print": {"sequence_id": "0", "command": "pause"}}),
            "qos": 0,
            "flag": 0,
        })
        print("pause:", pause)
    except Exception as e:
        print("ERROR:", e)
