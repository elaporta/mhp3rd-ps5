#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare the existing final2 permission source/CRT from maintained source and the pinned SDK."""
import argparse
import hashlib
import json
import pathlib
import subprocess

from prepare_textures import reject_links
from project_permission_diagnostics import crt_source, once, repair_source

ROOT = pathlib.Path(__file__).resolve().parents[1]


def sources(policy, sdk_crt):
    policy = repair_source(policy)
    policy = once(policy, '#define BUILD "title-permissions-0.1.2"',
                  '#define BUILD "title-permissions-0.1.3-final2"')
    policy = policy.replace('Keep Yakumo and all external writers closed', 'Keep the app and all external writers closed')
    policy = once(policy,
        '    notify("Yakumo permisos 0.1.2: REPARADOR INICIADO. Comprobando titulo y metadatos.");',
        '    notify("mhp3rd-ps5: Applying title permissions...");')
    policy = once(policy, '    notify("Yakumo permisos 0.1.2: aplicando modos 0777/0666 al titulo validado.");\n', '')
    policy = once(policy,
        '"Yakumo: PERMISOS LISTOS. %zu objetos verificados, %zu cambios. Ya puedes abrir el juego."',
        '"mhp3rd-ps5: Permissions applied (directories 0777; data files 0666; eboot.bin and libc.prx 0777). Launch the game now. %zu objects checked, %zu changes."')
    policy = once(policy,
        '"Yakumo: ERROR %s; errno=%d (%s); euid=%ju; cambios=%zu; %s; last metadata: %s. No abras el juego con esta preparacion."',
        '"mhp3rd-ps5: ERROR %s; errno=%d (%s); euid=%ju; changes=%zu; %s; last metadata: %s. Do not launch the game."')
    begin = sdk_crt.index('static int\npayload_terminate(void) {')
    end = sdk_crt.index('\n}\n', begin) + 3
    crt = sdk_crt[:begin] + '''static int
payload_terminate(void) {
  /* Local Yakumo audit helper: return to the manually invoked ELF loader.
   * Never call POSIX exit or terminate the running game/title. */
  return 0;
}
''' + sdk_crt[end:]
    crt = crt_source(crt)
    crt = once(crt, '#include "permission_bootstrap_status.h"', '#include "permission_bootstrap_status_quiet.h"')
    crt = crt.replace('Yakumo permisos 0.1.2', 'mhp3rd-ps5 permissions 0.1.3')
    if 'Yakumo' in policy or 'PERMISOS LISTOS' in policy or 'PPSA99373' not in policy:
        raise ValueError('Final payload source identity differs')
    return {'title_permissions_final.c': policy.encode(), 'crt_final.c': crt.encode()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'out/data/install-build/payload-source')
    args = parser.parse_args()
    output = args.output.absolute()
    reject_links(output)
    if not output.resolve().is_relative_to(ROOT / 'out/data'):
        parser.error('Payload sources must stay under out/data')
    spec = json.loads((ROOT / 'dependencies.lock.json').read_text())['repositories']['sdk']
    sdk = ROOT / spec['path']
    reject_links(sdk)
    head = subprocess.check_output(['git', '-C', str(sdk), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = subprocess.check_output(['git', '--no-optional-locks', '-C', str(sdk), 'status', '--porcelain'], text=True).strip()
    if head != spec['commit'] or dirty:
        raise ValueError('SDK must be clean and match dependencies.lock.json')
    original = subprocess.check_output(['git', '-C', str(sdk), 'show', spec['commit'] + ':crt/crt.c'])
    reject_links(sdk / 'crt/crt.c')
    if (sdk / 'crt/crt.c').read_bytes() != original:
        raise ValueError('SDK CRT differs')
    result = sources((ROOT / 'src/title_permissions.c').read_bytes().decode(), original.decode())
    for name, data in result.items():
        path = output / name
        reject_links(path)
        if path.exists() and path.read_bytes() != data:
            raise ValueError('Different existing payload source preserved: ' + str(path))
    output.mkdir(parents=True, exist_ok=True)
    for name, data in result.items():
        path = output / name
        if not path.exists():
            with path.open('xb') as stream:
                stream.write(data)
        print(name + ': ' + hashlib.sha256(data).hexdigest())
    print('Source preparation only; no ELF built and no hardware acceptance inferred.')


if __name__ == '__main__':
    main()
