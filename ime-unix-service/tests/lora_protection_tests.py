#!/usr/bin/env python3
"""Encrypted collection through the CLI.

A database written before encrypted recording existed keeps working; setting a
password converts its rows in place, recording stays off without one, and
forgetting the password clears the conversation data while keeping the file
usable.
"""

import json
import os
import pathlib
import sqlite3
import subprocess
import sys
import tempfile

LEGACY_ID = "11111111111111111111111111111111"
PASSWORD_VARIABLE = "LLAVON_TEST_PASSWORD"


def run(cli, *arguments, password=None, environment=None):
    env = dict(os.environ, **(environment or {}))
    values = list(arguments)
    if password is not None:
        env[PASSWORD_VARIABLE] = password
        values += ["--password-env", PASSWORD_VARIABLE]
    return subprocess.run([cli, *values], capture_output=True, text=True, env=env)


def main(cli: str) -> None:
    cli = str(pathlib.Path(cli).resolve())
    with tempfile.TemporaryDirectory(prefix="llavon-lora-protection-") as directory:
        root = pathlib.Path(directory)
        database = root / "commits.sqlite3"
        # The schema a manager from before encrypted recording left behind.
        with sqlite3.connect(database) as connection:
            connection.executescript(
                """
                CREATE TABLE commits (id TEXT PRIMARY KEY, context TEXT NOT NULL, answer TEXT NOT NULL,
                    state TEXT NOT NULL DEFAULT 'pending' CHECK (state IN ('pending','excluded','trained')),
                    committed_at TEXT NOT NULL DEFAULT 'today');
                CREATE TABLE readings (commit_id TEXT NOT NULL REFERENCES commits(id) ON DELETE CASCADE,
                    position INTEGER NOT NULL, reading TEXT NOT NULL, character INTEGER NOT NULL,
                    manually_selected INTEGER NOT NULL CHECK(manually_selected IN (0,1)),
                    PRIMARY KEY(commit_id,position));
                CREATE TABLE lora_runs (id INTEGER PRIMARY KEY, base_revision TEXT NOT NULL,
                    adapter_path TEXT NOT NULL, model_path TEXT NOT NULL, record_count INTEGER NOT NULL,
                    completed_at TEXT NOT NULL DEFAULT 'today');
                INSERT INTO commits (id,context,answer,state) VALUES
                    ('11111111111111111111111111111111','早安','你好','pending');
                INSERT INTO readings VALUES
                    ('11111111111111111111111111111111',0,'ㄋㄧˇ',20320,1),
                    ('11111111111111111111111111111111',1,'ㄏㄠˇ',22909,0);
                """
            )

        listing = run(cli, "list", "--db", str(database))
        assert listing.returncode == 0, listing.stderr
        assert "早安" in listing.stdout and "ㄋㄧˇ" in listing.stdout

        status = json.loads(run(cli, "protection-status", "--db", str(database)).stdout)
        assert status == {"configured": False, "enabled": False}, status
        # Enabling before a password exists is refused.
        assert run(cli, "set-recording", "--db", str(database), "--enabled", "1").returncode != 0
        # Writing a record without a password is impossible, so a plaintext
        # listing is all an old database can offer until it is converted.
        configured = run(cli, "configure-password", "--db", str(database), password="correct horse")
        assert configured.returncode == 0, configured.stderr
        status = json.loads(run(cli, "protection-status", "--db", str(database)).stdout)
        assert status == {"configured": True, "enabled": True}, status
        assert run(cli, "configure-password", "--db", str(database), password="again").returncode != 0

        with sqlite3.connect(database) as connection:
            context, answer, schema_version = connection.execute(
                "SELECT context,answer,schema_version FROM commits WHERE id=?", (LEGACY_ID,)).fetchone()
            assert schema_version == 2, schema_version
            assert context != "早安" and answer != "你好"
            assert all(character in "0123456789abcdef" for character in context)
            assert connection.execute(
                "SELECT COUNT(*) FROM commits WHERE context='早安' OR answer='你好'").fetchone()[0] == 0
            assert connection.execute(
                "SELECT COUNT(*) FROM readings WHERE reading='ㄋㄧˇ' OR character != 0").fetchone()[0] == 0

        # Reading sealed records needs the password; a wrong one is rejected.
        assert run(cli, "list", "--db", str(database)).returncode != 0
        wrong = run(cli, "list", "--db", str(database), password="wrong horse")
        assert wrong.returncode != 0 and "incorrect password" in wrong.stderr, wrong.stderr
        decrypted = run(cli, "list", "--db", str(database), password="correct horse")
        assert decrypted.returncode == 0, decrypted.stderr
        assert "早安" in decrypted.stdout and "你好" in decrypted.stdout and "ㄋㄧˇ" in decrypted.stdout
        # The selection metadata stays readable without the password.
        assert "ㄋㄧˇ" not in run(cli, "list", "--db", str(database)).stdout

        # Disabling stops new records without touching the stored ones.
        assert run(cli, "set-recording", "--db", str(database), "--enabled", "0").returncode == 0
        assert json.loads(run(cli, "protection-status", "--db", str(database)).stdout)["enabled"] is False
        assert run(cli, "set-recording", "--db", str(database), "--enabled", "1").returncode == 0

        # Forgetting the password clears the conversation data and the derived
        # parameters; the database stays usable and the history is untouched.
        with sqlite3.connect(database) as connection:
            connection.execute("INSERT INTO lora_runs (base_revision,adapter_path,model_path,record_count) "
                               "VALUES ('r','a','m',1)")
        forgotten = run(cli, "reset-conversation-data", "--db", str(database))
        assert forgotten.returncode == 0, forgotten.stderr
        assert json.loads(run(cli, "protection-status", "--db", str(database)).stdout) == {
            "configured": False, "enabled": False}
        with sqlite3.connect(database) as connection:
            assert connection.execute("SELECT COUNT(*) FROM commits").fetchone()[0] == 0
            assert connection.execute("SELECT COUNT(*) FROM readings").fetchone()[0] == 0
            assert connection.execute("SELECT COUNT(*) FROM lora_runs").fetchone()[0] == 1
        assert run(cli, "list", "--db", str(database)).stdout.strip() == ""


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: lora_protection_tests.py <llavon-ime-lora>")
    main(sys.argv[1])
