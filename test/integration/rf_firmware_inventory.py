#!/usr/bin/env python3
"""Collect non-sensitive hashes for the running HI1105 firmware package."""

import argparse
import json

from switch_t0_baseline import connect, execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--password", required=True)
    args = parser.parse_args()
    sta = connect("192.168.3.1", args.password)
    try:
        result = {
            "uptime": execute(sta, "cat /proc/uptime", 5),
            "hashes": execute(
                sta,
                "sha256sum "
                "/vendor/firmware/hi1105/pilot/WL_ITCM.bin "
                "/vendor/firmware/hi1105/pilot/WCPU_ROM.bin "
                "/vendor/firmware/hi1105/pilot/wifi_cfg "
                "/vendor/firmware/hi1105/pilot/wifi_only_cfg 2>&1",
                15,
            ),
            "modules": execute(
                sta,
                "lsmod | sed -n '1,100p'; "
                "for f in /lib/modules/5.4.61/*wifi*.ko /lib/modules/5.4.61/*hcc*.ko "
                "/lib/modules/5.4.61/*1105*.ko; do "
                "test -f \"$f\" && sha256sum \"$f\"; done",
                15,
            ),
        }
    finally:
        sta.close()
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
