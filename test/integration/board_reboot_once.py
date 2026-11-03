#!/usr/bin/env python3
"""Reboot exactly one test board through the existing Paramiko helper."""

import argparse
import time

from switch_t0_baseline import connect


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--password", required=True)
    args = parser.parse_args()
    client = connect(args.host, args.password)
    try:
        transport = client.get_transport()
        channel = transport.open_session(timeout=5)
        channel.exec_command("sync; reboot")
        time.sleep(1)
        print(f"reboot_sent host={args.host}", flush=True)
    finally:
        client.close()


if __name__ == "__main__":
    main()
