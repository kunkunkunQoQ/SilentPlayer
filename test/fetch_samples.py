"""Download small public sample files used by the format regression tests.

Saves into ./samples (next to this script). Safe to re-run: existing files are
kept. Only public sample sites are used; nothing is bundled with the player.

    python test/fetch_samples.py
"""
import os
import sys
import urllib.request

TARGET_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'samples')

CANDIDATES = [
    # (filename, [urls to try in order])
    ('t.mp3', [
        'https://download.samplelib.com/mp3/sample-6s.mp3',
    ]),
    ('t.wav', [
        'https://download.samplelib.com/wav/sample-3s.wav',
    ]),
    ('t.mp4', [
        'https://download.samplelib.com/mp4/sample-5s.mp4',
    ]),
    ('t.m4a', [
        'https://filesamples.com/samples/audio/m4a/sample1.m4a',
    ]),
    ('t.flac', [
        'https://filesamples.com/samples/audio/flac/sample1.flac',
    ]),
    ('t.aac', [
        'https://filesamples.com/samples/audio/aac/sample1.aac',
    ]),
    ('t.wma', [
        'https://filesamples.com/samples/audio/wma/sample1.wma',
    ]),
]

HDR = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'}


def main():
    os.makedirs(TARGET_DIR, exist_ok=True)
    failed = []
    for name, urls in CANDIDATES:
        out = os.path.join(TARGET_DIR, name)
        if os.path.exists(out) and os.path.getsize(out) > 0:
            print(f'{name}: already present ({os.path.getsize(out)} bytes)')
            continue
        ok = False
        for url in urls:
            try:
                req = urllib.request.Request(url, headers=HDR)
                data = urllib.request.urlopen(req, timeout=25).read()
                if len(data) < 1000:
                    print(f'{name}: {url} -> too small ({len(data)}), skip')
                    continue
                with open(out, 'wb') as f:
                    f.write(data)
                print(f'{name}: OK {len(data)} bytes  <- {url}')
                ok = True
                break
            except Exception as e:  # noqa: BLE001
                print(f'{name}: FAIL {url} -> {type(e).__name__}: {e}')
        if not ok:
            failed.append(name)

    if failed:
        print('FAILED: ' + ', '.join(failed))
        return 1

    make_derived()
    return 0


def _copy(src, dst):
    s = os.path.join(TARGET_DIR, src)
    d = os.path.join(TARGET_DIR, dst)
    if not os.path.exists(s):
        return
    with open(s, 'rb') as f:
        data = f.read()
    with open(d, 'wb') as f:
        f.write(data)
    print(f'{dst}: derived from {src} ({len(data)} bytes)')


def make_derived():
    """错误扩展名 / 损坏文件样本：用于验证「按内容识别，不信任扩展名」。"""
    # 真实内容 MP4，文件名 .mp3
    _copy('t.mp4', 'mp4content.mp3')
    # 真实内容 MP3，文件名 .mp4
    _copy('t.mp3', 'mp3content.mp4')
    # 内容与扩展名一致的对照样本
    _copy('t.mp3', 'normal.mp3')
    _copy('t.mp4', 'normal.mp4')

    # 损坏文件：随机字节，伪装成 .mp3
    broken = os.path.join(TARGET_DIR, 'broken.mp3')
    if not os.path.exists(broken):
        with open(broken, 'wb') as f:
            f.write(os.urandom(60000))
        print('broken.mp3: 60000 random bytes')

    # 截断文件：只保留 MP4 头，伪装成 .mp3
    trunc = os.path.join(TARGET_DIR, 'truncated.mp3')
    if not os.path.exists(trunc):
        src = os.path.join(TARGET_DIR, 't.mp4')
        if os.path.exists(src):
            with open(src, 'rb') as f:
                head = f.read(400)
            with open(trunc, 'wb') as f:
                f.write(head)
            print('truncated.mp3: first 400 bytes of t.mp4')


if __name__ == '__main__':
    sys.exit(main())
