#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build SDK, LLVM C++ runtimes, RADV and native tooling entirely from pinned sources."""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys

from source_support import ROOT, DATA, complete, digest, download, export_git, extract, record, run, save_json

LOCK = json.loads((ROOT / 'dependencies.lock.json').read_text())
# Native source/layout identity; SDK probe revisions are tracked separately below.
_native_lock = {key:value for key,value in LOCK.items() if key != 'build_environment'}
FINGERPRINT = hashlib.sha256((json.dumps(_native_lock, indent=2)+'\n').encode() + b'native-source-recipe-1').hexdigest()
PREFIX = DATA / 'native-source' / FINGERPRINT[:12]
SDK = PREFIX / 'sdk'
RADV = PREFIX / 'radv'
ENV = {'LLVM_CONFIG': shutil.which('llvm-config-18') or '/usr/bin/llvm-config-18', 'PS5_CLANG': 'clang-18', 'PS5_PAYLOAD_SDK': str(SDK),
       'PATH': str(SDK/'bin') + ':/usr/lib/llvm-18/bin:' + os.environ['PATH']}


def sdk():
    marker = PREFIX/'sdk-complete.json'
    artifacts = [SDK/'target/lib/libc.a', SDK/'target/lib/libps5platform.a', SDK/'target/lib/libc++.a',
                 SDK/'target/lib/libc++abi.a', SDK/'target/lib/libunwind.a', SDK/'target/lib/libSceImeDialog.so']
    sdk_recipe=FINGERPRINT+'-sdk-executable-checks-v2'
    base_marker=PREFIX/'sdk-base-complete.json'
    base_artifacts=[artifacts[0],artifacts[1],artifacts[5],SDK/'bin/prospero-clang']
    if marker.exists():
        previous=json.loads(marker.read_text())
        if previous.get('fingerprint')==sdk_recipe:
            complete(marker,artifacts,sdk_recipe)
            return
        # Migrate only our identified earlier source recipe, preserving its artifacts.
        complete(marker,artifacts,FINGERPRINT)
        history=PREFIX/'sdk-history/static-checks-v1'
        for file in [marker,*artifacts,SDK/'target/include/c++/v1/__config_site']:
            target=history/file.relative_to(PREFIX)
            target.parent.mkdir(parents=True,exist_ok=True)
            if not target.exists(): shutil.copy2(file,target)
        if not base_marker.exists(): record(base_marker,base_artifacts,FINGERPRINT)
    source = export_git('sdk', PREFIX/'sdk-source')
    if not complete(base_marker,base_artifacts,FINGERPRINT):
        run(['make', '-C', source, '-j2', 'install', 'DESTDIR='+str(SDK), 'LLVM_CONFIG=llvm-config-18'], PREFIX/'logs/sdk.log', env=ENV)
        record(base_marker,base_artifacts,FINGERPRINT)
    spec = LOCK['libraries']['llvm_runtimes']
    include = ('runtimes/', 'libcxx/', 'libcxxabi/', 'libunwind/', 'cmake/', 'llvm/cmake/', 'llvm/utils/', 'llvm/include/', 'LICENSE.TXT')
    exclude = ('libcxx/test/', 'libcxx/docs/', 'libcxx/benchmarks/', 'libcxxabi/test/', 'libunwind/test/')
    llvm = extract(download(spec), PREFIX/'llvm-source', {'sha256': spec['sha256'], 'include': include, 'exclude': exclude},
                   include, exclude) / ('llvm-project-'+spec['version']+'.src')
    build = PREFIX/'llvm-runtimes-source-v2'
    flags = ['-DCMAKE_TOOLCHAIN_FILE='+str(SDK/'toolchain/prospero.cmake'), '-DCMAKE_INSTALL_PREFIX='+str(SDK/'target'),
             '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=EXECUTABLE',
             '-DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-sie-ps5', '-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF',
             '-DCMAKE_CXX_FLAGS=-nostdlib++', '-DLLVM_ENABLE_RUNTIMES=libunwind;libcxxabi;libcxx',
             '-DLIBCXX_ENABLE_STATIC=YES', '-DLIBCXX_ENABLE_RTTI=YES', '-DLIBCXX_ENABLE_EXCEPTIONS=YES',
             '-DLIBCXX_ENABLE_THREADS=YES', '-DLIBCXX_ENABLE_SHARED=NO', '-DLIBCXX_ENABLE_ABI_LINKER_SCRIPT=NO',
             '-DLIBCXX_INCLUDE_TESTS=OFF', '-DLIBCXX_INCLUDE_BENCHMARKS=OFF', '-DLIBCXXABI_INCLUDE_TESTS=OFF', '-DLIBUNWIND_INCLUDE_TESTS=OFF',
             '-DLIBCXX_INCLUDE_DOCS=OFF', '-DLIBCXXABI_INCLUDE_DOCS=OFF', '-DLLVM_INCLUDE_DOCS=OFF',
             '-DLIBCXX_CXX_ABI=libcxxabi', '-DLIBCXXABI_ENABLE_STATIC=YES', '-DLIBCXXABI_ENABLE_EXCEPTIONS=YES',
             '-DLIBCXXABI_ENABLE_THREADS=YES', '-DLIBCXXABI_USE_LLVM_UNWINDER=YES', '-DLIBCXXABI_ENABLE_SHARED=NO',
             '-DLIBCXXABI_USE_COMPILER_RT=NO', '-DLIBUNWIND_ENABLE_STATIC=YES', '-DLIBUNWIND_ENABLE_THREADS=YES',
             '-DLIBUNWIND_IS_BAREMETAL=YES', '-DLIBUNWIND_ENABLE_SHARED=NO', '-DLIBUNWIND_USE_COMPILER_RT=NO']
    run(['cmake', '-S', llvm/'runtimes', '-B', build, '-G', 'Ninja', *flags], PREFIX/'logs/llvm-configure.log', env=ENV)
    run(['cmake', '--build', build, '-j2'], PREFIX/'logs/llvm-build.log', env=ENV)
    # Keep unchanged header timestamps stable across a source-recipe repair.
    headers={p:(digest(p),p.stat().st_atime_ns,p.stat().st_mtime_ns) for p in (SDK/'target/include').rglob('*') if p.is_file()}
    run(['cmake', '--install', build], PREFIX/'logs/llvm-install.log', env=ENV)
    for path,(sha,atime,mtime) in headers.items():
        if path.is_file() and digest(path)==sha:
            os.utime(path,ns=(atime,mtime))
    (SDK/'.ps5-sdk-revision').write_text(LOCK['repositories']['sdk']['commit']+'\n')
    record(marker, artifacts, sdk_recipe)


