#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch exact public source commits; resume owned fetches without resetting foreign work."""
import argparse
import json
import pathlib
import re
import subprocess

from prepare_textures import reject_links

ROOT = pathlib.Path(__file__).resolve().parents[1]


def git(*args, optional=False):
    result=subprocess.run(['git',*map(str,args)],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if result.returncode and not optional:
        raise ValueError('Git source operation failed: '+result.stderr.strip())
    return result.stdout.strip() if result.returncode==0 else None


def intent(name, entry, root=ROOT):
    path=root/entry['path']
    if not path.is_relative_to(root) or '..' in path.parts or not re.fullmatch('[0-9a-f]{40}',entry['commit']):
        raise ValueError('Invalid dependency lock entry: '+name)
    reject_links(path)
    marker=root/'out/data/dependency-fetch'/(name+'.json')
    reject_links(marker)
    identity={key:entry[key] for key in ('path','commit','url')}
    if marker.exists() and json.loads(marker.read_text()).get('identity')!=identity:
        raise ValueError('Different dependency fetch state preserved: '+name)
    return path,marker,identity


def inspect(name, entry, root=ROOT):
    path,marker,identity=intent(name,entry,root)
    owned=marker.is_file() and json.loads(marker.read_text()).get('identity')==identity
    if not path.exists():
        return 'missing'
    if not (path/'.git').is_dir():
        if owned and path.is_dir() and not any(path.iterdir()):
            return 'pending'
        raise ValueError('Dependency is not a dedicated Git checkout: '+name)
    head=git('-C',path,'rev-parse','--verify','HEAD',optional=True)
    if head is None and owned:
        remote=git('-C',path,'remote','get-url','origin',optional=True)
        if remote not in (None,entry['url']) or git('--no-optional-locks','-C',path,'status','--porcelain'):
            raise ValueError('Pending dependency contains different work; preserved: '+name)
        return 'pending'
    if head!=entry['commit'] or git('--no-optional-locks','-C',path,'status','--porcelain'):
        raise ValueError('Dependency revision or worktree differs: '+name)
    return 'pinned'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('names',nargs='*',help='Repository names from dependencies.lock.json')
    args=parser.parse_args()
    repos=json.loads((ROOT/'dependencies.lock.json').read_text())['repositories']
    selected=args.names or list(repos)
    states={}
    for name in selected:
        if name not in repos:
            parser.error('Unknown locked repository: '+name)
        states[name]=inspect(name,repos[name])
    from source_support import save_json
    for name in selected:
        entry=repos[name]
        path,marker,identity=intent(name,entry)
        if states[name]!='pinned':
            # Intent precedes mutation, so a failed init/fetch can safely resume later.
            save_json(marker,{'identity':identity,'status':'IN_PROGRESS'})
            path.parent.mkdir(parents=True,exist_ok=True)
            if not (path/'.git').exists():
                git('init',path)
                git('-C',path,'config','core.autocrlf','false')
            if git('-C',path,'remote','get-url','origin',optional=True) is None:
                git('-C',path,'remote','add','origin',entry['url'])
            git('-C',path,'fetch','--depth=1','origin',entry['commit'])
            git('-C',path,'checkout','--detach','FETCH_HEAD')
            if git('-C',path,'rev-parse','HEAD')!=entry['commit']:
                raise ValueError('Fetched revision differs: '+name)
            save_json(marker,{'identity':identity,'status':'DONE'})
        print(name+': '+entry['commit'],flush=True)


if __name__=='__main__':
    main()
