#!/usr/bin/env python3
"""Opt-in real model editor, history and export acceptance; no model or credential fixtures."""
import argparse
import hashlib
import json
import locale
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('application', type=Path)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--mode', required=True, choices=(
        'ai-subject', 'ai-semantic', 'ai-face', 'ai-completion', 'ai-raw-foundation', 'auto-start', 'subject-emphasis'))
    parser.add_argument('--output-base', type=Path, required=True)
    parser.add_argument('--clean-runtime', action='store_true')
    parser.add_argument('--runtime-url', help='Explicit local diagnostic Runtime; default is installed discovery')
    parser.add_argument('--credential-file', type=Path, help='Private credential file for that diagnostic Runtime')
    args = parser.parse_args()
    application, image = args.application.resolve(), args.image.resolve()
    args.output_base.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix=args.mode + '-', dir=args.output_base))
    daily = root / 'unrelated-library'
    daily.mkdir()
    sentinel = daily / 'sentinel'
    sentinel.write_bytes(b'AI acceptance must not open this library')
    before = digest(image)
    request, result, output = root / 'request.json', root / 'result.json', root / 'edited.png'
    request.write_text(json.dumps(dict(schema='shadow-pipeline-edit-20260814.1',
        requestId=args.mode, input=str(image), output=str(output),
        export={'format': 'png', 'maxEdge': 1200})), encoding='utf8')
    env = {key: value for key, value in os.environ.items() if not key.startswith('SHADOW_')}
    env.update(SHADOW_DESKTOP_DATA_ROOT=str(daily), SHADOW_PIPELINE_SMOKE_ACTION=args.mode,
               SHADOW_PIPELINE_SMOKE_OUTPUT=str(root), QT_QPA_PLATFORM='offscreen',
               QT_QUICK_BACKEND='software', QT_FORCE_STDERR_LOGGING='1')
    if args.runtime_url or args.credential_file:
        if not (args.runtime_url and args.credential_file):
            parser.error('diagnostic Runtime requires both --runtime-url and --credential-file')
        env['SHADOW_INFER_BASE_URL'] = args.runtime_url
        env['SHADOW_INFER_CREDENTIAL_FILE'] = str(args.credential_file.resolve(strict=True))
    if args.mode == 'auto-start':
        env['SHADOW_AUTO_START_REQUIRE_AI'] = '1'
        env['SHADOW_AUTO_START_SMOKE_UI'] = '1'
        env['SHADOW_AUTO_START_SMOKE_DARK_BASELINE'] = '1'
        evidence = root / 'auto-ui'
        evidence.mkdir()
        env['SHADOW_AUTO_START_EVIDENCE_DIR'] = str(evidence)
    if args.mode == 'subject-emphasis':
        env['SHADOW_PIPELINE_SMOKE_ACTION'] = ''
        env['SHADOW_SUBJECT_EMPHASIS_SMOKE_QUERY'] = 'person'
        env['SHADOW_SUBJECT_EMPHASIS_SMOKE_OUTPUT'] = str(root)
    if os.name == 'nt':
        env['QT_QPA_FONTDIR'] = str(Path(os.environ['SystemRoot']) / 'Fonts')
        if args.clean_runtime:
            env['PATH'] = str(application.parent) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
            for key in ('QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
                env.pop(key, None)
    arguments = ([str(application), '--isolate', str(image)] if args.mode == 'auto-start' else
                 [str(application), '--pipeline-edit', '--request', str(request), '--result', str(result)])
    try:
        with (root / 'application.log').open('wb') as log:
            process = subprocess.run(arguments, cwd=application.parent, env=env, stdout=log,
                stderr=subprocess.STDOUT, timeout=1840,
                **({'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}))
    finally:
        assert digest(image) == before, 'the source image changed'
        assert sentinel.read_bytes() == b'AI acceptance must not open this library'
        assert list(daily.iterdir()) == [sentinel], 'independent editing touched another library'
    diagnostic = (root / 'application.log').read_text(
        encoding=locale.getencoding() if os.name == 'nt' else 'utf8', errors='replace')
    assert process.returncode == 0, (process.returncode, diagnostic[-6000:], str(root))
    assert not any(message in diagnostic for message in
                   ('TypeError', 'ReferenceError', 'Binding loop')), diagnostic[-6000:]
    if args.mode == 'auto-start':
        assert 'Auto start UI pointer acceptance passed' in diagnostic, diagnostic[-6000:]
        assert 'Auto start export completed' in diagnostic, diagnostic[-6000:]
        outputs = list(root.glob('*-edited.jpg'))
        assert len(outputs) == 1 and outputs[0].stat().st_size > 500
    else:
        receipt = json.loads(result.read_text(encoding='utf8'))
        assert receipt['requestId'] == args.mode and receipt['outcome'] == 'completed', receipt
        if args.mode == 'subject-emphasis':
            assert 'Subject emphasis acceptance passed: cancel, local Qwen, grounding/SAM,' in diagnostic, diagnostic[-6000:]
        else:
            assert 'AI editor ' + args.mode + ' apply/undo/redo passed' in diagnostic, diagnostic[-6000:]
            assert 'AI editor ' + args.mode + ' export passed' in diagnostic, diagnostic[-6000:]
        with output.open('rb') as stream:
            header = stream.read(33)
        assert header[:8] == b'\x89PNG\r\n\x1a\n' and header[12:16] == b'IHDR'
        width, height = struct.unpack('>II', header[16:24])
        assert 0 < max(width, height) <= 1200 and header[24] >= 8
    print('Real AI editor acceptance passed:', args.mode, '; source unchanged; evidence:', root)


if __name__ == '__main__':
    main()