def generators():
    paths = []
    meson = None
    for name, spec in LOCK['host_sources'].items():
        source = extract(download(spec), PREFIX/('python-'+name), {'sha256': spec['sha256']})
        package = next(p for p in source.iterdir() if p.is_dir())
        if name == 'meson':
            meson = package/'meson.py'
        else:
            paths.append(str(package/'src'))
    return meson, {**ENV, 'PYTHONPATH': os.pathsep.join(paths)}


def radv_headers():
    # Public headers only. This lets independent profile compilation precede driver linking.
    source=ROOT/LOCK['repositories']['mesa']['path']/'include'
    for directory in ('vulkan','vk_video'):
        shutil.copytree(source/directory,RADV/'include'/directory,dirs_exist_ok=True)


def radv():
    marker = PREFIX/'radv-complete.json'
    artifacts = [RADV/'lib/libvulkan_radeon.ps5.a', RADV/'include/vulkan/vulkan.h']
    if complete(marker, artifacts, FINGERPRINT):
        return
    sdk()
    source = export_git('mesa', PREFIX/'mesa-source')
    radv_headers()
    meson, env = generators()
    # The Mesa commit pins this source-only subproject (archive + Meson build definitions).
    import configparser
    parser = configparser.ConfigParser()
    parser.read(source/'subprojects/zlib.wrap')
    wrap = parser['wrap-file']
    cache = source/'subprojects/packagecache'
    for kind in ('source', 'patch'):
        url = wrap[kind+'_url'].replace('http://zlib.net/', 'https://zlib.net/')
        download({'url':url, 'sha256':wrap[kind+'_hash']}, cache/wrap[kind+'_filename'])
    clc = PREFIX/'mesa-clc'
    if not (clc/'build.ninja').exists():
        run([sys.executable, meson, 'setup', clc, source, '-Dbuildtype=release', '-Dmesa-clc=enabled', '-Dinstall-mesa-clc=true',
             '-Dgallium-drivers=', '-Dvulkan-drivers=', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled',
             '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=enabled', '-Dshared-llvm=enabled',
             '-Dbuild-tests=false', '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dzstd=disabled', '-Dxmlconfig=disabled', '-Dtools='], PREFIX/'logs/clc-configure.log', env=env)
    run(['ninja', '-C', clc, '-j2', 'src/compiler/clc/mesa_clc', 'src/compiler/spirv/vtn_bindgen2'], PREFIX/'logs/clc-build.log', env=env)
    bin_dir = PREFIX/'clc-bin'
    bin_dir.mkdir(parents=True, exist_ok=True)
    for name, path in [('mesa_clc', clc/'src/compiler/clc/mesa_clc'), ('vtn_bindgen2', clc/'src/compiler/spirv/vtn_bindgen2')]:
        target = bin_dir/name
        if not target.exists():
            target.symlink_to(path)
    env['PATH'] = str(bin_dir)+':'+env['PATH']
    constants = PREFIX/'cross-constants.ini'
    constants.write_text("[constants]\nsdk = '"+str(SDK)+"'\n")
    driver = ROOT/LOCK['repositories']['vulkan']['path']
    build = PREFIX/'mesa-ps5'
    if not (build/'build.ninja').exists():
        run([sys.executable, meson, 'setup', build, source, '--wrap-mode=nodownload',
             '--cross-file', constants, '--cross-file', driver/'tooling/radv/ps5-cross.ini',
             '-Dvulkan-drivers=amd', '-Dgallium-drivers=', '-Dplatforms=', '-Dradv-winsys=ps5',
             '-Dllvm=disabled', '-Damd-use-llvm=false', '-Dvideo-codecs=', '-Dbuildtype=debugoptimized', '-Db_ndebug=true',
             '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
             '-Dvalgrind=disabled', '-Dlibunwind=disabled', '-Dzstd=disabled', '-Dzlib=enabled', '--force-fallback-for=zlib',
             '-Dexpat=disabled', '-Dxmlconfig=disabled', '-Dshader-cache=enabled', '-Dbuild-tests=false', '-Dvulkan-layers=',
             '-Dtools=', '-Dmesa-clc=system', '-Dradv-build-id='+LOCK['repositories']['mesa']['commit']], PREFIX/'logs/radv-configure.log', env=env)
    run(['ninja', '-C', build, '-j2', 'src/amd/vulkan/libvulkan_radeon.a'], PREFIX/'logs/radv-build.log', env=env)
    (RADV/'lib').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build/'src/amd/vulkan/libvulkan_radeon.a', artifacts[0])
    for directory in ('vulkan', 'vk_video'):
        shutil.copytree(source/'include'/directory, RADV/'include'/directory, dirs_exist_ok=True)
    record(marker, artifacts, FINGERPRINT)


