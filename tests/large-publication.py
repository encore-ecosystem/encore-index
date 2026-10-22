#!/usr/bin/env python3
"""Exercise multi-chunk publication with an actual bundled-library archive."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.request
from publication import BASE, request


def main():
    binary = str(Path(sys.argv[1]).resolve())
    archive = Path(sys.argv[2]).read_bytes()
    assert 65536 < len(archive) <= 4194304
    with tempfile.TemporaryDirectory(prefix='registry-large-publication-') as temporary:
        env = {**os.environ, 'ENCORE_INDEX_DATA': temporary, 'ENCORE_INDEX_PUBLIC_URL': BASE}
        for args in (('init',), ('user-add', 'alice')):
            subprocess.run([binary, *args], env=env, check=True, capture_output=True)
        token = subprocess.check_output([binary, 'token-create', 'alice'], env=env, text=True).strip()
        server = subprocess.Popen([binary, 'serve'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                assert server.poll() is None, server.communicate()
                try:
                    request('/healthz')
                    break
                except OSError:
                    time.sleep(.03)
            upload = json.loads(request('/api/v1/uploads', 'POST', b'', token, 201))['upload']
            for offset in range(0, len(archive), 65536):
                request(f'/api/v1/uploads/{upload}/{offset}', 'PUT', archive[offset:offset + 65536], token)
            result = request(f'/api/v1/uploads/{upload}/finish', 'POST',
                             json.dumps({'checksum': hashlib.sha256(archive).hexdigest(), 'nametag': 'neumann'}).encode(), token, 201)
            assert json.loads(result)['published']
            metadata = json.loads(request('/index/sq/sqlite.json'))
            with urllib.request.urlopen(metadata['versions'][0]['archive'], timeout=15) as response:
                assert response.read() == archive, 'binary archive download must preserve every byte'
            if len(sys.argv) > 4:
                compiler, project = (str(Path(path).resolve()) for path in sys.argv[3:5])
                publish_env = {**env, 'ENCORE_INDEX_URL': BASE + '/index',
                               'ENCORE_REGISTRY_TOKEN': token,
                               'ENCORE_REGISTRY_TOKEN_INDEX': BASE + '/index'}
                published = subprocess.run([compiler, 'publish'], cwd=project, env=publish_env,
                                           capture_output=True, text=True, timeout=120)
                assert published.returncode == 0, (published.stdout + published.stderr).replace(token, '[redacted]')
        finally:
            server.terminate()
            server.wait(timeout=10)
    print('Multi-chunk bundled archive publication passed:', len(archive), 'bytes')


if __name__ == '__main__':
    main()
