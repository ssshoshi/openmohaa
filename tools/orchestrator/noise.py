"""Console lines worth an agent's attention, without the retail game's spam.

The sidecar passes the console log through this before a line becomes a turn
event, and `orch.py backlog show` passes items filed before it through it too.
Two kinds of line go:

- DROP: what the retail game and the mods print all the time and that never
  says anything about the turn (a missing precache hint, a player animation
  the mod doesn't have, a missing localization entry);
- repeats: the same line, numbers and the timestamp aside, is kept once, with
  how many times it came ("x14").
"""

import re

DROP = [
    r"Add the following line to the \*?_?precache\.scr map script",
    r"^\s*cache models/",
    r"Can't find player animation '",
    r"Couldn't find view model animation",
    r"Couldn't find animation '.*' - trying sound alias instead",
    r"LOCALIZATION ERROR",
    r"Couldn't find image file for shader",
    r"Weapon::SurfaceCommand : group material\d+ not found",
    r"Attack (medium|long|short) range default case for",
    r"^\s*Reason: couldn't find (end|start) node",
]
_DROP = re.compile("|".join(DROP), re.I)

# "[2026-10-03 01:17:06 UTC-5.000] " in front of every logged line
_STAMP = re.compile(r"^\[\d{4}-\d\d-\d\d [\d:]+ UTC[-+][\d.]+\]\s*")
_NUMBERS = re.compile(r"-?\d+(\.\d+)?")


def strip_stamp(line):
    return _STAMP.sub("", line).strip()


def dropped(line):
    return bool(_DROP.search(strip_stamp(line)))


def key(line):
    """Two lines that differ only in numbers (entity numbers, positions) are
    the same line."""
    return _NUMBERS.sub("#", strip_stamp(line)).lower()


def collapse(events):
    """A turn's or an item's events with the spam dropped and repeated console
    lines kept once, counted. Other events are left as they are, in order."""
    out, seen = [], {}
    for e in events:
        if e.get("type") != "console":
            out.append(e)
            continue
        line = e.get("line", "")
        if dropped(line):
            continue
        k = key(line)
        if k in seen:
            seen[k]["count"] = seen[k].get("count", 1) + 1
            continue
        e = dict(e, line=strip_stamp(line))
        seen[k] = e
        out.append(e)
    return out
