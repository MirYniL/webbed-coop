"""Rebuild the distributable XOR differences from two GameMaker resource files."""

import json
import struct
import sys
import zipfile


def chunks(path):
    with open(path, "rb") as f:
        assert f.read(4) == b"FORM"
        f.read(4)
        while header := f.read(8):
            tag, size = struct.unpack("<4sI", header)
            yield tag.decode("ascii"), f.read(size)


if __name__ == "__main__":
    with zipfile.ZipFile(
        sys.argv[3], "w", zipfile.ZIP_DEFLATED, compresslevel=9
    ) as out:
        entries = []
        for old, new in zip(chunks(sys.argv[1]), chunks(sys.argv[2]), strict=True):
            tag, before = old
            name, after = new
            assert tag == name
            entries.append((tag, len(before), len(after)))
            # Preserve chunk boundaries so resource offsets do not shift the delta.
            size = len(after)
            diff = (
                int.from_bytes(before[:size], "little")
                ^ int.from_bytes(after, "little")
            ).to_bytes(size, "little")
            if diff.count(0) != size:
                out.writestr(tag, diff)
        out.writestr("layout.json", json.dumps(entries))
