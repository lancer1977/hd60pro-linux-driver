#!/usr/bin/env python3
"""Hardware-free contract test for the #61 ladder mask classifier."""

import sys


def is_prefix(mask: int) -> bool:
    # Keep this identical to mz0380_raw_ladder_is_prefix().
    return ((mask + 1) & mask) == 0


CASES = {
    0x00000000: True,
    0x00000001: True,
    0x00000007: True,
    0xFFFFFFFF: True,
    0x00100007: False,  # high rung with a lower rung clear
    0x80000000: False,  # only the top rung is written
    0x00010003: False,
}


def main() -> int:
    for mask, expected in CASES.items():
        actual = is_prefix(mask)
        if actual != expected:
            print(f"FAIL mask=0x{mask:08x}: got {actual}, want {expected}")
            return 1
    print(f"PASS raw ladder classifier: {len(CASES)} synthetic masks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
