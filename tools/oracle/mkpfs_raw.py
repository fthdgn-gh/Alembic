# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs MkPFS `pack folder --raw --no-compress` with a fixed clock.

    mkpfs_raw.py <source folder> <output.ffpfs> <unix time> [block size] [more MkPFS flags...]

Needs an environment with PSBrew/MkPFS installed (tools/oracle/compare.sh uses
.refs/mkpfs-venv). MkPFS stamps the image with time.time(); pinning it makes
the image reproducible so Alembic's output can be compared byte for byte. The
AMPR index is turned off: MkPFS would write it into the source folder.
"""
import sys
import time


def main() -> int:
    source, output, stamp = sys.argv[1], sys.argv[2], int(sys.argv[3])
    block_size = sys.argv[4] if len(sys.argv) > 4 else "auto"
    time.time = lambda: float(stamp)  # build_pfs reads int(time.time())
    from mkpfs import cli

    sys.argv = [
        "mkpfs", "pack", "folder", "--raw", "--no-compress", "--no-ampr-index",
        "--no-adjust-output-file-extension", "--block-size", block_size, *sys.argv[5:], source, output,
    ]
    return cli.main() or 0


if __name__ == "__main__":
    sys.exit(main())
