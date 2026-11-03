"""Explicit evidence for a declaration-only translation unit; default is fail closed."""
from pathlib import Path
import re
from .model import AnalysisError,display_path,digest_bytes


def validate_contracts(data):
    if not isinstance(data,dict) or data.get('version')!=1 or not isinstance(data.get('declaration_only_units'),list):
        raise AnalysisError('invalid body presence contract')
    seen=set()
    for row in data['declaration_only_units']:
        if not isinstance(row,dict) or set(row)!={'source','source_sha256','headers','declarations'}:
            raise AnalysisError('invalid declaration-only unit contract')
        if not isinstance(row['headers'],list) or any(not isinstance(x,dict) or set(x)!={'path','sha256'} for x in row['headers']):
            raise AnalysisError('declaration-only headers invalid')
        paths=[row['source']]+[x['path'] for x in row['headers']]
        if not paths or len(paths)<2 or len(set(paths))!=len(paths):raise AnalysisError('declaration-only source/header set invalid')
        for path in paths:
            if not isinstance(path,str) or not path or Path(path).is_absolute() or Path(path).drive or '..' in Path(path).parts or chr(92) in path:
                raise AnalysisError('declaration-only path must be root relative')
        if row['source'] in seen:raise AnalysisError('duplicate declaration-only source')
        seen.add(row['source'])
        hashes=[row['source_sha256']]+[x.get('sha256') for x in row['headers']]
        if any(not isinstance(h,str) or not re.fullmatch('[0-9a-f]{64}',h) for h in hashes):raise AnalysisError('declaration-only hash invalid')
        if not isinstance(row['declarations'],list) or not row['declarations'] or any(not isinstance(x,str) or not x for x in row['declarations']) or len(set(row['declarations']))!=len(row['declarations']):raise AnalysisError('declaration-only expected declarations invalid')
    return data


def verify_declaration_only(frontend,snapshot,contracts):
    relative=display_path(frontend.command['file'],frontend.root)
    spec=next((x for x in contracts['declaration_only_units'] if x['source']==relative),None)
    if spec is None:raise AnalysisError('no function bodies found in selected source scope; declaration-only contract absent')
    if not frontend.selected(Path(frontend.command['file'])):raise AnalysisError('declaration-only TU is outside selected scope')
    required=[{'path':spec['source'],'sha256':spec['source_sha256']}]+spec['headers']
    for row in required:
        absolute=(frontend.root/row['path']).resolve()
        if not absolute.is_relative_to(frontend.root) or not frontend.selected(absolute) or str(absolute) not in frontend.included_files:
            raise AnalysisError('declaration-only source/header not parsed in selected scope')
        if digest_bytes(snapshot.capture(absolute,text=True))!=row['sha256']:
            raise AnalysisError('declaration-only source/header hash differs')
    observed={v['qualified_name'] for v in frontend.symbols.values() if v['kind']=='FUNCTION_DECL' and v['evidence']['start']['path'] in {x['path'] for x in spec['headers']}}
    if observed!=set(spec['declarations']):raise AnalysisError('declaration-only declarations differ or were not observed')
    return {'kind':'fingerprinted-declaration-only','method':'explicit-body-presence-contract/v1','source':relative,'source_sha256':spec['source_sha256'],'headers':spec['headers'],'observed_declarations':sorted(observed),'selected_function_bodies':0,'interpretation':'Successfully parsed explicit declarations; no function body metric or design-quality claim.'}
