"""Install the optional, local-only Chinese girl voice outside the application bundle.

Requires uv on PATH. All models, Python downloads, caches and temporary files use
the explicit runtime directory. No API keys, login or cloud TTS account required.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import urllib.request

VITS_REVISION = '43c74f369834294464eca69316048d2f9a7d54f1'
MODEL_REVISION = '23aa81589339ff1ef49568b261ab8a0e642879bc'
MODEL_REPO = 'Misaka20001/Paimon-is-not-a-food'


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()


def download(url, target, expected_size=None, expected_hash=None):
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and (expected_hash is None or digest(target) == expected_hash):
        return
    partial = target.with_suffix(target.suffix + '.part')
    for attempt in range(5):
        offset = partial.stat().st_size if partial.exists() else 0
        request = urllib.request.Request(url, headers={'Range': f'bytes={offset}-'} if offset else {})
        with urllib.request.urlopen(request, timeout=90) as response:
            if offset and response.status != 206:
                raise RuntimeError('Server did not accept download resume')
            with partial.open('ab' if offset else 'wb') as stream:
                for chunk in iter(lambda: response.read(1024 * 1024), b''):
                    stream.write(chunk)
        if expected_size is None or partial.stat().st_size == expected_size:
            break
        if partial.stat().st_size > expected_size:
            raise RuntimeError('Unexpected model size')
    if expected_size is not None and partial.stat().st_size != expected_size:
        raise RuntimeError('Incomplete model download')
    if expected_hash is not None and digest(partial) != expected_hash:
        raise RuntimeError('Model checksum mismatch')
    partial.replace(target)
    print('Verified:', target.name, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, help='Absolute model/runtime directory; choose a drive with at least 3 GB free')
    parser.add_argument('--console-dir', required=True, help='Directory containing RoboMindConsole.exe')
    parser.add_argument('--skip-environment', action='store_true')
    args = parser.parse_args()
    if not pathlib.Path(args.runtime).is_absolute() or not pathlib.Path(args.console_dir).is_absolute():
        parser.error('Use absolute paths')
    root, console = pathlib.Path(args.runtime), pathlib.Path(args.console_dir)
    if not (console / 'RoboMindConsole.exe').is_file():
        parser.error('Console executable not found')
    for folder in ('tmp', 'cache', 'models', 'vits/text'):
        (root / folder).mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(UV_CACHE_DIR=str(root / 'cache/uv'), UV_PYTHON_INSTALL_DIR=str(root / 'python'),
               TEMP=str(root / 'tmp'), TMP=str(root / 'tmp'), UV_HTTP_TIMEOUT='90')
    python = root / 'venv/Scripts/python.exe'
    if not args.skip_environment:
        uv = shutil.which('uv')
        if not uv:
            raise RuntimeError('Install uv first')
        if not python.exists():
            subprocess.run([uv, 'venv', '--python', '3.11', str(root / 'venv')], env=env, check=True)
        subprocess.run([uv, 'pip', 'install', '--python', str(python), 'torch==2.7.1',
                        '--index-url', 'https://download.pytorch.org/whl/cpu'], env=env, check=True)
        subprocess.run([uv, 'pip', 'install', '--python', str(python), 'numpy==1.26.4', 'scipy==1.17.1',
                        'pypinyin==0.55.0', 'jieba==0.42.1', 'cn2an==0.5.23', 'setuptools'], env=env, check=True)
    if not python.exists():
        raise RuntimeError('Local Python environment missing')
    source_files = ['models.py', 'modules.py', 'commons.py', 'attentions.py', 'transforms.py', 'LICENSE', 'text/mandarin.py']
    for name in source_files:
        download(f'https://raw.githubusercontent.com/CjangCjengh/vits/{VITS_REVISION}/{name}', root / 'vits' / name)
    (root / 'vits/monotonic_align.py').write_text('def maximum_path(*args, **kwargs):\n    raise RuntimeError("Inference-only runtime; training unavailable")\n', encoding='utf-8')
    model_url = f'https://huggingface.co/spaces/{MODEL_REPO}/resolve/{MODEL_REVISION}/model/'
    download(model_url + 'config.json?download=true', root / 'models/config.json', 22375,
             '0709db7ac44072041c3db8c810ef863a3d20a3dd11f2aeffbfe79e4d9eeee187')
    download(model_url + 'G_953000.pth?download=true', root / 'models/G_953000.pth', 479276657,
             '0c54396a7a9027952e4d72fceb7e1da1f003d108837138927c2054a33eda0292')
    (root / 'voice.json').write_text(json.dumps({'config': 'models/config.json', 'model': 'models/G_953000.pth',
                                               'speaker_id': 551, 'speaker_name': '派蒙'}, ensure_ascii=False, indent=2), encoding='utf-8')
    worker = root / 'local_vits_worker.py'
    shutil.copyfile(pathlib.Path(__file__).with_name(worker.name), worker)
    (root / 'SOURCE.json').write_text(json.dumps({'model_repository': MODEL_REPO, 'model_revision': MODEL_REVISION,
                                                 'vits_repository': 'CjangCjengh/vits', 'vits_revision': VITS_REVISION,
                                                 'source_hashes': {name: digest(root / 'vits' / name) for name in source_files},
                                                 'usage': 'local personal test; character and voice rights are separate from code licenses'}, indent=2), encoding='utf-8')
    # Publish the pointer only after successful initialization; old settings survive failure.
    probe = subprocess.run([str(python), '-u', str(worker), '--runtime', str(root)], input='', encoding='utf-8',
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, timeout=60)
    result = next((json.loads(line) for line in probe.stdout.splitlines() if line.startswith('{"event": "ready"')), {})
    if probe.returncode or result.get('event') != 'ready':
        raise RuntimeError('Model initialization failed; Console configuration unchanged')
    settings = json.dumps({'python': str(python), 'worker': str(worker), 'runtime': str(root),
                           'dragon_voice': result.get('dragon_voice') is True}, indent=2)
    with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=console, suffix='.tmp', delete=False) as stream:
        stream.write(settings)
        pointer = pathlib.Path(stream.name)
    pointer.replace(console / 'voice-backend.json')
    print('Ready. Restart Console; select Auto or local VITS in LanChat settings.', flush=True)


if __name__ == '__main__':
    main()
