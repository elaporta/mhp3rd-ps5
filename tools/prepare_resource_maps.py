#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Overlay only the eight pinned David-vz resource maps on a prepared Enhanced copy."""
import argparse
import json
import pathlib
import re
import struct
import zlib
import urllib.request
from audit_resources import ROOT, digest, inspect_textures
from prepare_textures import reject_links

def replace_mapping(ini,key,value):
    section='';count=0;result=[]
    for line in ini.splitlines(keepends=True):
        text=line.strip()
        if text.startswith('[') and text.endswith(']'):section=text.lower()
        if section=='[hashes]' and re.match(r'^'+re.escape(key)+r'\s*=',text,re.I):
            line=key+' = '+value+'\n';count+=1
        result.append(line)
    if count!=1:raise ValueError('Enhanced resource-map key missing/ambiguous: '+key)
    return ''.join(result)

def png_dimensions(data):
    if data[:8]!=b'\x89PNG\r\n\x1a\n':raise ValueError('Invalid PNG signature')
    offset=8;dimensions=None;ended=False
    while offset+12<=len(data):
        length=struct.unpack('>I',data[offset:offset+4])[0]
        tag=data[offset+4:offset+8];end=offset+8+length
        if end+4>len(data) or zlib.crc32(data[offset+4:end])!=struct.unpack('>I',data[end:end+4])[0]:
            raise ValueError('Truncated/corrupt PNG chunk')
        if tag==b'IHDR':
            if dimensions is not None or length!=13:raise ValueError('Invalid PNG header')
            dimensions=struct.unpack('>II',data[offset+8:offset+16])
            if not all(0<v<=4096 for v in dimensions):raise ValueError('PNG dimensions exceed supported bound')
        offset=end+4
        if tag==b'IEND':ended=True;break
    if not dimensions or not ended or offset!=len(data):raise ValueError('Incomplete PNG')
    return dimensions

def augment(pack,source,spec):
    reject_links(pack.absolute());reject_links(source.absolute())
    if pack.resolve()==source.resolve() or source.resolve().is_relative_to(pack.resolve()) or pack.resolve().is_relative_to(source.resolve()):
        raise ValueError('Separate immutable source required')
    for name in ('runtime-manifest.json','preparation-report.json','runtime-case-index.txt','textures.ini'):
        reject_links(pack/name)
    manifest=json.loads((pack/'runtime-manifest.json').read_text())
    for name,expected in manifest.items():
        path=pack/name;reject_links(path)
        if not path.resolve().is_relative_to(pack.resolve()) or digest(path)!=expected:
            raise ValueError('Prepared input differs: '+name)
    files=spec['files'];data={}
    for name,info in files.items():
        path=source/name;reject_links(path)
        if not path.resolve().is_relative_to(source.resolve()) or digest(path)!=info['sha256']:
            raise ValueError('Pinned resource differs: '+name)
        data[name]=path.read_bytes()
    ini_name='TEXTURES/NPJB40001/textures.ini'
    mappings=re.findall(r'^([0-9a-f]{24})\s*=\s*(MAP/[^\r\n]+)',data[ini_name].decode('utf-8-sig'),re.M|re.I)
    if len(mappings)!=8 or len({key for key,_ in mappings})!=8:raise ValueError('Expected eight resource-map keys')
    available={name.casefold():name for name in files}
    ini=(pack/'textures.ini').read_text(encoding='utf-8-sig');outputs={};dimensions={}
    for key,name in mappings:
        actual=available['TEXTURES/NPJB40001/'.casefold()+name.casefold()]
        target='resource-maps/david-vz/'+pathlib.PurePosixPath(actual).name
        dimensions[target]=png_dimensions(data[actual])
        ini=replace_mapping(ini,key,target)
        outputs[target]=data[actual]
    outputs['textures.ini']=ini.encode('utf-8')
    outputs['DAVID-VZ-README.md']=data['README.md']
    for name in outputs:reject_links(pack/name)
    for name,contents in outputs.items():
        path=pack/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(contents)
    for name in outputs:manifest[name]=digest(pack/name)
    names=''.join(name+'\n' for name in sorted(manifest) if name.lower().endswith('.png'))
    (pack/'runtime-case-index.txt').write_bytes(names.encode())
    manifest['runtime-case-index.txt']=digest(pack/'runtime-case-index.txt')
    report=inspect_textures(pack)
    if report['unsafe']:raise ValueError('Unsafe augmented pack')
    report.update(base='supplied Enhanced',resource_maps_commit=spec['commit'],resource_map_keys=len(mappings),resource_map_dimensions=dimensions)
    (pack/'preparation-report.json').write_text(json.dumps(report,indent=2)+'\n')
    (pack/'runtime-manifest.json').write_text(json.dumps(manifest,indent=2,sort_keys=True)+'\n')
    print('Enhanced +',len(mappings),'resource maps:',report['png_count'],'PNGs;',len(report['missing']),'missing paths retain originals')
    return report

def acquire(source,spec):
    reject_links(source.absolute())
    for name,entry in spec['files'].items():
        path=source/name;reject_links(path)
        if path.exists():
            if digest(path)!=entry['sha256']:raise ValueError('Existing pinned resource differs: '+name)
            continue
        data=urllib.request.urlopen(entry['url'],timeout=60).read()
        import hashlib
        if hashlib.sha256(data).hexdigest()!=entry['sha256']:raise ValueError('Downloaded resource digest differs')
        path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack',type=pathlib.Path,required=True)
    parser.add_argument('--source',type=pathlib.Path,default=ROOT/'out/data/dependencies/david-vz')
    parser.add_argument('--acquire',action='store_true',help='Fetch only the locked public texture-map files/credits')
    args=parser.parse_args()
    for path in (args.pack,args.source):
        if not path.resolve().is_relative_to((ROOT/'out/data').resolve()):parser.error('Assets must stay under ignored out/data')
    spec=json.loads((ROOT/'dependencies.lock.json').read_text())['assets']['david_resource_maps']
    if args.acquire:acquire(args.source,spec)
    augment(args.pack,args.source,spec)
