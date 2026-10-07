import os
import shutil
import stat
import subprocess
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import pytest

from app import backup, db

IST = ZoneInfo("Asia/Kolkata")
TYPES = ["PT100"] * 7 + ["PT1000"]


def ist(y, mo, d, h=0, mi=0, s=0):
    return datetime(y, mo, d, h, mi, s, tzinfo=IST)


def add(epochs, device="d1"):
    rows = [(int(e), [20.0 + i * 0.1 for i in range(8)]) for e in epochs]
    db.insert_readings(device, "fw", TYPES, rows)


def destroy(path):
    """Remove a git repo; its object files are read-only, which plain rmtree cannot delete on Windows."""
    def make_writable(func, p, _):
        os.chmod(p, stat.S_IWRITE)
        func(p)
    shutil.rmtree(path, onerror=make_writable)


def git(*args, cwd):
    return subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, check=True).stdout


@pytest.fixture()
def env(tmp_path, monkeypatch):
    remote = tmp_path / "remote.git"
    git("init", "--bare", "-b", "main", str(remote), cwd=tmp_path)
    monkeypatch.setenv("RTD_DB", str(tmp_path / "t.db"))
    monkeypatch.setenv("BACKUP_REPO", str(remote))
    monkeypatch.setenv("BACKUP_DIR", str(tmp_path / "clone"))
    monkeypatch.setenv("BACKUP_TZ", "Asia/Kolkata")
    monkeypatch.setenv("RETENTION_DAYS", "30")
    monkeypatch.delenv("BACKUP_SSH_KEY", raising=False)
    backup.status.update(last_run=None, last_ok=None, last_error=None, last_summary=None)
    db.init_db()

    class Env:
        pass

    e = Env()
    e.remote, e.tmp = remote, tmp_path
    e.commits = lambda: int(git("rev-list", "--count", "main", cwd=remote).strip())
    e.file = lambda path: git("show", f"main:{path}", cwd=remote)
    e.files = lambda: git("ls-tree", "-r", "--name-only", "main", cwd=remote).split()
    return e


def test_finished_days_are_exported_with_ist_boundaries_and_today_is_left_alone(env):
    day5_start = ist(2026, 10, 5).timestamp()
    add([day5_start, ist(2026, 10, 5, 23, 59, 50).timestamp(),   # both on the 5th in IST
         ist(2026, 10, 6, 0, 0, 10).timestamp(),                  # just after midnight: the 6th
         ist(2026, 10, 7, 9, 0, 0).timestamp()])                  # today, not finished
    summary = backup.run(now=ist(2026, 10, 7, 10, 0))
    assert summary["exported_days"] == 2 and summary["pushed"] and "error" not in summary
    assert sorted(f for f in env.files() if f.endswith(".csv")) == [
        "data/d1/2026/2026-10-05.csv", "data/d1/2026/2026-10-06.csv"]
    lines = env.file("data/d1/2026/2026-10-05.csv").strip().split("\n")
    assert lines[0] == "timestamp_utc,ch1,ch2,ch3,ch4,ch5,ch6,ch7,ch8"
    assert len(lines) == 3 and lines[1].startswith("2026-10-04T18:30:00Z,20.00,20.10")
    assert len(env.file("data/d1/2026/2026-10-06.csv").strip().split("\n")) == 2
    assert "README.md" in env.files()


def test_a_second_run_changes_nothing(env):
    add([ist(2026, 10, 5, 12).timestamp()])
    backup.run(now=ist(2026, 10, 7, 10, 0))
    before = env.commits()
    again = backup.run(now=ist(2026, 10, 7, 11, 0))
    assert again["exported_days"] == 0 and env.commits() == before


def test_a_late_row_for_a_backed_up_day_is_exported_again(env):
    add([ist(2026, 10, 5, 12).timestamp()])
    backup.run(now=ist(2026, 10, 7, 10, 0))
    before = env.commits()
    add([ist(2026, 10, 5, 12, 0, 10).timestamp()])  # the logger caught up days later
    summary = backup.run(now=ist(2026, 10, 8, 0, 30))
    assert summary["exported_days"] == 1 and env.commits() == before + 1
    assert len(env.file("data/d1/2026/2026-10-05.csv").strip().split("\n")) == 3


