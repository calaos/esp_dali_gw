#!/usr/bin/env python3
"""Send one command to an esp_dali_gw gateway over MQTT and print the matching result.

Requires paho-mqtt, which is deliberately NOT part of the devcontainer image:

    pip install --user paho-mqtt

Examples:

    mqtt_cli.py scan
    mqtt_cli.py scan --deep
    mqtt_cli.py set 3 --level 128
    mqtt_cli.py set group:0 --off
    mqtt_cli.py set broadcast --level-pct 50 --fade-time 4
    mqtt_cli.py query 3 actual_level
    mqtt_cli.py raw FF08 --send-twice
    mqtt_cli.py rename 3 "Kitchen ceiling"
    mqtt_cli.py get-config
    mqtt_cli.py watch

The base topic defaults to the wildcard "dali_gw/+"; the concrete one is resolved from the
retained <base>/status message before publishing, so a single gateway on the broker needs no
--base-topic at all.
"""

import argparse
import json
import os
import random
import sys
import threading
import time

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is missing: pip install --user paho-mqtt")

DEFAULT_HOST = os.environ.get("DALI_GW_MQTT_HOST", "localhost")
DEFAULT_PORT = int(os.environ.get("DALI_GW_MQTT_PORT", "1883"))
DEFAULT_BASE = os.environ.get("DALI_GW_BASE_TOPIC", "dali_gw/+")

LONG_ACTIONS = {"scan", "commission", "poll_all"}


def make_client(args):
    """paho 2.x requires an explicit callback API version; 1.x does not know the argument."""
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        client = mqtt.Client()
    if args.username:
        client.username_pw_set(args.username, args.password)
    client.connect(args.host, args.port, keepalive=30)
    client.loop_start()
    return client


def resolve_base(client, base, timeout):
    """Turn a wildcard base topic into the concrete one of the first gateway that answers."""
    if "+" not in base and "#" not in base:
        return base

    found = []
    ready = threading.Event()

    def on_message(_client, _userdata, msg):
        found.append(msg.topic[: -len("/status")])
        ready.set()

    client.on_message = on_message
    client.subscribe(base + "/status", qos=0)
    if not ready.wait(timeout):
        client.unsubscribe(base + "/status")
        sys.exit(
            "no retained {}/status on the broker: pass --base-topic explicitly".format(base)
        )
    client.unsubscribe(base + "/status")
    client.on_message = None
    return found[0]


class Collector:
    """Collects results matching our correlation id, and prints progress while it waits."""

    def __init__(self, base, want_id, quiet_progress=False):
        self.base = base
        self.want_id = want_id
        self.quiet_progress = quiet_progress
        self.results = []
        self.event = threading.Event()

    def on_message(self, _client, _userdata, msg):
        try:
            payload = json.loads(msg.payload.decode("utf-8", "replace"))
        except ValueError:
            return

        if msg.topic == self.base + "/event/progress":
            if not self.quiet_progress:
                print(
                    "  {operation}: {done}/{total} (found {found})".format(
                        operation=payload.get("operation", "?"),
                        done=payload.get("done", 0),
                        total=payload.get("total", 0),
                        found=payload.get("found", 0),
                    ),
                    file=sys.stderr,
                )
            return

        if not msg.topic.startswith(self.base + "/result/"):
            return
        if payload.get("id") not in (self.want_id, str(self.want_id)):
            return
        self.results.append(payload)
        self.event.set()


def is_started_ack(result):
    data = result.get("data")
    return bool(result.get("ok")) and isinstance(data, dict) and data.get("started") is True


def send(args, action, payload, topic_suffix=None):
    client = make_client(args)
    base = resolve_base(client, args.base_topic, args.timeout)

    want_id = random.randint(1, 2**31 - 1)
    payload = dict(payload)
    payload["id"] = want_id

    collector = Collector(base, want_id)
    client.on_message = collector.on_message
    client.subscribe([(base + "/result/#", args.qos), (base + "/event/progress", args.qos)])
    # The subscription must be live before the command is published, or a fast result is missed.
    time.sleep(0.2)

    suffix = topic_suffix or ("cmd/" + action)
    client.publish(base + "/" + suffix, json.dumps(payload), qos=args.qos)

    deadline = time.time() + args.timeout
    result = None
    while time.time() < deadline:
        if not collector.event.wait(0.2):
            continue
        collector.event.clear()
        while collector.results:
            result = collector.results.pop(0)
            if is_started_ack(result) and action in LONG_ACTIONS:
                print("started, waiting for the final result...", file=sys.stderr)
                deadline = time.time() + args.long_timeout
                result = None
                continue
            client.loop_stop()
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0 if result.get("ok") else 1

    client.loop_stop()
    print("timed out waiting for a result with id {}".format(want_id), file=sys.stderr)
    return 2


def watch(args):
    client = make_client(args)
    base = args.base_topic

    def on_message(_client, _userdata, msg):
        body = msg.payload.decode("utf-8", "replace")
        try:
            body = json.dumps(json.loads(body), sort_keys=True)
        except ValueError:
            pass
        flag = " (retained)" if msg.retain else ""
        print("{}{} {}".format(msg.topic, flag, body), flush=True)

    client.on_message = on_message
    client.subscribe(base + "/#", qos=args.qos)
    print("watching {}/# — ^C to stop".format(base), file=sys.stderr)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        client.loop_stop()
    return 0


