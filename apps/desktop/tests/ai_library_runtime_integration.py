#!/usr/bin/env python3
"""Opt-in packaged QML acceptance against installed real local AI models."""
import argparse
import hashlib
import locale
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('application', type=Path)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--output-base', required=True, type=Path)
    parser.add_argument('--clean-runtime', action='store_true')
    args = parser.parse_args()
    application, image = args.application.resolve(), args.image.resolve()
    if image.suffix.lower() not in ('.jpg', '.jpeg', '.png'):
        raise ValueError('Choose one explicit public portrait JPEG or PNG')
    args.output_base.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='ai-library-', dir=args.output_base))
    inputs = root / 'input'
    inputs.mkdir()
    before = digest(image)
    shutil.copyfile(image, inputs / ('portrait' + image.suffix.lower()))
    env = {key: value for key, value in os.environ.items() if not key.startswith('SHADOW_')}
    env.update(SHADOW_DESKTOP_DATA_ROOT=str(root / 'data'), SHADOW_DESKTOP_SCAN_FOLDER=str(inputs),
               SHADOW_DESKTOP_AI_LIBRARY_SMOKE='1', QT_QPA_PLATFORM='offscreen',
               QT_QUICK_BACKEND='software', QT_FORCE_STDERR_LOGGING='1')
    if os.name == 'nt':
        env['QT_QPA_FONTDIR'] = str(Path(os.environ['SystemRoot']) / 'Fonts')
        if args.clean_runtime:
            env['PATH'] = str(application.parent) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32')
            for key in ('QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH'):
                env.pop(key, None)
    with (root / 'application.log').open('wb') as log:
        process = subprocess.run([str(application)], cwd=application.parent, env=env,
            stdout=log, stderr=subprocess.STDOUT, timeout=880,
            **({'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}))
    diagnostic = (root / 'application.log').read_text(
        encoding=locale.getencoding() if os.name == 'nt' else 'utf8', errors='replace')
    assert digest(image) == before, 'the source portrait changed'
    assert process.returncode == 0, (process.returncode, diagnostic[-6000:], str(root))
    assert not any(message in diagnostic for message in
                   ('TypeError', 'ReferenceError', 'Binding loop')), diagnostic[-6000:]
    for stage in ('people', 'semantic search', 'categories', 'description', 'classification'):
        assert 'AI Library ' + stage + ' passed' in diagnostic, (stage, diagnostic[-6000:])
    assert 'AI Library smoke passed' in diagnostic
    print('Real packaged AI Library acceptance passed; source unchanged; evidence:', root)


if __name__ == '__main__':
    main()
