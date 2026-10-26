#!/usr/bin/env python3
"""Post-flash live check of the phone remote's GATT gate, from a Mac (bleak).

    python3 -m pip install bleak
    python3 tests/phone_remote/ble_live_check.py            # host check
    python3 tests/phone_remote/ble_live_check.py --phone    # see README

Default (host check), run with the pairing window CLOSED: scans for "Totem",
connects without pairing anything new, and confirms every Remote Input
characteristic refuses reads, writes and subscriptions. AJ's Mac is a bonded
ZMK host, so macOS encrypts the link with that bond (Just Works, L2): this is
the "a host or a Just Works link cannot drive the remote" check. If macOS
offers a pairing prompt, cancel it; ZMK refuses pairing outside the window.

--phone: run on a machine that is NOT bonded to the dongle, after pairing it as
the phone (open the window, read Status, type the passkey into the OS prompt).
Confirms Status reads and that ZMK Studio's service refuses this link.

Payloads are harmless if a check fails: a key release of a key nobody holds,
text containing 0x01 (the handler refuses it), Control keepalive, an all-zero
mouse packet. Exit status 0 when every check passed.
"""
import argparse
import asyncio
import re
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

B = "-5c1a-4b2e-9d3f-8a6b4c2d1e0f"
SVC = "7e4e0001" + B
MOUSE, KEY, TEXT, CTRL, STATUS = ("7e4e000%d" % n + B for n in (2, 3, 4, 5, 6))
STUDIO_SVC = "00000000-0196-6107-c967-c5cfb1c2482a"
STUDIO_RPC = "00000001-0196-6107-c967-c5cfb1c2482a"

GATE = {5, 8, 15}  # Insufficient Authentication / Authorization / Encryption
HANDLER = {0x0D, 0x13, 0x80, 0x81}  # the write got past the gate

results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")


def att_code(err):
    """ATT error code from a CoreBluetooth or BlueZ error, or None."""
    m = re.search(r"Code=(\d+)", str(err)) or re.search(r"ATT error: 0x([0-9a-f]+)", str(err), re.I)
    if not m:
        return None
    return int(m.group(1), 16) if "0x" in m.group(0) else int(m.group(1))


async def refused(name, op):
    """op must fail at the gate, not in the handler and not succeed."""
    try:
        await op()
    except (BleakError, OSError) as e:
        code = att_code(e)
        ok = code not in HANDLER  # unknown codes count as refused; printed
        check(name, ok, f"refused ({code if code is not None else e})")
        return
    check(name, False, "ACCEPTED")


async def find(name):
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    hits = [(d, a) for d, a in found.values() if (a.local_name or d.name or "") == name]
    for d, a in hits:
        uuids = [u.lower() for u in a.service_uuids]
        print(f"found {name} {d.address} rssi {a.rssi} services {uuids}")
        print("      advertising for a paired phone" if SVC in uuids else "      ZMK advertising")
    return hits[0][0] if hits else None


async def host_check(c):
    svc = c.services.get_service(SVC)
    check("remote service present", svc is not None)
    if svc is None:
        return
    await refused("Status read", lambda: c.read_gatt_char(STATUS))
    await refused("Status subscribe", lambda: c.start_notify(STATUS, lambda *_: None))
    await refused("Key write (release 'a')", lambda: c.write_gatt_char(KEY, bytes([0, 0, 7, 0, 4, 0]), response=True))
    await refused("Text write (0x01)", lambda: c.write_gatt_char(TEXT, b"\x01", response=True))
    await refused("Control write (keepalive)", lambda: c.write_gatt_char(CTRL, b"\x02", response=True))
    # Write Without Response has no reply: nothing to see from this side.
    await c.write_gatt_char(MOUSE, bytes(8), response=False)
    print("INFO  Mouse is Write Without Response: refusal is not visible here; "
          "the dongle log must show no 'remote:' line for it")


async def phone_check(c):
    try:
        v = await c.read_gatt_char(STATUS)
        check("Status read as the phone", len(v) >= 6, v.hex(" "))
    except BleakError as e:
        check("Status read as the phone", False, str(e))
    if c.services.get_service(STUDIO_SVC) is None:
        print("INFO  Studio service not listed (OS hid it?): check it with nRF Connect")
        return
    await refused("Studio RPC read", lambda: c.read_gatt_char(STUDIO_RPC))
    await refused("Studio RPC write", lambda: c.write_gatt_char(STUDIO_RPC, b"\x00", response=True))
    print("INFO  HID and raw-HID (0x1812) are hidden from apps by macOS/iOS: "
          "check them with nRF Connect on the phone (README)")


async def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--name", default="Totem")
    ap.add_argument("--phone", action="store_true", help="this machine is paired as the phone")
    args = ap.parse_args()

    dev = await find(args.name)
    if dev is None:
        print(f"no device named {args.name!r} (a phone or host may hold it; ZMK and the "
              "phone advertising only run while the active profile's host is away "
              "or a paired phone is)")
        return 2
    async with BleakClient(dev) as c:
        await (phone_check(c) if args.phone else host_check(c))
    print(f"{results.count(True)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
