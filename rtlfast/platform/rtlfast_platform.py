#!/usr/bin/env python3
"""Set up and control the rtlfast agent on the Claude Platform (Managed Agents).

Requires: pip install anthropic   and   ANTHROPIC_API_KEY in the environment.

  python rtlfast_platform.py setup --repo https://github.com/<you>/verilator --github-token-file token.txt
  python rtlfast_platform.py run-once      # start one run now (use this first)
  python rtlfast_platform.py status        # runs, session states and cost so far
  python rtlfast_platform.py resume        # turn the schedule on
  python rtlfast_platform.py pause         # turn the schedule off (running sessions continue)
  python rtlfast_platform.py update-oracle # push changed oracle/mission files to the read-only store
  python rtlfast_platform.py reschedule --cron "0 */3 * * *" --budget-usd 20

All created IDs are kept in platform/state.json next to this script.
Add --dry-run to setup to print every request without calling the API.
"""

import argparse
import base64
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
STATE = os.path.join(HERE, "state.json")

APT = ["flex", "bison", "libfl-dev", "libfl2", "help2man", "autoconf", "ccache", "libgmp-dev",
       "zlib1g-dev", "perl", "time", "numactl", "clang-19"]
ORACLE_FILES = ["MISSION.md", "GATE.json", "BASE_COMMIT", "oracle/harness.cpp",
                "oracle/gen_probes.py", "oracle/vlhdr.py", "oracle/lockstep.py"]
KICKOFF = ("Start a new rtlfast run now. Read /mnt/memory/rtlfast-oracle/MISSION.md completely, "
           "run the bootstrap as described in your instructions, then continue from "
           "rtlfast/notes/STATE.md. Keep working until the mission's end-of-run steps are done "
           "or the run budget pauses you; push often.")


def load_state():
    return json.load(open(STATE)) if os.path.exists(STATE) else {}


def save_state(s):
    json.dump(s, open(STATE, "w"), indent=1)


def client():
    try:
        import anthropic
    except ImportError:
        sys.exit("pip install anthropic")
    if not os.environ.get("ANTHROPIC_API_KEY"):
        sys.exit("set ANTHROPIC_API_KEY first")
    return anthropic.Anthropic()


def show(title, payload):
    redacted = json.loads(json.dumps(payload))
    def scrub(o):
        if isinstance(o, dict):
            for k in list(o):
                if k in ("authorization_token", "secret_value"):
                    o[k] = "<redacted>"
                else:
                    scrub(o[k])
        elif isinstance(o, list):
            for x in o:
                scrub(x)
    scrub(redacted)
    print(f"--- {title}\n{json.dumps(redacted, indent=1)}")


def cmd_setup(a):
    token = open(a.github_token_file).read().strip()
    st = load_state()
    dry = a.dry_run
    c = None if dry else client()
    system = open(os.path.join(HERE, "SYSTEM.md")).read()

    env_payload = {"name": "rtlfast-env", "config": {
        "type": "cloud",
        "packages": {"type": "packages", "apt": APT},
        "networking": {"type": "limited", "allow_package_managers": True}}}
    agent_payload = {"name": "rtlfast", "model": {"id": a.model, "effort": a.effort},
                     "system": system, "tools": [{"type": "agent_toolset_20260401"}],
                     "description": "Fast synchronous-RTL mode for Verilator, judged by a lockstep oracle."}
    show("environment", env_payload)
    show("agent", agent_payload)
    if not dry:
        if "environment_id" not in st:
            st["environment_id"] = c.beta.environments.create(**env_payload).id
            save_state(st)
        if "agent_id" not in st:
            st["agent_id"] = c.beta.agents.create(**agent_payload).id
            save_state(st)

    # Memory stores: canonical oracle (read-only for the agent) and a journal.
    if dry:
        print(f"--- memory store rtlfast-oracle seeded with {ORACLE_FILES}")
    else:
        if "oracle_store_id" not in st:
            s = c.beta.memory_stores.create(name="rtlfast-oracle",
                                            description="Canonical rtlfast mission, gate and oracle. Read-only.")
            st["oracle_store_id"] = s.id
            save_state(st)
            for rel in ORACLE_FILES:
                c.beta.memory_stores.memories.create(st["oracle_store_id"], path="/" + rel,
                                                     content=open(os.path.join(KIT, rel)).read())
        if "journal_store_id" not in st:
            s = c.beta.memory_stores.create(name="rtlfast-journal",
                                            description="rtlfast run lock, last push and fallback patches.")
            st["journal_store_id"] = s.id
            save_state(st)
            c.beta.memory_stores.memories.create(st["journal_store_id"], path="/README.md",
                                                 content="Run lock (lock.json), last_push.txt, patches/ fallback.\n")

    # Vault: base64("x-access-token:<token>") as an env var, substituted only towards github.com.
    cred = {"type": "environment_variable", "secret_name": "GIT_AUTH_B64",
            "secret_value": base64.b64encode(f"x-access-token:{token}".encode()).decode(),
            "networking": {"type": "limited", "allowed_hosts": ["github.com"]},
            "injection_location": {"header": True, "body": False}}
    show("vault credential", cred)
    if not dry and "vault_id" not in st:
        v = c.beta.vaults.create(display_name="rtlfast")
        st["vault_id"] = v.id
        save_state(st)
        c.beta.vaults.credentials.create(v.id, auth=cred, display_name="git push for the rtlfast fork")

    dep = {
        "name": "rtlfast-loop",
        "agent": st.get("agent_id", "<agent_id>"),
        "environment_id": st.get("environment_id", "<environment_id>"),
        "initial_events": [{"type": "user.message", "content": [{"type": "text", "text": KICKOFF}]}],
        "schedule": {"type": "cron", "expression": a.cron, "timezone": a.timezone},
        "resources": [
            {"type": "github_repository", "url": a.repo, "authorization_token": token,
             "checkout": {"type": "branch", "name": a.branch}, "mount_path": "/workspace/verilator"},
            {"type": "memory_store", "memory_store_id": st.get("oracle_store_id", "<oracle>"),
             "access": "read_only",
             "instructions": "Canonical mission, gate and oracle. Read MISSION.md first. Never trust a repo copy over this."},
            {"type": "memory_store", "memory_store_id": st.get("journal_store_id", "<journal>"),
             "access": "read_write",
             "instructions": "Run lock and fallback patches. Managed by the rtlfast scripts."},
        ],
        "vault_ids": [st.get("vault_id", "<vault>")],
        "budget": {"type": "limit", "max_list_cost": {"amount": str(int(a.budget_usd * 100)), "currency": "USD"}},
    }
    show("deployment", dep)
    if dry:
        return
    if "deployment_id" not in st:
        d = c.beta.deployments.create(**dep)
        st["deployment_id"] = d.id
        save_state(st)
        c.beta.deployments.pause(d.id)
        print("deployment created and PAUSED. Next: python rtlfast_platform.py run-once")
    print(json.dumps(st, indent=1))