def parse_target(text):
    """"3" -> gear/3/set, "group:0" -> group/0/set, "broadcast" -> broadcast/set."""
    if text == "broadcast":
        return "broadcast/set"
    if text.startswith("group:"):
        return "group/{}/set".format(int(text.split(":", 1)[1]))
    return "gear/{}/set".format(int(text))


def set_payload(args):
    payload = {}
    if args.level is not None:
        payload["level"] = args.level
    if args.level_pct is not None:
        payload["level_pct"] = args.level_pct
    if args.on:
        payload["on"] = True
    if args.off:
        payload["on"] = False
    if args.scene is not None:
        payload["scene"] = args.scene
    if args.cmd is not None:
        payload["cmd"] = args.cmd
    if args.mirek is not None:
        payload["mirek"] = args.mirek
    if args.rgb is not None:
        payload["rgb"] = [int(v) for v in args.rgb.split(",")]
    if args.fade_time is not None:
        payload["fade_time"] = args.fade_time
    if not payload:
        sys.exit("nothing to set: pass --level, --on, --off, --cmd, ...")
    return payload


def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--base-topic", default=DEFAULT_BASE)
    parser.add_argument("--username")
    parser.add_argument("--password")
    parser.add_argument("--qos", type=int, default=0, choices=(0, 1, 2))
    parser.add_argument("--timeout", type=float, default=10.0, help="seconds (default 10)")
    parser.add_argument(
        "--long-timeout", type=float, default=180.0, help="seconds for scan/commission"
    )

    sub = parser.add_subparsers(dest="action", required=True)

    scan = sub.add_parser("scan")
    scan.add_argument("--deep", action="store_true")

    com = sub.add_parser("commission")
    com.add_argument("--mode", choices=("unaddressed", "all"), default="unaddressed")
    com.add_argument("--confirm", action="store_true")
    com.add_argument("--start-addr", type=int, default=0)

    sub.add_parser("cancel")
    sub.add_parser("bus-check")
    sub.add_parser("get-gears")
    sub.add_parser("get-config")

    setp = sub.add_parser("set")
    setp.add_argument("target", help="short address, group:N or broadcast")
    setp.add_argument("--level", type=int)
    setp.add_argument("--level-pct", type=int)
    setp.add_argument("--on", action="store_true")
    setp.add_argument("--off", action="store_true")
    setp.add_argument("--scene", type=int)
    setp.add_argument("--cmd", help="up, down, step_up, recall_max, ...")
    setp.add_argument("--mirek", type=int)
    setp.add_argument("--rgb", help="r,g,b")
    setp.add_argument("--fade-time", type=int)

    query = sub.add_parser("query")
    query.add_argument("addr", type=int)
    query.add_argument("name", nargs="?", help="query name, e.g. actual_level")
    query.add_argument("--opcode", type=int)

    raw = sub.add_parser("raw")
    raw.add_argument("frame", help="4 or 6 hex digits")
    raw.add_argument("--send-twice", action="store_true")
    raw.add_argument("--expect-reply", action="store_true")

    ident = sub.add_parser("identify")
    ident.add_argument("addr", type=int)

    ren = sub.add_parser("rename")
    ren.add_argument("addr", type=int, help="short address, or group number with --group")
    ren.add_argument("name")
    ren.add_argument("--group", action="store_true")

    setcfg = sub.add_parser("set-config")
    setcfg.add_argument("file", help="JSON document, or - for stdin")

    for name in ("reboot", "factory-reset"):
        p = sub.add_parser(name)
        p.add_argument("--confirm", action="store_true", required=True)

    sub.add_parser("watch")
    return parser


def main():
    args = build_parser().parse_args()
    action = args.action

    if action == "watch":
        return watch(args)
    if action == "set":
        return send(args, "set", set_payload(args), topic_suffix=parse_target(args.target))

    if action == "scan":
        return send(args, "scan", {"deep": args.deep})
    if action == "commission":
        return send(
            args,
            "commission",
            {"mode": args.mode, "confirm": args.confirm, "start_addr": args.start_addr},
        )
    if action == "cancel":
        return send(args, "cancel", {})
    if action == "bus-check":
        return send(args, "bus_check", {})
    if action == "get-gears":
        return send(args, "get_gears", {})
    if action == "get-config":
        return send(args, "get_config", {})
    if action == "query":
        payload = {"addr": args.addr}
        if args.opcode is not None:
            payload["opcode"] = args.opcode
        elif args.name:
            payload["query"] = args.name
        else:
            sys.exit("pass a query name or --opcode")
        return send(args, "query", payload)
    if action == "raw":
        return send(
            args,
            "raw",
            {
                "frame": args.frame,
                "send_twice": args.send_twice,
                "expect_reply": args.expect_reply,
            },
        )
    if action == "identify":
        return send(args, "identify", {"addr": args.addr})
    if action == "rename":
        key = "group" if args.group else "addr"
        return send(args, "rename", {key: args.addr, "name": args.name})
    if action == "set-config":
        text = sys.stdin.read() if args.file == "-" else open(args.file).read()
        return send(args, "set_config", json.loads(text))
    if action in ("reboot", "factory-reset"):
        return send(args, action.replace("-", "_"), {"confirm": args.confirm})

    sys.exit("unhandled action " + action)



if __name__ == "__main__":
    sys.exit(main())
