#!/usr/bin/env python3
"""Exercise the local GUI's authentication, CLI delegation, and job lifetime."""

import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError
from urllib.request import Request, urlopen


def main(gui: str, real_cli: str, tables: str, pinned_commit: str) -> None:
    # The mock CLI delegates record actions to the real executable through a
    # sibling symlink; a relative argument would resolve against the temporary
    # directory and dangle.
    real_cli = str(Path(real_cli).resolve())
    tables = str(Path(tables).resolve())
    with tempfile.TemporaryDirectory(prefix="llavon-lora-gui-") as directory:
        state = Path(directory) / "state"
        state.mkdir(mode=0o700)
        database = state / "commits.sqlite3"
        with sqlite3.connect(database) as db:
            db.executescript("""
                PRAGMA foreign_keys=ON;
                CREATE TABLE commits (id TEXT PRIMARY KEY, context TEXT, answer TEXT,
                    state TEXT DEFAULT 'pending', committed_at TEXT DEFAULT 'today');
                CREATE TABLE readings (commit_id TEXT REFERENCES commits(id) ON DELETE CASCADE,
                    position INTEGER, reading TEXT, character INTEGER, manually_selected INTEGER);
                CREATE TABLE lora_runs (id INTEGER PRIMARY KEY, completed_at TEXT, record_count INTEGER, model_path TEXT);
                INSERT INTO commits (id,context,answer) VALUES
                    ('11111111111111111111111111111111','你好','你');
                INSERT INTO readings VALUES ('11111111111111111111111111111111',0,'ㄋㄧˇ',20320,1);
            """)

        # The fake executable stands in for the CLI's slow jobs.
        # Record actions use the real CLI and its SQLite transactions.
        fake = Path(directory) / "mock-cli"
        fake.write_text("""#!/usr/bin/env python3
import os, pathlib, sys, time
args = sys.argv[1:]
if args[0] in ('exclude', 'delete'):
    os.execv(sys.argv[0] + '.real', [sys.argv[0] + '.real', *args])
if args[0] == 'fetch-model':
    target = pathlib.Path(args[args.index('--output-dir') + 1]); target.mkdir(exist_ok=True)
    (target / 'current.revision').write_text('1' * 40)
    pinned = target / ('1' * 40); pinned.mkdir(exist_ok=True)
    for asset in ('config.json', 'ime_vocab.json', 'model.safetensors'):
        (pinned / asset).write_text('fixture')
    print('downloaded', flush=True)
elif args[0] == 'check-model':
    print('revision=' + '1' * 40, flush=True)
elif args[0] == 'install-trainer':
    target = pathlib.Path(args[args.index('--output-dir') + 1]); target.mkdir(parents=True, exist_ok=True)
    binary = target / 'llavon-lora'; binary.write_text('#!/bin/sh\\nexit 0\\n'); binary.chmod(0o755)
    (target / 'libtorch_cpu.so').write_text('native library fixture')
    (target / 'trainer-release.json').write_text(__import__('json').dumps({'commit':os.environ['MOCK_PINNED_COMMIT']}))
    print('trainer=' + str(binary), flush=True)
elif args[0] == 'train':
    assert 'LLAVON_IME_LORA_CLI_PATH' in os.environ and '--rank' in args and '--selected-ids' in args
    assert '--device' in args and '--shuffle' in args
    selection=__import__('json').loads(pathlib.Path(args[args.index('--selected-ids') + 1]).read_text())
    assert len(selection['selected']) == 1 and len(selection['reviewed']) == 201
    print('step=1/10', flush=True)
    time.sleep(30)
else:
    sys.exit(2)
""")
        fake.chmod(0o700)
        os.symlink(real_cli, str(fake) + ".real")
        env = dict(os.environ)
        env.pop('LLAVON_IME_LORA_CLI_PATH', None)
        env['MOCK_PINNED_COMMIT'] = pinned_commit
        env['LLAVON_IME_LORA_ASSETS_DIR'] = str(Path(directory) / 'assets')
        env['XDG_CONFIG_HOME'] = str(Path(directory) / 'config')
        launchers = Path(directory) / 'bin'
        launchers.mkdir()
        (launchers / 'fcitx5-remote').write_text('#!/bin/sh\nexit 0\n')
        (launchers / 'fcitx5-remote').chmod(0o700)
        env['PATH'] = str(launchers) + os.pathsep + env.get('PATH', '')

        args = [gui, "--no-browser", "--state-dir", str(state), "--cli", str(fake),
                "--tables-dir", tables, "--idle-seconds", "5"]
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            first = process.stdout.readline().strip()
            assert first.startswith("URL=http://127.0.0.1:"), first
            url, token = first[4:].split("#")
            url = url.rstrip('/')

            def request(path: str, body=None, *, auth=True, origin=None):
                headers = {"Host": url.split("//", 1)[1]}
                if auth:
                    headers["X-Llavon-Token"] = token
                if origin:
                    headers["Origin"] = origin
                if body is not None:
                    headers["Content-Type"] = "application/json"
                req = Request(url + path, json.dumps(body).encode() if body is not None else None,
                              headers=headers)
                with urlopen(req, timeout=3) as response:
                    return response.read().decode()

            def raw(path: str, body=None, *, auth=True, origin=None, host=None,
                    content_type="application/json", method=None):
                headers = {"Host": host or url.split("//", 1)[1]}
                if auth:
                    headers["X-Llavon-Token"] = token
                if origin:
                    headers["Origin"] = origin
                data = None
                if body is not None:
                    data = body if isinstance(body, bytes) else json.dumps(body).encode()
                    headers["Content-Type"] = content_type
                req = Request(url + path, data, headers, method=method)
                try:
                    with urlopen(req, timeout=3) as response:
                        return response.status, response.read().decode()
                except HTTPError as error:
                    return error.code, error.read().decode()

            page = request('/', auth=False)
            assert '拉風輸入法・個人化訓練' in page
            assert 'rel="icon"' in page and 'class="brand-logo" src="data:image/png;base64' in page
            assert '@@LOGO@@' not in page
            with urlopen(Request(url + '/', headers={'Host': url.split('//', 1)[1]}), timeout=3) as response:
                assert "img-src 'self' data:" in response.headers.get('Content-Security-Policy', '')
            readings = json.loads(request('/api/readings'))
            assert 'ㄋㄧˇ' in readings['你'] and '€' not in readings
            for rejected in [lambda: request('/api/records', auth=False),
                             lambda: request('/api/records', origin='https://example.com')]:
                try:
                    rejected()
                    raise AssertionError('unauthorized access was accepted')
                except HTTPError as error:
                    assert error.code == 403
            # Authentication, origin, host, routing and malformed requests.
            assert raw('/api/records', auth=False)[0] == 403
            assert raw('/api/records', origin='https://example.com')[0] == 403
            assert raw('/api/records', host='example.com')[0] == 403
            assert raw('/api/records', origin='http://' + url.split('//', 1)[1])[0] == 200
            assert raw('/api/does-not-exist')[0] == 404
            assert raw('/api/does-not-exist', body={})[0] == 404
            assert raw('/api/check', body={}, content_type='text/plain')[0] == 400
            assert raw('/api/check', body=b'{not json')[0] == 400
            assert raw('/api/check', body=[1, 2])[0] == 400
            assert raw('/api/records?state=bogus')[0] == 400
            assert raw('/api/records?offset=-1')[0] == 400
            assert raw('/api/records?offset=1000000')[0] == 400
            assert raw('/api/records?nonsense=1')[0] == 400
            assert raw('/api/records/' + 'z' * 32 + '/exclude', body={})[0] == 400
            assert raw('/api/records/' + '0' * 32 + '/bogus', body={})[0] == 400
            assert raw('/api/records/' + 'e' * 32 + '/exclude', body={})[0] == 400
            assert raw('/api/cancel', body={})[0] == 400
            assert raw('/api/use-model', body={'id': 'nope'})[0] == 400
            assert raw('/api/use-model', body={'id': '999'})[0] == 400

            second = subprocess.run(args, capture_output=True, text=True, timeout=5, check=True, env=env)
            assert second.stdout.strip() == first

            records = json.loads(request('/api/records'))['rows']
            assert records[0]['answer'] == '你' and records[0]['readings'] == ['ㄋㄧˇ']
            assert records[0]['manual'] == [True]
            record = records[0]['id']
            status, reason = raw('/api/records/' + record + '/exclude', body={})
            assert status == 200, reason
            assert json.loads(request('/api/records'))['rows'] == []
            assert len(json.loads(request('/api/records?state=excluded&offset=0'))['rows']) == 1
            request('/api/records/' + record + '/delete', {})
            assert json.loads(request('/api/records?state=excluded&offset=0'))['rows'] == []
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT count(*) FROM readings').fetchone()[0] == 0
                db.executemany("INSERT INTO commits (id,context,answer) VALUES (?,?,?)",
                               [(f'{i:032x}', '上下文', '你') for i in range(1, 202)])
            first_page = json.loads(request('/api/records?offset=0'))
            second_page = json.loads(request('/api/records?offset=200'))
            assert len(first_page['rows']) == 200 and first_page['has_more']
            assert len(second_page['rows']) == 1 and not second_page['has_more']

            request('/api/check', {})
            for _ in range(40):
                result = json.loads(request('/api/state'))
                if result['job']['state'] == 'completed':
                    break
                time.sleep(.05)
            assert result['job']['state'] == 'completed'

            request('/api/fetch', {})
            for _ in range(40):
                result = json.loads(request('/api/state'))
                if result['job']['state'] == 'completed':
                    break
                time.sleep(.05)
            assert result['job']['state'] == 'completed' and result['model_ready']
            assert (Path(env['LLAVON_IME_LORA_ASSETS_DIR']) / 'current.revision').is_file()

            request('/api/install-trainer', {})
            for _ in range(40):
                result = json.loads(request('/api/state'))
                if result['job']['state'] == 'completed':
                    break
                time.sleep(.05)
            assert result['job']['state'] == 'completed' and result['trainer_ready']
            stamp = state / 'tools/lora/trainer-release.json'
            stamp.write_text(json.dumps({'commit': '0' * 40}))
            assert not json.loads(request('/api/state'))['trainer_ready']
            stamp.write_text(json.dumps({'commit': pinned_commit}))

            options = {'rank': '8', 'alpha': '16', 'dropout': '0', 'batch-size': '1',
                       'gradient-accumulation': '1', 'epochs': '5', 'max-steps': '-1',
                       'learning-rate': '0.0001', 'weight-decay': '0', 'warmup-steps': '0',
                       'max-grad-norm': '1', 'save-every': '0', 'seed': '42',
                       'max-seq-length': '384', 'target-modules': 'q_proj,v_proj',
                       'device': 'auto', 'dtype': 'float32', 'shuffle': '1'}
            pending = json.loads(request('/api/pending-ids'))
            assert len(pending) == 201
            for payload in [{'ids': [], 'reviewed': pending, 'options': options},
                            {'ids': ['zz'], 'reviewed': pending, 'options': options},
                            {'ids': pending, 'reviewed': 'nope', 'options': options},
                            {'ids': pending, 'reviewed': pending, 'options': {}},
                            {'ids': pending, 'reviewed': pending}]:
                assert raw('/api/train', body=payload)[0] == 400, payload
            request('/api/train', {'ids': [f'{1:032x}'], 'reviewed': pending, 'options': options})
            assert json.loads(request('/api/state'))['trainer_ready']
            for _ in range(40):
                result = json.loads(request('/api/state'))
                if 'step=1/10' in result['job']['log']:
                    break
                time.sleep(.05)
            assert result['job']['state'] == 'running' and 'step=1/10' in result['job']['log']
            assert abs(float(result['job']['percent']) - 13.0) < 0.5
            # A running job rejects every other job and model switch.
            assert raw('/api/fetch', body={})[0] == 400
            assert raw('/api/check', body={})[0] == 400
            assert raw('/api/install-trainer', body={})[0] == 400
            assert raw('/api/train', body={'ids': pending, 'reviewed': pending, 'options': options})[0] == 400
            assert raw('/api/use-model', body={'id': '1'})[0] == 400
            # The list endpoints stay available while the job runs.
            assert len(json.loads(request('/api/pending-ids'))) == 201
            assert len(json.loads(request('/api/runs'))) == 0
            request('/api/cancel', {})
            for _ in range(40):
                result = json.loads(request('/api/state'))
                if result['job']['state'] == 'cancelled':
                    break
                time.sleep(.05)
            assert result['job']['state'] == 'cancelled'
            gguf = Path(directory) / 'personalized-Q4_K_M.gguf'
            gguf.write_text('model')
            with sqlite3.connect(database) as db:
                db.execute('INSERT INTO lora_runs (completed_at,record_count,model_path) VALUES (?,?,?)',
                           ('today', 1, str(Path(directory) / 'missing.gguf')))
                db.execute('INSERT INTO lora_runs (completed_at,record_count,model_path) VALUES (?,?,?)',
                           ('today', 1, str(gguf)))
            runs = json.loads(request('/api/runs'))
            assert len(runs) == 2
            run = runs[0]
            assert run['model_path'] == str(gguf) and run['record_count'] == 1
            assert str(run['rank']) == '8' and str(run['alpha']) == '16'
            assert run['cumulative_count'] == 2 and run['optimizer_steps'] == '0'
            # A run whose model file disappeared cannot be applied.
            assert raw('/api/use-model', body={'id': runs[1]['id']})[0] == 400
            if sys.platform == 'darwin':
                config = Path(env['XDG_CONFIG_HOME']) / 'llavon-ime/config.json'
                original = '{"model_path":"/old/model.gguf","candidate_page_size":9}'
            else:
                config = Path(env['XDG_CONFIG_HOME']) / 'fcitx5/conf/llavon-ime.conf'
                original = 'SelectionKeysCount=9\nModelPath="/old/model.gguf"\n'
            config.parent.mkdir(parents=True)
            config.write_text(original)
            request('/api/use-model', {'id': run['id']})
            request('/api/use-model', {'id': run['id']})  # reapplying replaces the path
            settings = config.read_text()
            if sys.platform == 'darwin':
                assert json.loads(settings) == {'candidate_page_size': 9, 'model_path': str(gguf)}
            else:
                assert 'SelectionKeysCount=9' in settings
                assert 'ModelPath="' + str(gguf) + '"' in settings
                assert settings.count('ModelPath=') == 1
            process.wait(timeout=8)  # no browser heartbeat: the temporary server exits
            assert process.returncode == 0
            # No browser/CLI child retained its singleton lock after exit.
            reopened = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            try:
                new_url = reopened.stdout.readline().strip()
                assert new_url.startswith('URL=http://127.0.0.1:')
                assert reopened.poll() is None
            finally:
                reopened.terminate()
                reopened.wait(timeout=5)
            # A manager built from different sources replaces an idle session so
            # the browser never stays on an outdated interface.
            stale = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
            try:
                stale_url = stale.stdout.readline().strip()
                assert stale.poll() is None
                lock = state / 'gui.lock'
                lines = lock.read_text().splitlines()
                lock.write_text(lines[0] + '\n' + lines[1] + '\nan old build\n')
                replacement = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
                try:
                    replacement_url = replacement.stdout.readline().strip()
                    assert replacement_url.startswith('URL=http://127.0.0.1:')
                    assert replacement_url != stale_url
                    stale.wait(timeout=8)
                    assert stale.returncode == 0
                finally:
                    replacement.terminate()
                    replacement.wait(timeout=5)
            finally:
                if stale.poll() is None:
                    stale.terminate()
                    stale.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if process.returncode != 0:
                print(process.stderr.read(), file=sys.stderr)


if __name__ == '__main__':
    main(*sys.argv[1:])
