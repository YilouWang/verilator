#!/usr/bin/env python3
"""uvm_pr_harvest/harvest_via_gh.py

Pull merged-PR metadata from the upstream verilator/verilator repo via the
GitHub REST API and group it by the five UVM-enabling thematic stages.

Authentication: set GITHUB_TOKEN to a personal access token with public_repo
scope to lift the 60 req/h anonymous rate limit. The script also works without
a token but will paginate slowly and may hit the limit on a fresh run.

Output: stage_<NN>.json (raw merged-PR list) and stage_<NN>.csv (date, number,
title, merged_at, url) for each of the five stages.

Why both this script and harvest.sh? `harvest.sh` operates on the local git
history (fast, offline, every commit), but PR titles in Verilator are
authoritative on GitHub (the merged commit subject is sometimes squashed and
truncated). Use this script when you need the canonical PR title or the
PR description body.
"""

from __future__ import annotations

import csv
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

REPO = "verilator/verilator"
OUT = Path(__file__).parent / "out_gh"
OUT.mkdir(exist_ok=True)

# (stage_id, label, query_terms, since_iso)
STAGES: list[tuple[str, str, list[str], str]] = [
    ("01_scheduling", "Scheduling & Time Semantics",
     ["timing", "coroutine", "scheduler", "NBA", "non-blocking",
      "fork", "join", "delay", "V3Sched", "V3Timing", "V3Delayed"],
     "2020-01-01"),
    ("02_oop", "Classes & Dynamic Objects",
     ["class", "extends", "super.new", "virtual method", "virtual function",
      "inheritance", "polymorph", "parameterized class", "null handle"],
     "2020-01-01"),
    ("03_random", "Constrained Randomization",
     ["randomize", "constraint", "rand_mode", "constraint_mode",
      "solve before", "randc", "std::randomize", "dist", "soft constraint",
      "extern constraint", "pre_randomize", "post_randomize"],
     "2020-01-01"),
    ("04_iface", "Interface Polymorphism",
     ["virtual interface", "modport", "clocking block", "VirtIface",
      "interface methods", "parameterized interface"],
     "2020-01-01"),
    ("05_sva", "SystemVerilog Assertions",
     ["assert property", "cover property", "assume property",
      "concurrent assertion", "sequence", "property", "$sampled",
      "disable iff", "first_match", "throughout", "until",
      "named sequence", "intersect", "goto repetition",
      "V3Assert", "NFA", "SVA"],
     "2018-01-01"),
]


def gh_get(url: str) -> tuple[dict | list, dict]:
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "uvm-pr-harvest",
    })
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    backoff = 2
    for _ in range(5):
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return json.loads(resp.read()), dict(resp.headers)
        except urllib.error.HTTPError as e:
            if e.code in (403, 429) and "rate" in e.headers.get("X-RateLimit-Remaining", ""):
                reset = int(e.headers.get("X-RateLimit-Reset", "0"))
                wait = max(reset - int(time.time()), backoff)
                print(f"rate-limited; sleeping {wait}s", file=sys.stderr)
                time.sleep(wait)
            else:
                raise
        except urllib.error.URLError:
            time.sleep(backoff)
            backoff *= 2
    raise RuntimeError(f"giving up on {url}")


def search_prs(terms: list[str], since: str) -> list[dict]:
    """Run the GitHub search API once per term and de-dup by PR number.

    Each search is constrained to merged PRs in the upstream repo since
    `since`. Search returns at most 1000 results per query, which is plenty
    for any single keyword inside one repo.
    """
    seen: dict[int, dict] = {}
    for term in terms:
        q = f'repo:{REPO} is:pr is:merged "{term}" merged:>={since}'
        page = 1
        while page <= 10:  # 10 * 100 = 1000 hard ceiling per term
            url = (
                "https://api.github.com/search/issues?"
                + urllib.parse.urlencode({"q": q, "per_page": 100, "page": page})
            )
            data, _ = gh_get(url)
            items = data.get("items", []) if isinstance(data, dict) else []
            if not items:
                break
            for it in items:
                seen.setdefault(it["number"], it)
            if len(items) < 100:
                break
            page += 1
            time.sleep(0.5)  # polite gap between pages
        time.sleep(0.5)
    return sorted(seen.values(), key=lambda it: it.get("closed_at") or "")


def write_stage(stage_id: str, label: str, prs: list[dict]) -> None:
    json_path = OUT / f"{stage_id}.json"
    csv_path = OUT / f"{stage_id}.csv"
    json_path.write_text(json.dumps(prs, indent=2))
    with csv_path.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["stage", "merged_at", "number", "title", "url"])
        for pr in prs:
            w.writerow([
                label,
                pr.get("closed_at", ""),
                pr["number"],
                re.sub(r"\s+", " ", pr["title"]).strip(),
                pr["html_url"],
            ])
    print(f"{stage_id}: {len(prs)} PRs -> {csv_path.name}")


def main() -> None:
    for stage_id, label, terms, since in STAGES:
        prs = search_prs(terms, since)
        write_stage(stage_id, label, prs)


if __name__ == "__main__":
    main()
