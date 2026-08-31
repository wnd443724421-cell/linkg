# Cellular Status implementation

Baseline: `wnd443724421-cell/linkg` @ `1aa7ea9d526d3ace09ccebe972cae39cd0d08b70`

## Implemented

- `cellular_status` no longer owns a worker thread. It is driven by the `network-cell` Owner through `cellular_status_get_deadline()` and `cellular_status_process()`.
- Added targeted refresh masks for network mode, SIM, registration, radio, PDP, PDP address, QNETDEV, expected Host network parameters and Linux Host state.
- Added independent watchdog deadlines with convergence/steady intervals.
- Added per-fact metadata: confirmed / attempted time / successful update time / latest query error.
- Query failures retain the most recently confirmed fact; explicit absence is published as a successful fact.
- QENG atomically owns `network_type + serving_cell`; `-ENODATA` is normalized to confirmed no-serving-cell.
- Public Host IPv4/Global IPv6 are sourced only from Linux `usb0`.
- Internal facts retain modem PDP addresses, QNETDEV state, `netmaskset` expected IPv4/IPv6 parameters and actual Host routes for later Monitor/Recovery use.
- Snapshot publication is a whole-structure replacement under the Status lock.
- `rg255_query_registration()` no longer depends on `network_type`.
- Registration state `stat=3` maps to `DENIED`; AUTO merges DENIED only when both EPS and 5GS domains are denied.

## Files

Replacement source files:

- `modules/cellular/cellular_status.h`
- `modules/cellular/cellular_status.c`
- `platform/cellular/rg255/rg255_query.h`
- `include/linkg/modules/cellular/linkg_cellular_status.h` (same public contract as baseline, included for completeness)

Patches against the exact baseline:

- `patches/rg255_query.c.patch`
- `patches/rg255_query_test.c.patch`
- `patches/rg255_query_fixture_test.c.patch`

## Validation performed

- `cellular_status.c` compiled with `gcc -std=gnu17 -Wall -Wextra -Wpedantic` against interface-compatible stubs.
- A behavioral harness covered initial full collection, retained values after query failure, QENG `-ENODATA`, PDP inactive semantics, QNETDEV disconnect semantics, Host IPv6 disappearance and stale network-mode fallback.
- The same harness passed with AddressSanitizer + UndefinedBehaviorSanitizer.

A complete repository build was not run in this environment because the private `1aa7ea9d...` tree is not mounted locally. Use `tools/apply_to_linkg.sh <repo-root>` on a local checkout at that baseline, then run the normal project `./build.sh` and cellular fixture tests.

## Deliberately not implemented yet

- Cellular Monitor / URC parsing and event queue.
- `network-cell` Owner loop integration.
- Recovery decisions/actions.
- Netlink event integration; Host state currently retains the 800 ms fallback watchdog.
