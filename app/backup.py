"""Nightly backup of finished days to a private git repo, then pruning of what is safely backed up.

Every finished local day (in BACKUP_TZ) becomes one CSV per device, data/<device>/<year>/<date>.csv, committed
and pushed to BACKUP_REPO. Rows older than RETENTION_DAYS are deleted from the database, but only for days whose
last push matches what is in the database now. A late row (the logger was offline and caught up days later) makes a
day differ from its backup, so that day is exported again and the file is updated in a new commit.

With BACKUP_REPO unset nothing is backed up and nothing is ever deleted.
"""
import asyncio
import csv
import io
import logging
import os
import shlex
import subprocess
import threading
import time
from datetime import date, datetime, time as dtime, timedelta, timezone
from typing import List, Optional
from zoneinfo import ZoneInfo

from . import db

log = logging.getLogger("rtd.backup")
_lock = threading.Lock()
status = {"last_run": None, "last_ok": None, "last_error": None, "last_summary": None}

REPO_README = """# RTD logger data

Daily backups written automatically by the RTD logger server. One CSV per device per day:
`data/<device_id>/<year>/<YYYY-MM-DD>.csv`.

- The day in the file name is the calendar day in {tz}; `timestamp_utc` inside is UTC.
- Columns: `ch1`..`ch8` in degrees Celsius, two decimals, empty when a sensor reported a fault.
- A file is rewritten if readings from that day arrive late (a logger that was offline catching up).
"""


def config() -> dict:
    env = os.environ.get
    tz_name = env("BACKUP_TZ", "Asia/Kolkata")
    base = os.path.dirname(os.path.abspath(db.db_path()))
    return {
        "repo": env("BACKUP_REPO", "").strip(),
        "key": env("BACKUP_SSH_KEY", "").strip(),
        "tz_name": tz_name,
        "tz": ZoneInfo(tz_name),
        "workdir": os.path.abspath(env("BACKUP_DIR") or os.path.join(base, "backup-repo")),
        "retention_days": int(env("RETENTION_DAYS", "30")),
        "at": env("BACKUP_AT", "00:30"),
    }


def _git(cfg: dict, *args: str, cwd: Optional[str] = None) -> str:
    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")
    if cfg["key"]:
        known_hosts = os.path.join(os.path.dirname(cfg["workdir"]), "known_hosts")
        env["GIT_SSH_COMMAND"] = ("ssh -i %s -o IdentitiesOnly=yes -o StrictHostKeyChecking=accept-new "
                                  "-o UserKnownHostsFile=%s" % (shlex.quote(cfg["key"]), shlex.quote(known_hosts)))
    done = subprocess.run(["git", *args], cwd=cwd or cfg["workdir"], env=env, capture_output=True,
                          text=True, timeout=180)
    if done.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (args[0], (done.stderr or done.stdout).strip()[-400:]))
    return done.stdout


def _ensure_repo(cfg: dict) -> None:
    workdir = cfg["workdir"]
    if not os.path.isdir(os.path.join(workdir, ".git")):
        parent = os.path.dirname(workdir)
        os.makedirs(parent, exist_ok=True)
        _git(cfg, "clone", cfg["repo"], workdir, cwd=parent)
    _git(cfg, "config", "user.name", os.environ.get("BACKUP_GIT_NAME", "rtd-logger backup"))
    _git(cfg, "config", "user.email", os.environ.get("BACKUP_GIT_EMAIL", "rtd-logger@users.noreply.github.com"))
    try:
        _git(cfg, "rev-parse", "--verify", "HEAD")
    except RuntimeError:  # a brand-new empty repo has no branch yet
        _git(cfg, "checkout", "-B", "main")


def day_bounds(day: date, tz) -> tuple:
    start = datetime.combine(day, dtime.min, tzinfo=tz)
    end = datetime.combine(day + timedelta(days=1), dtime.min, tzinfo=tz)
    return int(start.timestamp()), int(end.timestamp())


