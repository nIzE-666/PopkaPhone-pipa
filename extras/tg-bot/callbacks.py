"""Compact callback_data encoder/decoder (strictly <= 64 UTF-8 bytes)."""

from __future__ import annotations

from typing import Tuple


MAX_CALLBACK_BYTES = 64
SEP = ":"


def pack_cb(scope: str, action: str, *args: object) -> str:
    """Encode callback parameters into a compact colon-delimited string <= 64 bytes."""
    parts = [str(scope).strip(), str(action).strip()]
    for arg in args:
        s = str(arg)
        if SEP in s:
            raise ValueError(f"Callback argument cannot contain '{SEP}': {s!r}")
        parts.append(s)
    if any(not p for p in parts[:2]):
        raise ValueError("Callback scope and action must be non-empty")
    encoded = SEP.join(parts)
    byte_len = len(encoded.encode("utf-8"))
    if byte_len > MAX_CALLBACK_BYTES:
        raise ValueError(f"callback_data exceeds {MAX_CALLBACK_BYTES} bytes ({byte_len}): {encoded!r}")
    return encoded


def unpack_cb(data: str) -> Tuple[str, str, Tuple[str, ...]]:
    """Decode callback_data into (scope, action, extra_args)."""
    if not data or SEP not in data:
        raise ValueError(f"Malformed callback_data: {data!r}")
    parts = data.split(SEP)
    if len(parts) < 2 or not parts[0] or not parts[1]:
        raise ValueError(f"Malformed callback_data: {data!r}")
    return parts[0], parts[1], tuple(parts[2:])