def tools():
    marker = PREFIX/'tools-complete.json'
    native_tool = PREFIX/'tools/ps5-native-tool'
    libc_builder = PREFIX/'tools/libc-builder'
    if complete(marker, [native_tool, libc_builder], FINGERPRINT):
        return
    spec = LOCK['libraries']['zlib_host']
    source = extract(download(spec), PREFIX/'zlib-source', {'sha256':spec['sha256']})/('zlib-'+spec['version'])
    build = PREFIX/'zlib-build'
    run(['cmake','-S',source,'-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DZLIB_BUILD_EXAMPLES=OFF'], PREFIX/'logs/zlib-configure.log')
    run(['cmake','--build',build,'--target','zlibstatic','-j2'], PREFIX/'logs/zlib-build.log')
    (PREFIX/'tools').mkdir(parents=True, exist_ok=True)
    driver = ROOT/LOCK['repositories']['vulkan']['path']/'tooling/native'
    run(['clang++-18','-std=c++20','-O2','-I',source,'-I',build, driver/'native_app_builder.cpp', driver/'self_container.cpp',
         driver/'elf_object.cpp',driver/'sce_module_writer.cpp',build/'libz.a','-o',native_tool],PREFIX/'logs/native-tool.log')
    run(['clang++-18','-std=c++20','-O2',driver/'libc_builder.cpp','-o',libc_builder],PREFIX/'logs/libc-builder.log')
    record(marker,[native_tool,libc_builder],FINGERPRINT)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', choices=['sdk','radv','tools','all'], default='all')
    stage = parser.parse_args().stage
    PREFIX.mkdir(parents=True, exist_ok=True)
    for name in (['sdk','radv','tools'] if stage == 'all' else [stage]):
        globals()[name]()
    save_json(PREFIX/'paths.json',{'sdk':str(SDK),'radv':str(RADV),'prefix':str(PREFIX),'source_fingerprint':FINGERPRINT})
    print('Pinned source component build finished: '+stage,flush=True)
