#!/usr/bin/env python3
"""Bind absent optional Mesa dispatch entries to ELF's unresolved-weak value, zero."""
import pathlib
import re
import sys

OPTIONAL = re.compile(r"(?:annotate|ctx_roll|radv|rmv|rra|sqtt|threaded|utrace|vk|wsi)_[A-Za-z0-9_]+")
IME_IMPORTS = {'sceImeDialogInit', 'sceImeDialogGetStatus', 'sceImeDialogGetResult', 'sceImeDialogTerm', 'sceImeDialogAbort'}


def linker_script(nm_output):
    names = set()
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] in ("w", "v"):
            name = fields[1]
            if fields[0] == 'w' and name in IME_IMPORTS:
                continue  # Preserve reviewed nullable system imports; never bind them permanently to zero.
            if not OPTIONAL.fullmatch(name):
                raise ValueError(f"Unexpected optional weak import; review before binding: {name}")
            names.add(name)
    return "/* Absent optional Mesa dispatch entries, after the first complete link. */\n" + "".join(
        f"PROVIDE_HIDDEN({name} = 0);\n" for name in sorted(names))


def validate_imports(nm_output, optional_ime=False, global_ime=False):
    weak = set()
    global_names = set()
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        kind, name = fields
        if kind in ('w', 'v'):
            if not optional_ime or kind != 'w' or name not in IME_IMPORTS:
                raise ValueError('Unexpected retained weak import: '+name)
            weak.add(name)
        if kind == 'U' and name in IME_IMPORTS:
            global_names.add(name)
        if kind == 'U' and (name in {'realpath','lstat','access','exit','_exit','_Exit'} or (name in IME_IMPORTS and not global_ime)):
            raise ValueError('Forbidden mandatory import: '+name)
    if optional_ime and weak != IME_IMPORTS:
        raise ValueError('Complete nullable IME capability must survive linking')
    if global_ime and global_names != IME_IMPORTS:
        raise ValueError('Complete global IME capability must survive linking')


if __name__ == "__main__":
    if sys.argv[1] == '--validate':
        validate_imports(pathlib.Path(sys.argv[2]).read_text(), '--optional-ime' in sys.argv[3:], '--global-ime' in sys.argv[3:])
    else:
        pathlib.Path(sys.argv[2]).write_text(linker_script(pathlib.Path(sys.argv[1]).read_text()))
