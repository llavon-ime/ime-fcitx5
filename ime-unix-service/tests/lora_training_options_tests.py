#!/usr/bin/env python3
"""Verify trainer argv parity, selected-record handling, and manual-choice weight."""
import json
import hashlib
import io
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tarfile
import tempfile


def main(cli, tables, pinned_commit):
    with tempfile.TemporaryDirectory(prefix='llavon-lora-options-') as root:
        root = Path(root)
        revision = 'a' * 40
        model = root / revision
        model.mkdir()
        (model / 'config.json').write_text('{"vocab_size":18546,"max_position_embeddings":384}')
        vocab = [''] * 18546
        for name in ('chars', 'special_tokens', 'bpmf'):
            for token, number in json.loads((Path(tables) / 'tokens' / (name + '.json')).read_text()).items():
                if number < 18546:
                    vocab[number] = token
        (model / 'ime_vocab.json').write_text(json.dumps({'tokens': vocab}, ensure_ascii=False))
        (model / 'model.safetensors').write_text('base checkpoint')
        db = root / 'commits.sqlite3'
        with sqlite3.connect(db) as connection:
            connection.executescript("""
                CREATE TABLE commits(id TEXT PRIMARY KEY,context TEXT,answer TEXT,state TEXT DEFAULT 'pending',
                    committed_at TEXT DEFAULT 'today');
                CREATE TABLE readings(commit_id TEXT,position INTEGER,reading TEXT,character INTEGER,manually_selected INTEGER);
                CREATE TABLE lora_runs(id INTEGER PRIMARY KEY,base_revision TEXT,adapter_path TEXT,model_path TEXT,
                    record_count INTEGER,completed_at TEXT DEFAULT 'now');
                INSERT INTO commits (id,context,answer,state) VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','早安','你','pending');
                INSERT INTO commits (id,context,answer,state) VALUES('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','早安','你','pending');
                INSERT INTO commits (id,context,answer,state) VALUES('cccccccccccccccccccccccccccccccc','早安','你','pending');
                INSERT INTO readings VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',0,'ㄋㄧˇ',20320,1);
                INSERT INTO readings VALUES('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',0,'ㄋㄧˇ',20320,0);
                INSERT INTO readings VALUES('cccccccccccccccccccccccccccccccc',0,'ㄋㄧˇ',20320,0);
            """)
        trainer = root / 'llavon-lora'
        trainer.write_text('''#!/usr/bin/env python3
import json, pathlib, sys
args=sys.argv[1:]
if args == ['--version', '--json']:
    print(json.dumps({'trainerApi':2}));sys.exit(0)
if args[0]=='validate':sys.exit(0)
if args[0]=='train':
    pathlib.Path(__file__+'.args').write_text(json.dumps(args))
    dest=pathlib.Path(args[args.index('--output-dir')+1]);dest.mkdir()
    (dest/'adapter_model.safetensors').write_text('adapter')
    (dest/'training_state.json').write_text('{"step":1}')
elif args[0]=='export-gguf':
    pathlib.Path(args[args.index('--quantized-outfile')+1]).write_text('gguf')
else:sys.exit(2)
''')
        trainer.chmod(0o700)
        selected = root / 'selected.json'
        selected.write_text(json.dumps({'selected':['aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'],
                                        'reviewed':['aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb']}))
        output = root / 'run'
        command = [cli, 'train', '--db', str(db), '--model-dir', str(model), '--tables-dir', tables,
                   '--output-dir', str(output), '--revision', revision,
                   '--selected-ids', str(selected), '--rank', '8', '--alpha', '16', '--dropout', '0',
                   '--max-steps', '1', '--device', 'auto', '--dtype', 'float32', '--shuffle', '0']
        trainer_env = dict(os.environ, LLAVON_IME_LORA_CLI_PATH=str(trainer))
        subprocess.run(command, check=True, capture_output=True, text=True, env=trainer_env)
        args = json.loads(Path(str(trainer) + '.args').read_text())
        assert '--no-shuffle' in args and args[args.index('--max-steps') + 1] == '1'
        assert args[args.index('--device') + 1] == 'auto'
        assert not (output/'training.jsonl').exists(), 'the readable dataset must be removed after training'
        assert (output/'adapter').is_dir()
        with sqlite3.connect(db) as connection:
            assert connection.execute("SELECT state FROM commits WHERE id LIKE 'aaaa%'").fetchone()[0] == 'trained'
            assert connection.execute("SELECT state FROM commits WHERE id LIKE 'bbbb%'").fetchone()[0] == 'excluded'
            assert connection.execute("SELECT state FROM commits WHERE id LIKE 'cccc%'").fetchone()[0] == 'pending'
            assert connection.execute('SELECT record_count,rank,optimizer_steps FROM lora_runs').fetchone() == (1,8,1)
        incompatible = command.copy()
        incompatible[incompatible.index('--rank') + 1] = '16'
        incompatible[incompatible.index('--output-dir') + 1] = str(root / 'second')
        assert subprocess.run(incompatible, capture_output=True, env=trainer_env).returncode != 0

        # The remaining CLI commands are part of the manager's public surface.
        listing = subprocess.run([cli, 'list', '--db', str(db)], check=True, capture_output=True, text=True)
        assert 'cccccccccccccccccccccccccccccccc' in listing.stdout
        dataset = subprocess.run([cli, 'dataset', '--db', str(db), '--model-dir', str(model),
                                  '--tables-dir', tables, '--output', str(root / 'review.jsonl')],
                                 check=True, capture_output=True, text=True)
        assert dataset.stdout.strip() == 'trainable=1 skipped=0'
        for argument in ([], ['nonsense'], ['train', '--db', str(db), '--model-dir', str(model),
                                             '--tables-dir', tables, '--output-dir', str(root / 'x'),
                                             '--revision', revision, '--trainer', str(trainer)]):
            failed = subprocess.run([cli, *argument], capture_output=True, text=True)
            assert failed.returncode != 0, argument
        assert '--trainer is no longer supported' in subprocess.run(
            [cli, 'train', '--db', str(db), '--model-dir', str(model), '--tables-dir', tables,
             '--output-dir', str(root / 'x'), '--revision', revision, '--trainer', str(trainer)],
            capture_output=True, text=True).stderr
        # fetch-model reuses verified assets and only downloads what is missing.
        asset_root = root / 'assets'
        download_log = root / 'downloads.log'
        fake_bin = root / 'curl-bin'
        fake_bin.mkdir()
        payloads = {'config.json': b'config', 'ime_vocab.json': b'vocab', 'model.safetensors': b'weights'}
        metadata = {'sha': revision,
                    'siblings': [{'rfilename': name, 'size': len(payload),
                                  'lfs': {'sha256': __import__('hashlib').sha256(payload).hexdigest()}}
                                 for name, payload in payloads.items()]}
        curl = fake_bin / 'curl'
        curl.write_text('#!/usr/bin/env python3\n'
                        'import pathlib, sys, os\n'
                        'args = sys.argv[1:]\n'
                        'target = pathlib.Path(args[args.index("--output") + 1])\n'
                        'with open(os.environ["MOCK_DOWNLOADS"], "a") as log: log.write(args[-1] + "\\n")\n'
                        'url = args[-1]\n'
                        'if "revision/main" in url:\n'
                        '    target.write_text(os.environ["MOCK_METADATA"])\n'
                        'else:\n'
                        '    target.write_bytes({"config.json": b"config", "ime_vocab.json": b"vocab",\n'
                        '                        "model.safetensors": b"weights"}[url.rsplit("/", 1)[1]])\n')
        curl.chmod(0o700)
        fetch_env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ['PATH'],
                         MOCK_DOWNLOADS=str(download_log), MOCK_METADATA=json.dumps(metadata))
        fetch = [cli, 'fetch-model', '--output-dir', str(asset_root)]
        subprocess.run(fetch, check=True, capture_output=True, env=fetch_env)
        assert download_log.read_text().count('model.safetensors') == 1
        second_fetch = subprocess.run(fetch, check=True, capture_output=True, text=True, env=fetch_env)
        assert download_log.read_text().count('model.safetensors') == 1
        assert 'revision=' + revision in second_fetch.stdout
        (asset_root / revision / 'ime_vocab.json').unlink()
        subprocess.run(fetch, check=True, capture_output=True, env=fetch_env)
        assert download_log.read_text().count('ime_vocab.json') == 2
        assert download_log.read_text().count('model.safetensors') == 1

        newer = root / ('b' * 40)
        newer.mkdir()
        for name in ('config.json', 'ime_vocab.json', 'model.safetensors'):
            (newer / name).write_bytes((model / name).read_bytes())
        selected.write_text(json.dumps({'selected':['cccccccccccccccccccccccccccccccc'],
                                        'reviewed':['cccccccccccccccccccccccccccccccc']}))
        resumed = command.copy()
        resumed[resumed.index('--model-dir') + 1] = str(newer)
        resumed[resumed.index('--revision') + 1] = 'b' * 40
        resumed[resumed.index('--output-dir') + 1] = str(root / 'second')
        subprocess.run(resumed, check=True, capture_output=True, text=True, env=trainer_env)
        args = json.loads(Path(str(trainer) + '.args').read_text())
        assert args[args.index('--model') + 1] == str(model)
        assert args[args.index('--resume-adapter') + 1] == str(output / 'adapter')
        with sqlite3.connect(db) as connection:
            assert connection.execute('SELECT base_revision,record_count,cumulative_record_count,parent_id '
                                      'FROM lora_runs ORDER BY id DESC LIMIT 1').fetchone() == (revision,1,2,1)

        # Exercise the real CLI installer without downloading the 166 MB
        # release: curl is the only mocked boundary, checksum/tar/API are real.
        archive = root / 'release.tar.gz'
        payload = b'#!/bin/sh\nprintf \'{"trainerApi":2}\\n\'\n'
        with tarfile.open(archive, 'w:gz') as tar:
            member = tarfile.TarInfo('./llavon-lora')
            member.mode = 0o755
            member.size = len(payload)
            tar.addfile(member, io.BytesIO(payload))
            library_payload = b'native library fixture'
            library = tarfile.TarInfo('./libtorch_cpu.so')
            library.mode = 0o755
            library.size = len(library_payload)
            tar.addfile(library, io.BytesIO(library_payload))
        version = '2026.09.24.7'
        target = 'osx-arm64-cpu' if sys.platform == 'darwin' else 'linux-x64-cpu'
        asset = f'llavon-lora-{version}-{target}.tar.gz'
        manifest = root / 'latest.json'
        manifest.write_text(json.dumps({'schema': 1, 'trainerApi': 1, 'version': version, 'commit': pinned_commit,
            'assets': {target: {'name': asset,
                'url': 'https://github.com/llavon-ime/lora-trainer/releases/download/v'+version+'/'+asset,
                'size': archive.stat().st_size,
                'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()}}}))
        fake_bin = root / 'bin'
        fake_bin.mkdir()
        curl = fake_bin / 'curl'
        curl.write_text('#!/bin/sh\nfor arg do\n  if [ "$previous" = --output ]; then output="$arg"; fi\n  previous="$arg"\ndone\necho "$output" >> "$MOCK_CURL_LOG"\ncase "$arg" in *releases/download/v*/latest.json) cp "$MOCK_PINNED_MANIFEST" "$output";; *latest.json?*) cp "$MOCK_MANIFEST" "$output";; *) cp "$MOCK_ARCHIVE" "$output";; esac\n')
        curl.chmod(0o700)
        env = dict(os.environ, PATH=str(fake_bin)+os.pathsep+os.environ['PATH'],
                   MOCK_MANIFEST=str(manifest), MOCK_PINNED_MANIFEST=str(manifest), MOCK_ARCHIVE=str(archive),
                   MOCK_CURL_LOG=str(root / 'curl.log'), LLAVON_IME_LORA_TRAINER_CACHE=str(root / 'cache'),
                   LLAVON_IME_LORA_RELEASE_ATTEMPTS='1')
        installed = root / 'installed'
        install_command = [cli, 'install-trainer', '--output-dir', str(installed)]
        subprocess.run(install_command, env=env, capture_output=True, check=True)
        assert (installed / 'llavon-lora').is_file()
        assert (installed / 'libtorch_cpu.so').is_file()
        stamp = json.loads((installed / 'trainer-release.json').read_text())
        assert stamp['commit'] == pinned_commit
        assert stamp['sha256'] == hashlib.sha256((installed / 'llavon-lora').read_bytes()).hexdigest()
        def archive_fetches():
            return sum(1 for line in Path(env['MOCK_CURL_LOG']).read_text().splitlines() if 'tar.gz' in line)
        assert archive_fetches() == 1
        # A matching installation is left alone: no manifest, no archive.
        repeat = subprocess.run(install_command, env=env, capture_output=True, text=True)
        assert repeat.returncode == 0 and 'already-installed=true' in repeat.stdout
        assert archive_fetches() == 1
        # Another target reuses the verified cached archive instead of fetching
        # the release again.
        installed2 = root / 'installed2'
        second = subprocess.run([cli, 'install-trainer', '--output-dir', str(installed2)],
                                env=env, capture_output=True, text=True)
        assert second.returncode == 0 and archive_fetches() == 1
        assert (installed2 / 'libtorch_cpu.so').is_file()
        (installed / 'llavon-lora').unlink()
        contents = json.loads(manifest.read_text())
        contents['commit'] = '0' * 40
        manifest.write_text(json.dumps(contents))
        mismatch = subprocess.run(install_command, env=env, capture_output=True, text=True)
        assert mismatch.returncode != 0 and 'pinned submodule commit' in mismatch.stderr
        assert not (installed / 'llavon-lora').exists()
        contents['commit'] = pinned_commit
        manifest.write_text(json.dumps(contents))
        immutable = root / 'immutable.json'
        immutable.write_text(json.dumps(dict(contents, commit='0' * 40)))
        env['MOCK_PINNED_MANIFEST'] = str(immutable)
        mismatch = subprocess.run(install_command, env=env, capture_output=True, text=True)
        assert mismatch.returncode != 0 and 'immutable trainer release' in mismatch.stderr
        assert not (installed / 'llavon-lora').exists()
        env['MOCK_PINNED_MANIFEST'] = str(manifest)
        contents['assets'][target]['sha256'] = '0' * 64
        manifest.write_text(json.dumps(contents))
        assert subprocess.run(install_command, env=env, capture_output=True).returncode != 0
        assert not (installed / 'llavon-lora').exists()
        # An archive that only carries the executable must be rejected: without
        # the native libraries TorchSharp fails at startup.
        binary_only = root / 'binary-only.tar.gz'
        with tarfile.open(binary_only, 'w:gz') as tar:
            member = tarfile.TarInfo('./llavon-lora')
            member.mode = 0o755
            member.size = len(payload)
            tar.addfile(member, io.BytesIO(payload))
        contents['assets'][target]['sha256'] = hashlib.sha256(binary_only.read_bytes()).hexdigest()
        contents['assets'][target]['size'] = binary_only.stat().st_size
        manifest.write_text(json.dumps(contents))
        env['MOCK_ARCHIVE'] = str(binary_only)
        rejected = subprocess.run(install_command, env=env, capture_output=True, text=True)
        assert rejected.returncode != 0 and 'native libraries' in rejected.stderr
        assert not (installed / 'llavon-lora').exists()


if __name__ == '__main__':
    main(*sys.argv[1:])
