"""Parse Verilator-generated headers.

Extracts the design state members of the root class (<prefix>___024root.h)
and the top-level ports of the model class (<prefix>.h).

Only plain data members that hold design state are returned. Verilator's own
scheduling and bookkeeping members (names starting with "__V", vlSymsp, ...)
are excluded, because a different backend may legitimately keep different
bookkeeping.
"""

import re
import sys

# e.g.  ::CData/*6:0*/ TestHarness__DOT__foo;
#       ::VlWide<3>/*75:0*/ TestHarness__DOT__bar;
#       ::VlUnpacked<::VlWide<8>/*255:0*/, 512> TestHarness__DOT__mem;
_SCALAR = r"(?:::)?(CData|SData|IData|QData|EData)"
_WIDE = r"(?:::)?VlWide<(\d+)>"
MEMBER_RE = re.compile(
    r"^\s*(?P<type>(?:::)?(?:CData|SData|IData|QData|EData|VlWide<\d+>|VlUnpacked<.*>))"
    r"\s*(?:/\*(?P<msb>\d+):(?P<lsb>\d+)\*/)?\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*;\s*$"
)
PORT_RE = re.compile(
    r"^\s*VL_(?P<dir>IN|OUT|INOUT)(?P<kind>8|16|64|W)?\(&(?P<name>[A-Za-z_][A-Za-z0-9_]*),\s*"
    r"(?P<msb>\d+),\s*(?P<lsb>\d+)(?:,\s*(?P<words>\d+))?\);"
)

EXCLUDE_PREFIXES = ("__V", "vlSymsp", "vlNamep")


def parse_root(path):
    """Return list of dicts: name, type, kind (scalar|wide|unpacked), width."""
    out = []
    seen = set()
    with open(path) as f:
        for line in f:
            m = MEMBER_RE.match(line)
            if not m:
                continue
            name = m.group("name")
            if name.startswith(EXCLUDE_PREFIXES) or name in seen:
                continue
            t = m.group("type").lstrip(":")
            if t.startswith("VlUnpacked"):
                kind = "unpacked"
            elif t.startswith("VlWide"):
                kind = "wide"
            else:
                kind = "scalar"
            width = None
            if m.group("msb") is not None:
                width = int(m.group("msb")) - int(m.group("lsb")) + 1
            seen.add(name)
            out.append({"name": name, "type": t, "kind": kind, "width": width})
    return out


def parse_ports(path):
    """Return list of dicts: name, dir, kind, width."""
    out = []
    with open(path) as f:
        for line in f:
            m = PORT_RE.match(line)
            if not m:
                continue
            width = int(m.group("msb")) - int(m.group("lsb")) + 1
            out.append({
                "name": m.group("name"),
                "dir": m.group("dir"),
                "kind": m.group("kind") or "8",
                "width": width,
            })
    return out


if __name__ == "__main__":
    for p in sys.argv[1:]:
        if p.endswith("___024root.h"):
            ms = parse_root(p)
            kinds = {}
            for m in ms:
                kinds[m["kind"]] = kinds.get(m["kind"], 0) + 1
            print(p, len(ms), kinds)
        else:
            print(p, parse_ports(p))
