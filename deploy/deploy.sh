#!/usr/bin/env bash
# Ship the committed code, the app's certificate and deploy/.env to the server and (re)start it.
#   bash deploy/deploy.sh <server-ip> [path-to-ssh-key]
# Run from the repo root. Only files committed to git are sent (git archive), so uncommitted
# edits are not deployed; certificates and .env are sent separately and never enter git.
set -euo pipefail

HOST="${1:?usage: deploy/deploy.sh <server-ip> [ssh-key]}"
KEY="${2:-deploy/certs/rtd-server-key.pem}"
SSH_OPTS=(-i "$KEY" -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15)
remote() { ssh "${SSH_OPTS[@]}" "ubuntu@$HOST" "$@"; }

echo "waiting for the server to finish installing Docker..."
until remote 'test -f /var/lib/rtd-docker-ready' 2>/dev/null; do sleep 6; done

ARCHIVE="$(mktemp -t rtd-app.XXXXXX.tgz)"
git archive --format=tar.gz -o "$ARCHIVE" HEAD
scp "${SSH_OPTS[@]}" "$ARCHIVE" "ubuntu@$HOST:/tmp/rtd-app.tgz"
rm -f "$ARCHIVE"
remote 'mkdir -p ~/rtd/deploy/certs && tar -xzf /tmp/rtd-app.tgz -C ~/rtd && rm /tmp/rtd-app.tgz'

scp "${SSH_OPTS[@]}" deploy/certs/AmazonRootCA1.pem deploy/certs/rtd-dashboard.cert.pem \
    deploy/certs/rtd-dashboard.private.key "ubuntu@$HOST:~/rtd/deploy/certs/"
scp "${SSH_OPTS[@]}" deploy/.env "ubuntu@$HOST:~/rtd/deploy/.env"
remote 'chmod 600 ~/rtd/deploy/.env ~/rtd/deploy/certs/*.key'

# The backup pushes to GitHub with its own key, created once on the server and never copied off it.
remote 'mkdir -p ~/rtd/deploy/keys && { [ -f ~/rtd/deploy/keys/backup_key ] || ssh-keygen -q -t ed25519 -N "" -C rtd-logger-backup -f ~/rtd/deploy/keys/backup_key; }'

remote 'cd ~/rtd/deploy && docker compose up -d --build'
remote 'cd ~/rtd/deploy && docker compose ps'

echo
echo "Backup deploy key (GitHub: the data repo > Settings > Deploy keys > Add, tick 'Allow write access'):"
remote 'cat ~/rtd/deploy/keys/backup_key.pub'
