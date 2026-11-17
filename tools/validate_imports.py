#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
"""Verify the launch imports select LncUtil packed inside SystemService."""
import base64
import hashlib
from pathlib import Path
import struct
import sys


def validate(path):
    data = Path(path).read_bytes()
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected a little-endian ELF64 OELF')
    phoff = struct.unpack_from('<Q', data, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', data, 54)
    headers = [struct.unpack_from('<IIQQQQQQ', data, phoff + i * phsize) for i in range(phnum)]
    dynamic = next(p for p in headers if p[0] == 2)
    dynlib = next(p for p in headers if p[0] == 0x61000000)
    tags = []
    for at in range(dynamic[2], dynamic[2] + dynamic[5], 16):
        tag, value = struct.unpack_from('<QQ', data, at)
        if not tag:
            break
        tags.append((tag, value))
    values = dict(tags)
    start = dynlib[2] + values[0x61000035]
    strings = data[start:start + values[0x61000037]]

    def entries(tag):
        return {strings[value & 0xffffffff:].split(b'\0', 1)[0].decode(): value >> 48
                for kind, value in tags if kind == tag}

    modules, libraries = entries(0x6100000f), entries(0x61000015)
    if 'libSceLncUtil' in modules:
        raise ValueError('LncUtil must be packed in SystemService, not a standalone module')
    alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-'

    def encoded_id(value):
        if value < 64:
            return alphabet[value]
        return encoded_id(value // 64) + alphabet[value % 64]

    lib_id = encoded_id(libraries['libSceLncUtil'])
    module_id = encoded_id(modules['libSceSystemService'])
    suffix = bytes.fromhex('518d64a635ded8c1e6b039b1c3e55230')
    for symbol in ('sceLncUtilInitialize', 'sceLncUtilLaunchApp'):
        digest = hashlib.sha1(symbol.encode() + suffix).digest()[:8][::-1]
        nid = base64.b64encode(digest).decode().rstrip('=').replace('/', '-')
        encoded = f'{nid}#{lib_id}#{module_id}'.encode()
        if encoded + b'\0' not in strings:
            raise ValueError(f'{symbol} does not import LncUtil from SystemService')
    print('Launch imports validated: LncUtil inside SystemService')


if __name__ == '__main__':
    try:
        validate(sys.argv[1])
    except (IndexError, KeyError, StopIteration, ValueError, struct.error) as error:
        sys.exit(f'Launch import validation failed: {error}')
