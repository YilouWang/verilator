#!/bin/bash
# Push the current branch to origin. Commit first.
# Auth: if a plain push fails, uses the vault-provided GIT_AUTH_B64 (base64 of
# "x-access-token:<token>") as an HTTP header; the platform swaps the
# placeholder for the real value at egress, so the token never enters the sandbox.
# If pushing is impossible, saves the commits as patches in the journal store.
set -uo pipefail
REPO="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
BRANCH="${RTLFAST_BRANCH:-rtlfast}"
JOURNAL="${RTLFAST_JOURNAL_MEM:-/mnt/memory/rtlfast-journal}"
cd "$REPO"
try_push() {
  if [ -n "${GIT_AUTH_B64:-}" ]; then
    git -c http.https://github.com/.extraheader="AUTHORIZATION: basic ${GIT_AUTH_B64}" push origin "HEAD:$BRANCH" "$@"
  else
    git push origin "HEAD:$BRANCH" "$@"
  fi
}
if try_push 2>/tmp/push.err; then
  echo "rtlfast: pushed $(git rev-parse --short HEAD) to $BRANCH"
  [ -d "$JOURNAL" ] && date -u +%FT%TZ" $(git rev-parse HEAD)" > "$JOURNAL/last_push.txt"
  exit 0
fi
if grep -qiE "non-fast-forward|fetch first|rejected" /tmp/push.err; then
  echo "rtlfast: remote moved, rebasing"
  if [ -n "${GIT_AUTH_B64:-}" ]; then
    git -c http.https://github.com/.extraheader="AUTHORIZATION: basic ${GIT_AUTH_B64}" pull --rebase origin "$BRANCH"
  else
    git pull --rebase origin "$BRANCH"
  fi && try_push && { echo "rtlfast: pushed after rebase"; exit 0; }
fi
echo "rtlfast: PUSH FAILED:"; cat /tmp/push.err
if [ -d "$JOURNAL" ]; then
  base="$(git merge-base HEAD "origin/$BRANCH" 2>/dev/null || cat rtlfast/BASE_COMMIT)"
  mkdir -p "$JOURNAL/patches"
  git format-patch -q "$base..HEAD" -o "$JOURNAL/patches/$(date -u +%Y%m%dT%H%M%S)"
  echo "rtlfast: commits saved as patches under $JOURNAL/patches (mention this in REPORT.md)"
fi
exit 1
