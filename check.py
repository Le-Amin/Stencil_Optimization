#!/usr/bin/env python3
"""Compare une image stencil.pgm à l'image de référence (tolérance +/- 1).

Compatible Python 3 (et Python 2.7)."""

import argparse
import sys


def parse_args():
    parser = argparse.ArgumentParser(
        description="Testing script for HPC Stencil coursework",
        fromfile_prefix_chars='@',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--verbose", action='store_true')
    parser.add_argument("--tolerance", default=1, type=int,
                        help="Absolute tolerance to match against reference results")
    parser.add_argument("--ref-stencil-file", required=True,
                        help="reference stencil.pgm results file")
    parser.add_argument("--stencil-file", required=True,
                        help="calculated stencil.pgm results file")
    return parser.parse_args()


def read_pgm(path):
    with open(path, "rb") as f:
        header = f.readline().split()
        data = bytearray(f.read())
    return [h.decode() for h in header], data


def main():
    args = parse_args()
    (ref_format, ref_nx, ref_ny, ref_depth), ref = read_pgm(args.ref_stencil_file)
    (fmt, nx, ny, depth), res = read_pgm(args.stencil_file)

    if not (fmt == ref_format == "P5"):
        print("Error: incorrect file format"); sys.exit(1)
    if not (depth == ref_depth == "255"):
        print("Error: incorrect depth value"); sys.exit(1)
    if not (nx == ref_nx and ny == ref_ny):
        print("Error: image sizes do not match"); sys.exit(1)

    nx, ny = int(nx), int(ny)
    if len(ref) < nx * ny or len(res) < nx * ny:
        print("Error: truncated image data"); sys.exit(1)

    passed = True
    max_diff = 0
    for j in range(ny):
        row = j * nx
        for i in range(nx):
            diff = abs(ref[row + i] - res[row + i])
            if diff > max_diff:
                max_diff = diff
            if diff > args.tolerance:
                passed = False
                if args.verbose:
                    print("Values differ at (%d, %d): ref=%d, res=%d"
                          % (i, j, ref[row + i], res[row + i]))

    print(80 * "-")
    if passed:
        print("Comparison passed: images match with tolerance +/- %d (max diff %d)"
              % (args.tolerance, max_diff))
    else:
        print("Comparison failed (max diff %d)" % max_diff)
    print(80 * "-")
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
