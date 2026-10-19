#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Project diagnostics around the frozen repair policy and pinned SDK CRT."""
import argparse
import hashlib
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_SHA = 'dcfabaac61919431ddff6ea882c8ba7a59db8a482e340705e1d972a96cf1de30'


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Diagnostic anchor is not unique: '+old[:80])
    return text.replace(old, new)


def repair_source(text):
    if hashlib.sha256(text.encode()).hexdigest() != SOURCE_SHA:
        raise ValueError('Frozen permission policy source changed')
    text = once(text, '#define BUILD "title-permissions-0.1.0"', '#define BUILD "title-permissions-0.1.2"')
    text = once(text, '    klog_printf("[%s] %s\\n", BUILD, message);', '')
    text = once(text, '    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);',
                '    const int notify_rc = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);\n'
                '    klog_printf("[%s] notify_rc=%d %s\\n", BUILD, notify_rc, message);')
    text = once(text, '    if (inventory(&list) || identity(&list)) goto failure;',
                '    notify("Yakumo permisos 0.1.2: REPARADOR INICIADO. Comprobando titulo y metadatos.");\n'
                '    if (inventory(&list) || identity(&list)) goto failure;')
    text = once(text, '    if (repair(&list)) goto failure;',
                '    notify("Yakumo permisos 0.1.2: aplicando modos 0777/0666 al titulo validado.");\n'
                '    if (repair(&list)) goto failure;')
    return text


def crt_source(text):
    text = once(text, '#include "syscall.h"',
                '#include "syscall.h"\n#include "permission_bootstrap_status.h"')
    text = once(text, '  payload_args = args;',
                '  payload_args = args;\n'
                '  permission_bootstrap_status(args, "Yakumo permisos 0.1.2: ENTRADA ELF recibida.");')
    for name, label in [('__crt_syscall_init(args)', 'SYSCALL'), ('__kernel_init(args)', 'KERNEL'),
                        ('__klog_init()', 'KLOG'), ('__patch_init()', 'SDK PATCH INIT'),
                        ('__rtld_init()', 'RTLD')]:
        anchor = '  if((error='+name+')) {\n'
        text = once(text, anchor,
                    '  permission_bootstrap_status(args, "Yakumo permisos 0.1.2: inicio '+label+'.");\n'+anchor+
                    '    permission_bootstrap_status(args, "Yakumo permisos 0.1.2: ERROR '+label+' antes del reparador. Consultar klog.");\n')
    text = once(text, '    klog_puts("Unable to resolve the symbol \'__isthreaded\'");',
                '    permission_bootstrap_status(args, "Yakumo permisos 0.1.2: ERROR __isthreaded antes del reparador.");\n'
                '    klog_puts("Unable to resolve the symbol \'__isthreaded\'");')
    text = once(text, '    if((err=payload_run())) {',
                '    permission_bootstrap_status(args, "Yakumo permisos 0.1.2: inicializacion SDK completa; cargando reparador.");\n'
                '    if((err=payload_run())) {\n'
                '      permission_bootstrap_status(args, "Yakumo permisos 0.1.2: ERROR al cargar/cerrar reparador. Consultar klog.");')
    return text


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to((ROOT/'out/data').resolve()):
        raise ValueError('Diagnostics must stay under ignored out/data')
    out.mkdir(parents=True, exist_ok=True)
    # read_bytes keeps the frozen SHA independent of Windows newline conversion.
    original = (ROOT/'src/title_permissions.c').read_bytes().decode()
    write_changed(out/'title_permissions_diagnostic.c', repair_source(original))
    if args.native:
        crt = out/'crt-diagnostic.c'
        subprocess.run([sys.executable, str(ROOT/'tools/prepare_installation_crt.py'), str(crt)], check=True)
        write_changed(crt, crt_source(crt.read_text()))


if __name__ == '__main__':
    main()