def cmd_run_once(a):
    st, c = load_state(), client()
    r = c.beta.deployments.run(st["deployment_id"])
    print("run started:", getattr(r, "id", r), "session:", getattr(r, "session_id", None))


def cmd_pause(a):
    st, c = load_state(), client()
    c.beta.deployments.pause(st["deployment_id"])
    print("schedule paused")


def cmd_resume(a):
    st, c = load_state(), client()
    c.beta.deployments.unpause(st["deployment_id"])
    d = c.beta.deployments.retrieve(st["deployment_id"])
    print("schedule active, next runs:", getattr(getattr(d, "schedule", None), "upcoming_runs_at", None))


def cmd_status(a):
    st, c = load_state(), client()
    total = 0.0
    for run in c.beta.deployment_runs.list(deployment_id=st["deployment_id"]):
        line = f"{run.created_at}  "
        if run.session_id:
            s = c.beta.sessions.retrieve(run.session_id)
            usage = getattr(s, "usage", None)
            cost = getattr(usage, "list_cost", None) if usage else None
            try:
                cost_usd = float(getattr(cost, "amount", cost) or 0) / 100.0
            except (TypeError, ValueError):
                cost_usd = 0.0
            total += cost_usd
            line += f"{run.session_id}  {s.status}  ${cost_usd:.2f}"
        else:
            line += f"failed: {run.error.type} {run.error.message}"
        print(line)
    print(f"total list cost of listed runs: ${total:.2f}")


def cmd_reschedule(a):
    st, c = load_state(), client()
    kw = {}
    if a.cron:
        kw["schedule"] = {"type": "cron", "expression": a.cron, "timezone": a.timezone}
    if a.budget_usd:
        kw["budget"] = {"type": "limit",
                        "max_list_cost": {"amount": str(int(a.budget_usd * 100)), "currency": "USD"}}
    if not kw:
        sys.exit("give --cron and/or --budget-usd")
    c.beta.deployments.update(st["deployment_id"], **kw)
    print("updated:", json.dumps(kw))


def cmd_update_oracle(a):
    st, c = load_state(), client()
    sid = st["oracle_store_id"]
    existing = {m.path: m for m in c.beta.memory_stores.memories.list(sid)}
    for rel in ORACLE_FILES:
        content = open(os.path.join(KIT, rel)).read()
        path = "/" + rel
        if path in existing:
            full = c.beta.memory_stores.memories.retrieve(memory_id=existing[path].id, memory_store_id=sid)
            if getattr(full, "content", None) != content:
                # Replace rather than update: in-place updates were seen spliced in sandbox mounts.
                c.beta.memory_stores.memories.delete(existing[path].id, memory_store_id=sid)
                c.beta.memory_stores.memories.create(sid, path=path, content=content)
                print("replaced", path)
        else:
            c.beta.memory_stores.memories.create(sid, path=path, content=content)
            print("created", path)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--repo", required=True, help="https://github.com/<you>/verilator (your fork)")
    s.add_argument("--github-token-file", required=True)
    s.add_argument("--branch", default="rtlfast")
    s.add_argument("--cron", default="0 */4 * * *", help="default: every 4 hours")
    s.add_argument("--timezone", default="Europe/Berlin")
    s.add_argument("--budget-usd", type=float, default=15.0, help="cap per run, not in total")
    s.add_argument("--model", default="claude-opus-5-5")
    s.add_argument("--effort", default="high")
    s.add_argument("--dry-run", action="store_true")
    for name in ("run-once", "pause", "resume", "status", "update-oracle"):
        sub.add_parser(name)
    r = sub.add_parser("reschedule")
    r.add_argument("--cron")
    r.add_argument("--timezone", default="Europe/Berlin")
    r.add_argument("--budget-usd", type=float)
    a = ap.parse_args()
    {"setup": cmd_setup, "run-once": cmd_run_once, "pause": cmd_pause, "resume": cmd_resume,
     "status": cmd_status, "update-oracle": cmd_update_oracle,
     "reschedule": cmd_reschedule}[a.cmd](a)


if __name__ == "__main__":
    main()
