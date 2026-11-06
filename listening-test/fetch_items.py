#!/usr/bin/env python3
"""Fetches the excerpts of items.csv from the ODAQ archive. Uses only the Python standard library.

The archive has 1 GB, of which the test needs a few files. Range requests read just those. The record
on Zenodo cannot change, and zipfile checks the checksum of each file that it reads.
"""
from __future__ import annotations

import argparse
import csv
import io
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCHIVE_URL = "https://zenodo.org/api/records/10405774/files/ODAQ.zip/content"
FOLDER_IN_ARCHIVE = "ODAQ/ODAQ_unprocessed/"
LICENSE_FILES = ("ODAQ/_license_disclaimer.txt", "ODAQ/_detailed_license.csv")
READ_AHEAD_BYTES = 1 << 18


class RemoteFile(io.RawIOBase):
    """A file on a web server that supports range requests, as a seekable file for zipfile."""

    def __init__(self, url: str):
        super().__init__()
        self.position = 0
        request = urllib.request.Request(url, headers={"Range": "bytes=0-0"})
        with urllib.request.urlopen(request) as response:
            # A redirect leads to the storage, and asking there saves one step for each request.
            self.url = response.url
            content_range = response.headers.get("Content-Range")
            if response.status != 206 or not content_range:
                raise OSError(f"{url} does not support range requests")
            self.size = int(content_range.split("/")[1])

    def seekable(self) -> bool:
        return True

    def readable(self) -> bool:
        return True

    def tell(self) -> int:
        return self.position

    def seek(self, offset: int, whence: int = io.SEEK_SET) -> int:
        base = {io.SEEK_SET: 0, io.SEEK_CUR: self.position, io.SEEK_END: self.size}[whence]
        self.position = base + offset
        return self.position

    def readinto(self, buffer) -> int:
        if self.position >= self.size or len(buffer) == 0:
            return 0
        last = min(self.position + len(buffer), self.size) - 1
        request = urllib.request.Request(self.url, headers={"Range": f"bytes={self.position}-{last}"})
        with urllib.request.urlopen(request) as response:
            data = response.read()
        buffer[: len(data)] = data
        self.position += len(data)
        return len(data)


def wanted_files(items_csv: Path) -> list[str]:
    with open(items_csv, newline="", encoding="utf-8") as file:
        return [row["file"] for row in csv.DictReader(file)]


def fetch(url: str, files: list[str], folder: Path) -> list[str]:
    """Writes the files and the license notes into the folder. Returns the names it wrote."""
    folder.mkdir(parents=True, exist_ok=True)
    written = []
    with zipfile.ZipFile(io.BufferedReader(RemoteFile(url), READ_AHEAD_BYTES)) as archive:
        members = [(FOLDER_IN_ARCHIVE + name, name) for name in files]
        members += [(member, Path(member).name) for member in LICENSE_FILES]
        for member, name in members:
            target = folder / name
            if target.exists() and target.stat().st_size == archive.getinfo(member).file_size:
                continue
            target.write_bytes(archive.read(member))
            written.append(name)
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--items", type=Path, default=HERE / "items.csv", help="list of excerpts")
    parser.add_argument("--out", type=Path, default=HERE / "items", help="folder for the audio")
    arguments = parser.parse_args()

    for name in fetch(ARCHIVE_URL, wanted_files(arguments.items), arguments.out):
        print(name)
    print(f"The audio is in {arguments.out}")


if __name__ == "__main__":
    main()
