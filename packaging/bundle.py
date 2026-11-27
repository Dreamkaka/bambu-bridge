"""Package the bridge and matched Open Bamboo native libraries (no wheel install)."""
import argparse
import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile
from urllib.request import urlopen
from zipfile import ZipFile

RELEASE = '0.2.65'
SOURCE = 'https://github.com/persano/open-bamboo-networking'
REVISION = 'b3f388e81704df5b65616ca4a8d9f1221d203272'
TARGETS = {
    'linux_x64': ('linux', '2dcde53d21dc1ba9115c6a8332485fe75b72897355453d3ab464cdfe428923aa',
                  ('libbambu_networking.so', 'libBambuSource.so', 'liblive555.so')),
    'win_x64': ('win', 'a03fe9637be01accc9ec3f5c9db85eef7c156cf7a1168e7acfab1a7cc48fee12',
                ('bambu_networking.dll', 'BambuSource.dll', 'live555.dll')),
}
HERE = Path(__file__).resolve().parent


def bundle(target, executable, output, notices, cached_wheel=None):
    platform, digest, libraries = TARGETS[target]
    url = ('https://github.com/persano/open-bambu-networking-plugin/releases/download/'
           f'v{RELEASE}/open_bambu_networking_{platform}_x86_64.whl')
    output = output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        wheel = cached_wheel
        if wheel is None:
            wheel = root / 'plugin.whl'
            with urlopen(url, timeout=60) as response, wheel.open('wb') as dest:
                shutil.copyfileobj(response, dest)
        with wheel.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != digest:
                raise SystemExit(f'Open Bamboo v{RELEASE} {target}: SHA-256 mismatch')
        stage = root / 'package'
        plugins = stage / 'plugins'
        plugins.mkdir(parents=True)
        with ZipFile(wheel) as archive:
            for prefix, files in (
                (f'open_bambu_networking/bin/{target}', libraries),
                (f'open_bambu_networking-{RELEASE}.dist-info', ('NOTICE', 'LICENSE.AGPL')),
            ):
                for name in files:
                    with archive.open(f'{prefix}/{name}') as src, (plugins / name).open('wb') as dest:
                        shutil.copyfileobj(src, dest)
        (plugins / 'SOURCE.txt').write_text(
            f'Open Bamboo Networking binaries: {url}\n'
            f'Corresponding source tree: {SOURCE}/tree/{REVISION}\n'
            f'Corresponding source archive: {SOURCE}/archive/{REVISION}.tar.gz\n',
            encoding='utf-8',
        )
        shutil.copyfile(HERE / 'obn.conf', plugins / 'obn.conf')
        exe = stage / ('bambu-bridge.exe' if platform == 'win' else 'bambu-bridge')
        shutil.copy2(executable, exe)
        if platform == 'win':
            shutil.copyfile(HERE.parent / 'run.bat', stage / 'run.bat')
        else:
            shutil.copyfile(HERE / 'run.sh', stage / 'run.sh')
        for notice in notices:
            shutil.copyfile(notice, stage / notice.name)
        if platform == 'win':
            print(shutil.make_archive(str(output), 'zip', root_dir=stage))
        else:
            def executable_mode(member):
                if member.name in ('./bambu-bridge', './run.sh'):
                    member.mode = 0o755
                return member

            archive_path = f'{output}.tar.gz'
            with tarfile.open(archive_path, 'w:gz') as archive:
                archive.add(stage, arcname='.', filter=executable_mode)
            print(archive_path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('platform', choices=TARGETS)
    parser.add_argument('executable', type=Path)
    parser.add_argument('output', type=Path, help='archive path without extension')
    parser.add_argument('notices', type=Path, nargs='*')
    parser.add_argument('--wheel', type=Path, help='use an existing copy of the pinned wheel')
    args = parser.parse_args()
    bundle(args.platform, args.executable, args.output, args.notices, args.wheel)