def _csv_text(device_id: str, start: int, end: int) -> str:
    buf = io.StringIO()
    writer = csv.writer(buf, lineterminator="\n")
    writer.writerow(["timestamp_utc"] + [f"ch{i}" for i in range(1, db.CHANNELS + 1)])
    for row in db.iter_export(device_id, start, end - 1):
        stamp = datetime.fromtimestamp(row["ts"], tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        writer.writerow([stamp] + ["" if row[c] is None else f"{row[c]:.2f}" for c in db.COLS])
    return buf.getvalue()


def _days_with_data(cfg: dict, device_id: str, until_day: date):
    """Yield (day, start, end, count, newest_received) for each local day before until_day that has rows."""
    first = db.first_ts(device_id)
    if first is None:
        return
    day = datetime.fromtimestamp(first, tz=cfg["tz"]).date()
    while day < until_day:
        start, end = day_bounds(day, cfg["tz"])
        count, newest = db.day_stats(device_id, start, end)
        if count:
            yield day, start, end, count, newest
        day += timedelta(days=1)


def _unchanged(device_id: str, day: date, count: int, newest: int) -> bool:
    done = db.get_backup_day(device_id, day.isoformat())
    return bool(done) and done["row_count"] == count and done["max_received"] == newest


def _export_and_push(cfg: dict, today: date, summary: dict) -> None:
    pending = []
    for device_id in db.device_ids():
        for day, start, end, count, newest in _days_with_data(cfg, device_id, today):
            if not _unchanged(device_id, day, count, newest):
                pending.append((device_id, day, start, end, count, newest))
    if not pending:
        return

    _ensure_repo(cfg)
    readme = os.path.join(cfg["workdir"], "README.md")
    if not os.path.exists(readme):
        with open(readme, "w", encoding="utf-8", newline="\n") as f:
            f.write(REPO_README.format(tz=cfg["tz_name"]))
    for device_id, day, start, end, count, _ in pending:
        folder = os.path.join(cfg["workdir"], "data", device_id, str(day.year))
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, f"{day.isoformat()}.csv"), "w", encoding="utf-8", newline="\n") as f:
            f.write(_csv_text(device_id, start, end))

    _git(cfg, "add", "-A")
    if _git(cfg, "status", "--porcelain").strip():
        days = sorted({d.isoformat() for _, d, *_ in pending})
        title = "Back up %s" % (days[0] if len(days) == 1 else "%s to %s" % (days[0], days[-1]))
        _git(cfg, "commit", "-m", "%s (%d device-days)" % (title, len(pending)))
    if _git(cfg, "remote").strip():
        try:
            _git(cfg, "pull", "--rebase", "--autostash", "origin", "main")
        except RuntimeError as e:
            if "couldn't find remote ref" not in str(e):  # an empty remote has no main yet, which is fine
                raise
        _git(cfg, "push", "-u", "origin", "HEAD:main")
    for device_id, day, _start, _end, count, newest in pending:
        db.set_backup_day(device_id, day.isoformat(), count, newest)
    summary["exported_days"] = len(pending)
    summary["pushed"] = True


def _prune(cfg: dict, today: date, summary: dict) -> None:
    keep_from = today - timedelta(days=cfg["retention_days"] - 1)
    for device_id in db.device_ids():
        for day, start, end, count, newest in list(_days_with_data(cfg, device_id, keep_from)):
            if _unchanged(device_id, day, count, newest):
                summary["pruned_rows"] += db.delete_range(device_id, start, end)
            else:
                summary["kept_days"] += 1
                log.warning("not pruning %s %s: no matching backup yet", device_id, day)


def run(now: Optional[datetime] = None) -> dict:
    """One backup-and-prune pass. Never raises; problems are recorded in `status` and the summary."""
    cfg = config()
    summary = {"exported_days": 0, "pushed": False, "pruned_rows": 0, "kept_days": 0}
    with _lock:
        status["last_run"] = int(time.time())
        if not cfg["repo"]:
            summary["skipped"] = "BACKUP_REPO is not set: no backup, and nothing is deleted"
            status.update(last_ok=None, last_error=None, last_summary=summary)
            return summary
        today = (now or datetime.now(cfg["tz"])).astimezone(cfg["tz"]).date()
        error = None
        try:
            _export_and_push(cfg, today, summary)
        except Exception as e:  # the push failed: keep everything, try again next time
            error = str(e)
            log.error("backup failed: %s", error)
        try:
            _prune(cfg, today, summary)  # only touches days whose earlier push still matches
        except Exception as e:
            error = error or str(e)
            log.error("prune failed: %s", e)
        if error:
            summary["error"] = error
        else:
            status["last_ok"] = int(time.time())
        status.update(last_error=error, last_summary=summary)
        log.info("backup run: %s", summary)
        return summary


def info() -> dict:
    cfg = config()
    return {"enabled": bool(cfg["repo"]), "retention_days": cfg["retention_days"], "timezone": cfg["tz_name"],
            "runs_at": cfg["at"], **status, "devices": db.backup_summary()}


def _next_run(cfg: dict, now: datetime) -> datetime:
    hour, minute = (int(x) for x in cfg["at"].split(":"))
    target = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
    return target if target > now else target + timedelta(days=1)


async def scheduler() -> None:
    """Catch up shortly after start-up, then run once a day at BACKUP_AT."""
    await asyncio.sleep(45)
    while True:
        await asyncio.to_thread(run)
        cfg = config()
        now = datetime.now(cfg["tz"])
        wait = (_next_run(cfg, now) - now).total_seconds()
        if status["last_error"]:
            wait = min(wait, 3600)  # a failed push is retried within the hour
        await asyncio.sleep(max(60, wait))


if __name__ == "__main__":
    import json
    logging.basicConfig(level=logging.INFO)
    db.init_db()
    print(json.dumps(run(), indent=2))
