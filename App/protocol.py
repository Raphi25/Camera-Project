"""Protocol-version negotiation helpers shared by GUI transports."""

PROTOCOL_CLIENT_MAX_VERSION = 3
PROTOCOL_LEGACY_VERSION = 1



def parse_protocol_response(lines: list[str]) -> dict | None:
    """Parse the firmware's PROTOCOL response, returning ``None`` if malformed."""
    for line in lines:
        marker = line.find("PROTOCOL ")
        if marker < 0:
            continue
        fields = {}
        for token in line[marker + len("PROTOCOL "):].split():
            if "=" in token:
                key, value = token.split("=", 1)
                fields[key] = value
        try:
            selected = int(fields["selected"])
            current = int(fields["current"])
            minimum = int(fields["minimum"])
            frame = int(fields["frame"])
        except (KeyError, ValueError):
            return None
        return {
            "selected": selected,
            "current": current,
            "minimum": minimum,
            "frame": frame,
            "capabilities": frozenset(
                item for item in fields.get("capabilities", "").split(",") if item
            ),
            "legacy": False,
        }

    if any("Unknown command" in line or line.startswith("ERR PROTOCOL") for line in lines):
        return {
            "selected": PROTOCOL_LEGACY_VERSION,
            "current": PROTOCOL_LEGACY_VERSION,
            "minimum": PROTOCOL_LEGACY_VERSION,
            "frame": 1,
            "capabilities": frozenset(),
            "legacy": True,
        }
    return None


def protocol_has_capability(protocol: dict | None, capability: str) -> bool:
    """Return whether a negotiated protocol advertises a named capability."""
    return bool(protocol and capability in protocol.get("capabilities", ()))
