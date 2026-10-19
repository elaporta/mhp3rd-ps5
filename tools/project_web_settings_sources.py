#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Combine the current native settings adapters with the byte-checked r6 transport."""
import argparse
import hashlib
import json
import pathlib
import project_game_sources as base
import project_online_sources as online

ROOT = pathlib.Path(__file__).resolve().parents[1]


def project(output):
    base.project(output, performance=True)
    manifest = json.loads((output/'projection.json').read_text())
    previous = json.loads((ROOT/'tools/source_projection.lock.json').read_text())
    for name, record in manifest['sources'].items():
        path = output/name
        text = online.adapt(name,path.read_text(),base)
        if name.endswith('/adhoc/client.cpp'):
            for revision in (4, 5):
                (output/f'r{revision}_client.cpp').write_text(online.regression_client(text, revision, base))
        if '/adhoc/' in name or name.endswith('/hle/hle_adhoc.cpp'):
            if hashlib.sha256(text.encode()).hexdigest() != previous['sources'][name]['output_sha256']:
                raise ValueError('Working r6 transport differs: '+name)
        if name.endswith('/install/user_data.cpp'):
            text = '#include "game_settings.hpp"\n'+text
            text = base.replace(text,'for (const auto &[key, value] : entries) out <<',
                'for (const auto &[key, value] : yakumo::game_settings::retain_pending(entries)) out <<')
        if name.endswith('/kernel/fast_forward.cpp'):
            text = '#include "game_web.hpp"\n'+text
            text = base.replace(text,'yakumo::text_input::service().blocks_input()',
                '(yakumo::text_input::service().blocks_input() || yakumo::game_web::blocks_input())')
        encoded = text.encode()
        if path.read_bytes() != encoded:
            path.write_bytes(encoded)
        record['output_sha256'] = hashlib.sha256(encoded).hexdigest()
        if record['output_sha256'] != previous['sources'][name]['output_sha256']:
            raise ValueError('Accepted source projection differs: '+name)
    for name,key in (('r4_client.cpp','r4_regression_client_sha256'),('r5_client.cpp','r5_regression_client_sha256'),
                     ('adhoc_ppsspp_peer_tests.cpp','independent_peer_fixture_sha256')):
        if name == 'adhoc_ppsspp_peer_tests.cpp':
            (output/name).write_text(online.peer_fixture(base))
        content=(output/name).read_bytes()
        if hashlib.sha256(content).hexdigest()!=previous[key]:
            raise ValueError('Accepted regression fixture differs: '+name)
    entry = (ROOT/'src/game_first_frame.cpp').read_text()
    entry = '#include "game_web.hpp"\n#include "game_network.hpp"\n#include "game_settings.hpp"\n'+entry
    entry = base.replace(entry,'    heartbeat=platform::milliseconds();\n    static std::uint64_t next_text_tick{};',
        '    heartbeat=platform::milliseconds();\n    yakumo::game_network::tick(heartbeat.load());\n    yakumo::game_web::tick(heartbeat.load());\n    static std::uint64_t next_text_tick{};')
    entry = base.replace(entry,'        auto& settings=mhp3rd::settings::current();',
        '        auto& settings=mhp3rd::settings::current();\n        yakumo::game_settings::apply_defaults();\n        yakumo::game_network::initialize();')
    entry = base.replace(entry,'        mhp3rd::ensure_renderer();',
        '        mhp3rd::ensure_renderer();\n        yakumo::game_web::initialize();')
    entry = base.replace(entry,'    cleanup_started=platform::milliseconds();heartbeat=cleanup_started.load();',
        '    cleanup_started=platform::milliseconds();heartbeat=cleanup_started.load();\n    yakumo::game_web::stop();\n    yakumo::game_network::shutdown();\n    heartbeat=platform::milliseconds();')
    if hashlib.sha256(entry.encode()).hexdigest() != previous['online_entry_sha256']:
        raise ValueError('Accepted native entry projection differs')
    target = output/'online_entry.cpp'
    if not target.exists() or target.read_text() != entry:
        target.write_text(entry)
    # Recreate fault-injection oracles from maintained source instead of missing source freezes.
    resolver = (ROOT/'src/adhoc_resolver.cpp').read_text()
    resolver = base.replace(resolver, '&result.address, kTimeoutMicroseconds, kRetries, 0)', '&result.address, 2, kRetries, 0)')
    (output/'r1_resolver_regression.cpp').write_text(resolver)
    socket = (ROOT/'src/adhoc_socket.cpp').read_text()
    socket = base.replace(socket, 'kAdd=1,kModify=2,kDelete=3', 'kAdd=1,kModify=3,kDelete=2')
    socket = base.replace(socket, 'if (errno==ETIMEDOUT)', 'if (false && errno==ETIMEDOUT)')
    socket = base.replace(socket, '    char line[256];', '    return; // Negative fixture: historical synchronous/error-sink regression.\n    char line[256];')
    (output/'r3_socket_regression.cpp').write_text(socket)
    for key in ('r4_regression_client_sha256','r5_regression_client_sha256','independent_peer_fixture_sha256'):
        manifest[key] = previous[key]
    manifest.update(online=True,web_settings=True,baseline='game-online-0.7.0-r6',
                    online_entry_sha256=hashlib.sha256(entry.encode()).hexdigest())
    (output/'projection.json').write_text(json.dumps(manifest,indent=2)+'\n')


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    parser.add_argument('--performance',action='store_true')
    args=parser.parse_args()
    if not args.output.resolve().is_relative_to(ROOT/'out/data'):
        parser.error('Web projection must remain under ignored out/data')
    project(args.output)
