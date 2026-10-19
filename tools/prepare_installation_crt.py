#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project the pinned GPL SDK payload CRT with return-to-loader termination."""
import hashlib
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SDK = ROOT / 'deps/PS5_PayloadSDK'
PIN = 'b83202be73e930050e00de0f1b4d9ec46c0391bf'


def main():
    actual = subprocess.check_output(['git', '-C', str(SDK), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != PIN:
        raise ValueError('SDK revision differs from dependencies.lock.json')
    subprocess.run(['git', '-C', str(SDK), 'diff', '--exit-code', 'HEAD', '--', 'crt'], check=True)
    original = subprocess.check_output(['git', '-C', str(SDK), 'show', PIN+':crt/crt.c'])
    if original != (SDK/'crt/crt.c').read_bytes():
        raise ValueError('Pinned SDK CRT source changed')
    source = original.decode()
    begin = source.index('static int\npayload_terminate(void) {')
    end = source.index('\n}\n', begin)+3
    projected = source[:begin]+'''static int
payload_terminate(void) {
  /* Local Yakumo audit helper: return to the manually invoked ELF loader.
   * Never call POSIX exit or terminate the running game/title. */
  return 0;
}
'''+source[end:]
    target = pathlib.Path(sys.argv[1]).resolve()
    if not target.is_relative_to((ROOT/'out/data').resolve()):
        raise ValueError('CRT projection must stay under ignored out/data')
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text()!=projected:
        target.write_text(projected)
    target.with_suffix('.source.sha256').write_text(hashlib.sha256(original).hexdigest()+'\n')


if __name__ == '__main__':
    main()
