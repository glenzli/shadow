#!/usr/bin/env python3
"""Packaged pointer, self-contained reopen, branch, and publication contracts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def png(path):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    pixels = b''.join(
        b'\0' + b''.join(bytes((x + 32, y + 40, 64)) for x in range(96)) for y in range(64))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 96, 64, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b''))


def inspect(path):
    with zipfile.ZipFile(path) as archive:
        manifest = json.loads(archive.read('manifest.json'))
        assert manifest['format'] == 'dev.shadow.document' and manifest['version'] == 1
        assert set(archive.namelist()) == {'manifest.json'} | {v['path'] for v in manifest['files']}
        for entry in manifest['files']:
            data = archive.read(entry['path'])
            assert len(data) == entry['bytes'] and hashlib.sha256(data).hexdigest() == entry['sha256']
        source = manifest['sources'][0]
        return manifest, archive.read(source['render']), json.loads(archive.read(source['history']))


def main():
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='shadow-document-integration-') as directory:
        root = Path(directory)
        daily = root / 'daily-library'
        daily.mkdir()
        sentinel = daily / 'sentinel'
        sentinel.write_bytes(b'unrelated library and settings')
        source = root / 'original.png'
        png(source)
        original = source.read_bytes()
        if os.name != "nt":
            source.chmod(0o444)
        env = os.environ.copy()
        env.update(SHADOW_DESKTOP_DATA_ROOT=str(daily), SHADOW_PIPELINE_SMOKE_ACTION='document',
                   SHADOW_AUTO_START_SMOKE_MEASURED_ONLY='1')
        if sys.platform != 'darwin':
            env.setdefault('QT_QPA_PLATFORM', 'offscreen')

        def run(name, input_path, mode='roundtrip', expected=0, expected_hash=None):
            output = root / (name + '.shdw')
            request, result = root / (name + '-request.json'), root / (name + '-result.json')
            request.write_text(json.dumps({'schema': 'shadow.document-edit/1', 'requestId': name,
                'input': str(input_path), 'inputSha256': expected_hash or digest(input_path), 'output': str(output)}))
            process_env = dict(env, SHADOW_DOCUMENT_SMOKE_MODE=mode)
            if mode == 'cancel':
                process_env['SHADOW_PIPELINE_SMOKE_ACTION'] = 'cancel'
            proc = subprocess.run([executable, '--pipeline-edit', '--request', str(request), '--result', str(result)],
                                  env=process_env, capture_output=True, text=True, timeout=120)
            assert proc.returncode == expected, (name, proc.returncode, proc.stderr[-8000:], result.read_text() if result.exists() else 'no result')
            receipt = json.loads(result.read_text())
            assert receipt['schema'] == 'shadow.document-result/1' and receipt['requestId'] == name
            if expected == 0:
                assert receipt['outcome'] == 'completed', receipt
                assert receipt['artifact']['sha256'] == digest(output)
                assert receipt['artifact']['byteLength'] == str(output.stat().st_size)
                assert 'Document UI return admitted' in proc.stderr
            else:
                assert 'artifact' not in receipt and not output.exists(), receipt
            assert sentinel.read_bytes() == b'unrelated library and settings'
            print('PASS', name, flush=True)
            return output, receipt

        first, _ = run('first', source, 'create')
        first_manifest, first_render, history = inspect(first)
        assert first_render[:8] == b'\x89PNG\r\n\x1a\n' and first_render[24:26] == b'\x08\x02'
        assert len(history['steps']) == 3 and len(history['redo']) == 1, history
        with zipfile.ZipFile(first) as archive:
            assert archive.read(first_manifest['sources'][0]['path']) == original
            assert not any('credentials' in path or 'ui.ini' in path for path in archive.namelist())
        source.unlink()
        roundtrip, _ = run('roundtrip', first)
        second_manifest, second_render, _ = inspect(roundtrip)
        assert first_render == second_render, 'same Recipe changed after self-contained reopen'
        assert first_manifest['document_id'] == second_manifest['document_id']
        assert first_manifest['revision'] != second_manifest['revision']
        metadata, _ = run('metadata', roundtrip, 'metadata')
        with zipfile.ZipFile(metadata) as archive:
            catalog = root / 'metadata-catalog.sqlite'
            catalog.write_bytes(archive.read('catalog.sqlite'))
        with sqlite3.connect(catalog) as catalog_db:
            assert catalog_db.execute('SELECT name FROM photo_variants').fetchone()[0] == 'Portable variant'
        _, metadata_render, _ = inspect(metadata)
        assert metadata_render == second_render, 'renaming a Variant changed its pixels'
        resumed, _ = run('resumed', roundtrip, 'resume')
        _, changed_render, history = inspect(resumed)
        assert changed_render != first_render and len(history['steps']) == 4
        assert history['steps'][-1]['parent'] == 2, 'restored branch was not preserved'
        run('cancelled', first, 'cancel', expected=2)
        run('wrong-source-digest', first, expected=1, expected_hash='0' * 64)

        corrupt = root / 'corrupt.shdw'
        with zipfile.ZipFile(first) as source_zip, zipfile.ZipFile(corrupt, 'w') as target:
            changed_path = first_manifest['sources'][0]['path']
            for name in source_zip.namelist():
                data = source_zip.read(name)
                target.writestr(name, data + b'changed' if name == changed_path else data)
        run('corrupt-payload', corrupt, expected=1)
        traversal = root / 'traversal.shdw'
        with zipfile.ZipFile(traversal, 'w') as archive:
            archive.writestr('manifest.json', '{}')
            archive.writestr('../escaped', 'must not be written')
        run('unsafe-entry', traversal, expected=1)
        assert not (root / 'escaped').exists()

        conflict = root / 'conflict.shdw'
        shutil.copy2(first, conflict)
        process_env = dict(env, SHADOW_DOCUMENT_SMOKE_MODE='conflict')
        proc = subprocess.Popen([executable, '--isolate', str(conflict)], env=process_env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        lines = []
        import time
        import selectors
        deadline = time.monotonic() + 120
        selector = selectors.DefaultSelector()
        selector.register(proc.stderr, selectors.EVENT_READ)
        ready = False
        try:
            while time.monotonic() < deadline and proc.poll() is None:
                if not selector.select(timeout=1):
                    continue
                line = os.read(proc.stderr.fileno(), 65536).decode("utf-8", errors="replace")
                lines.append(line)
                if 'DOCUMENT_READY_FOR_CONFLICT' in line:
                    conflict.write_bytes(b'external newer revision')
                    ready = True
                    break
            out, err = proc.communicate(timeout=120)
            assert ready and proc.returncode == 2, ''.join(lines) + err
            assert conflict.read_bytes() == b'external newer revision'
            assert 'Document conflicting save rejected' in err
        finally:
            selector.close()
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        print('PASS concurrent-edit-conflict and isolated Library', flush=True)


if __name__ == '__main__':
    main()
