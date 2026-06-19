"""生成 SS928 CLUT 17v2 的轴序×位序 identity 候选 bundle。"""

from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path

import numpy as np

COUNTS = (729, 648, 648, 576, 648, 576, 576, 512)


def candidate(axis_order: tuple[str, ...], bit_order: tuple[str, ...]) -> np.ndarray:
    banks: list[list[tuple[float, float, float]]] = [[] for _ in range(8)]
    for d2 in range(17):
        for d1 in range(17):
            for d0 in range(17):
                coords = dict(zip(axis_order, (d0 / 16, d1 / 16, d2 / 16)))
                bank = (d0 & 1) | ((d1 & 1) << 1) | ((d2 & 1) << 2)
                banks[bank].append((coords["R"], coords["G"], coords["B"]))
    assert tuple(map(len, banks)) == COUNTS
    contiguous = [item for bank in banks for item in bank]
    starts = np.cumsum((0,) + COUNTS[:-1])
    values = []
    zero = (0.0, 0.0, 0.0)
    for i in range(729):
        values.extend((
            contiguous[starts[0] + i],
            contiguous[starts[1] + i] if i < COUNTS[1] else zero,
            contiguous[starts[2] + i],
            contiguous[starts[3] + i] if i < COUNTS[3] else zero,
        ))
    for i in range(648):
        values.extend((
            contiguous[starts[4] + i],
            contiguous[starts[5] + i] if i < COUNTS[5] else zero,
            contiguous[starts[6] + i],
            contiguous[starts[7] + i] if i < COUNTS[7] else zero,
        ))
    q = np.rint(np.asarray(values) * 1023).astype(np.uint32)
    channels = {"R": q[:, 0], "G": q[:, 1], "B": q[:, 2]}
    hi, mid, low = (channels[name] for name in bit_order)
    return ((hi << 20) | (mid << 10) | low).astype("<u4")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--manifest", required=True)
    args = parser.parse_args()
    records = []
    arrays = []
    for axis in itertools.permutations("RGB"):
        for bits in itertools.permutations("RGB"):
            arrays.append(candidate(axis, bits))
            records.append({"index": len(records), "axis_d0d1d2": "".join(axis),
                            "bits_high_mid_low": "".join(bits)})
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.concatenate(arrays).tofile(out)
    Path(args.manifest).write_text(json.dumps(records, indent=2) + "\n")
    print(f"{len(records)} candidates -> {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