def test_old_rows_are_pruned_only_after_they_are_backed_up(env):
    now = ist(2026, 10, 7, 10, 0)
    old = ist(2026, 8, 20, 12).timestamp()       # 48 days ago
    edge_out = ist(2026, 9, 7, 12).timestamp()   # 30 days ago: outside a 30-day window that includes today
    edge_in = ist(2026, 9, 8, 12).timestamp()    # 29 days ago: the oldest day still kept
    recent = ist(2026, 10, 6, 12).timestamp()
    add([old, edge_out, edge_in, recent])
    summary = backup.run(now=now)
    assert summary["pruned_rows"] == 2 and summary["kept_days"] == 0
    left = [r["ts"] for r in db.recent_rows("d1", 10)]
    assert sorted(left) == sorted([int(edge_in), int(recent)])
    # the pruned days are safe in the backup repo
    assert "data/d1/2026/2026-08-20.csv" in env.files() and "data/d1/2026/2026-09-07.csv" in env.files()


def test_nothing_is_deleted_when_the_push_fails(env):
    add([ist(2026, 8, 20, 12).timestamp(), ist(2026, 10, 6, 12).timestamp()])
    destroy(env.remote)
    summary = backup.run(now=ist(2026, 10, 7, 10, 0))
    assert "error" in summary and summary["pruned_rows"] == 0 and not summary["pushed"]
    assert len(db.recent_rows("d1", 10)) == 2
    assert backup.status["last_error"]


def test_a_day_that_changed_after_its_backup_is_kept_until_it_is_pushed_again(env, monkeypatch):
    old = ist(2026, 8, 20, 12).timestamp()
    add([old])
    monkeypatch.setenv("RETENTION_DAYS", "1000")  # back it up without pruning
    backup.run(now=ist(2026, 10, 7, 10, 0))
    add([old + 10])                                # a late row for that day
    destroy(env.remote)                            # and the next push cannot succeed
    monkeypatch.setenv("RETENTION_DAYS", "30")
    summary = backup.run(now=ist(2026, 10, 8, 0, 30))
    assert summary["pruned_rows"] == 0 and summary["kept_days"] == 1
    assert len(db.recent_rows("d1", 10)) == 2


def test_without_a_backup_repo_nothing_is_ever_deleted(env, monkeypatch):
    monkeypatch.delenv("BACKUP_REPO")
    add([ist(2026, 8, 20, 12).timestamp()])
    summary = backup.run(now=ist(2026, 10, 7, 10, 0))
    assert "skipped" in summary and len(db.recent_rows("d1", 10)) == 1


def test_each_device_gets_its_own_folder(env):
    add([ist(2026, 10, 5, 12).timestamp()], device="d1")
    add([ist(2026, 10, 5, 13).timestamp()], device="d2")
    backup.run(now=ist(2026, 10, 7, 10, 0))
    assert {"data/d1/2026/2026-10-05.csv", "data/d2/2026/2026-10-05.csv"} <= set(env.files())


def test_status_endpoint_reports_the_last_run(env):
    from fastapi.testclient import TestClient
    from app.main import app
    add([ist(2026, 10, 5, 12).timestamp()])
    backup.run(now=ist(2026, 10, 7, 10, 0))
    with TestClient(app) as client:
        info = client.get("/api/backup").json()
    assert info["enabled"] and info["retention_days"] == 30 and info["timezone"] == "Asia/Kolkata"
    assert info["last_ok"] and info["devices"][0]["last_day"] == "2026-10-05"


def test_demo_devices_are_never_backed_up_or_pruned(env):
    add([ist(2026, 8, 20, 12).timestamp()], device="demo-logger")
    add([ist(2026, 8, 20, 12).timestamp()], device="d1")
    summary = backup.run(now=ist(2026, 10, 7, 10, 0))
    assert summary["pruned_rows"] == 1                      # only the real device's old row
    assert not any("demo-logger" in f for f in env.files())
    assert len(db.recent_rows("demo-logger", 10)) == 1      # the demo row is left alone
