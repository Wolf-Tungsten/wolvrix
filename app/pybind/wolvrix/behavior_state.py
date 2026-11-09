"""Import explicit v2 array bindings before hierarchy flattening.

The fallback SV is complete RTL, not an opaque runtime call. The importer
replaces only authenticated carrier module bodies and keeps their typed ports.
"""
import copy
import hashlib
import re
from pathlib import Path


def _attr(value):
    if isinstance(value, bool): kind='bool'
    elif isinstance(value, int): kind='int'
    elif isinstance(value, str): kind='string'
    elif isinstance(value, list): return dict(t='int[]' if value and isinstance(value[0], int) else 'string[]',vs=value)
    else: raise ValueError('unsupported behavior attribute')
    return dict(t=kind,v=value)


def carrier_source(binding):
    """Canonical fallback template, checked against both metadata and read RTL."""
    arrays=binding['arrays'];name=binding['module']
    if not re.fullmatch(r'WolvrixBehaviorStateGroup_v1_[0-9]+',name):raise ValueError('unknown carrier version/name')
    ports=[('clock',1,True),('reset',1,True)]
    for kind,directions in (('updates',('valid','index','data')),('reads',('index','data'))):
        for i,a in enumerate(binding[kind]):
            spec=arrays[a];aw=max(1,(spec['rows']-1).bit_length());prefix='u' if kind=='updates' else 'r'
            for field in directions:ports.append((f'{prefix}{i}_{field}',1 if field=='valid' else aw if field=='index' else spec['width'],kind=='updates' or field=='index'))
    expected=[dict(name=n,width=w,direction='input' if inp else 'output') for n,w,inp in ports]
    if binding['ports']!=expected:raise ValueError('behavior metadata ports disagree with array operations')
    text='module '+name+'(\n'+',\n'.join(f"  {'input' if inp else 'output'} wire [{w-1}:0] {n}" for n,w,inp in ports)+'\n);\n'
    for i,a in enumerate(arrays):text+=f"reg [{a['width']-1}:0] state{i} [0:{a['rows']-1}];\n"
    for i,a in enumerate(binding['reads']):text+=f"assign r{i}_data = state{a}[r{i}_index & {arrays[a]['rows']-1}];\n"
    text+='always @(posedge clock) begin\n  if (reset) begin\n'
    for i,a in enumerate(arrays):
        for row,v in enumerate(a['initial']):text+=f"    state{i}[{row}] <= {a['width']}'d{v};\n"
    text+='  end else begin\n'
    for i,a in enumerate(binding['updates']):text+=f"    if (u{i}_valid) state{a}[u{i}_index & {arrays[a]['rows']-1}] <= u{i}_data;\n"
    return text+'  end\nend\nendmodule\n'


def validate_observer_state_feedback(design):
    """Follow group observations through declared debug state to their consumers."""
    for graph in design['graphs']:
        ops=graph['ops'];owned={o['attrs']['memSymbol']['v'] for o in ops if o['kind']=='kBehaviorStateGroup'}
        if not owned:continue
        users={};states={};readers={}
        read_kinds={'kRegisterReadPort':'regSymbol','kLatchReadPort':'latchSymbol','kMemoryReadPort':'memSymbol'}
        write_kinds={'kRegisterWritePort':'regSymbol','kLatchWritePort':'latchSymbol','kMemoryWritePort':'memSymbol','kMemoryFillPort':'memSymbol'}
        def observer(op):return bool(op.get('attrs',{}).get('behavior.observerDomain',{}).get('v'))
        for i,op in enumerate(ops):
            for value in op['in']:users.setdefault(value,[]).append(i)
            if op['kind'] in ('kRegister','kLatch','kMemory'):states[op['sym']]=observer(op)
            if op['kind'] in read_kinds:
                symbol=op['attrs'][read_kinds[op['kind']]]['v']
                readers.setdefault(symbol,[]).extend(op['out'])
        todo=[v for o in ops if o['kind']=='kMemoryReadPort' and o['attrs']['memSymbol']['v'] in owned for v in o['out']]
        seen=set()
        while todo:
            value=todo.pop()
            if value in seen:continue
            seen.add(value)
            for i in users.get(value,[]):
                op=ops[i]
                if op['kind']=='kBehaviorStateGroup':continue
                if op['kind'] in write_kinds:
                    symbol=op['attrs'][write_kinds[op['kind']]]['v']
                    if not states.get(symbol,False):raise ValueError('observer group feeds unowned state/effect: '+op['sym'])
                    todo.extend(readers.get(symbol,[]))
                elif op['kind']=='kXMRWrite' or (op['kind'] in ('kDpicCall','kSystemTask') and (not observer(op) or op['out'])):
                    raise ValueError('observer group feeds unowned state/effect: '+op['sym'])
                todo.extend(op['out'])


def bind_observer_domains(design, manifest):
    """Bind explicitly declared module boundaries, without a name-prefix allowlist.

    Unused ports may be removed by firtool; every remaining port must match the
    elaborated declaration. Descendants are specialized for each observer
    domain, leaving shared functional instances of e.g. DelayReg unlabelled.
    """
    domains=manifest.get('observer_domains',[])
    if not domains:return
    graphs={g['symbol']:g for g in design['graphs']}
    roots={d['module'] for d in domains}
    for ordinal,domain in enumerate(domains):
        if domain.get('version')!=1 or not domain.get('id') or domain['module'] not in graphs:
            raise ValueError('invalid/missing observer domain binding')
        graph=graphs[domain['module']];vals={v['sym']:v for v in graph['vals']}
        expected={p['name']:(p['direction'],p['width']) for p in domain['ports']}
        actual={p['name']:(direction,vals[p['val']]['w']) for key,direction in [('in','input'),('out','output')] for p in graph['ports'][key]}
        if graph['ports'].get('inout') or not actual or any(expected.get(k)!=v for k,v in actual.items()):
            raise ValueError('observer domain port mismatch: '+domain['module'])
        specialized={}
        def own(name):
            if name in roots and name!=domain['module']:return name
            if name in specialized:return specialized[name]
            source=graphs[name]
            target=name if name==domain['module'] else f'WolvrixObserverDomain_v1_{ordinal}_{name}'
            specialized[name]=target
            if target!=name:
                if target in graphs:raise ValueError('duplicate observer specialization')
                source=copy.deepcopy(source);source['symbol']=target
                graphs[target]=source;design['graphs'].append(source)
            for op in source['ops']:
                if op['kind'] in ('kRegister','kLatch','kMemory','kBehaviorStateGroup','kDpicCall','kSystemTask'):
                    op.setdefault('attrs',{})['behavior.observerDomain']=_attr(domain['id'])
                if op['kind']=='kInstance' and domain.get('include_children'):
                    child=op['attrs']['moduleName']['v']
                    op['attrs']['moduleName']=_attr(own(child))
            return target
        own(domain['module'])


def lower_behavior_state(design, manifest, source_dir):
    if manifest.get('version') not in (2, 3): raise ValueError('grouped lowering requires binding manifest v2/v3')
    bindings={}
    for model in manifest.get('models',[]):
        if model.get('lowering')=='ordinary':
            if model.get('binding'):raise ValueError('ordinary model has grouped binding')
            continue
        b=model.get('binding')
        if not b or b.get('version')!=1 or not model.get('observer_only'):
            raise ValueError('missing/unsupported observer array binding')
        name=b['module']
        if name in bindings: raise ValueError('duplicate behavior module binding')
        if hashlib.sha256(carrier_source(b).encode()).hexdigest()!=b['sv_sha256']:
            raise ValueError('behavior metadata differs from fallback contract')
        bindings[name]=b
    result=copy.deepcopy(design);found=set()
    for graph in result['graphs']:
        # Graph names may be uniquified by ingest; use an exact module symbol,
        # never guess ownership from arbitrary flattened signal names.
        name=graph['symbol']
        if name not in bindings: continue
        b=bindings[name];found.add(name)
        sources={op.get('loc',{}).get('file') for op in graph['ops']} - {None, ''}
        authenticated=False
        for filename in sources:
            source=Path(filename)
            if not source.is_absolute() and not source.exists():source=Path(source_dir)/source
            text=source.read_text()
            bodies=re.findall(r'module\s+'+re.escape(name)+r'\b[\s\S]*?endmodule(?:\n|$)',text)
            # Split-verilog output may omit the final newline and prepend a
            # firtool comment. Authenticate the complete module body itself.
            body=(bodies[0].removesuffix('\n')+'\n') if len(bodies)==1 else ''
            if body and hashlib.sha256(body.encode()).hexdigest()==b['sv_sha256']:
                authenticated=True
        if not authenticated:raise ValueError('behavior fallback source hash mismatch: '+name)

        arrays=b['arrays']; updates=b['updates']; reads=b['reads']
        if not arrays: raise ValueError('behavior group must own arrays')
        offsets=[];initial=[];total=0
        for a in arrays:
            n,w=a['rows'],a['width']
            if n<1 or n>65536 or n&(n-1) or not 1<=w<=64: raise ValueError('unsupported array shape')
            if len(a['initial'])!=n or any(not 0<=int(x)<(1<<w) for x in a['initial']):raise ValueError('invalid initializer')
            offsets.append(total);total+=n;initial+=a['initial']
        if total>65536 or len({a['name'] for a in arrays})!=len(arrays):raise ValueError('invalid group layout')
        if any(not isinstance(a,int) or not 0<=a<len(arrays) for a in updates+reads):raise ValueError('invalid array binding index')
        width=max(a['width'] for a in arrays)
        ports=graph['ports'];vals={v['sym']:v for v in graph['vals']}
        actual={p['name']:(direction,vals[p['val']]['w']) for key,direction in [('in','input'),('out','output')] for p in ports[key]}
        expected={p['name']:(p['direction'],p['width']) for p in b['ports']}
        if len(expected)!=len(b['ports']) or actual!=expected or ports.get('inout'):raise ValueError('behavior port binding mismatch: '+name)
        portvals={p['name']:p['val'] for key in ('in','out') for p in ports[key]}
        graph['vals']=[{k:v for k,v in vals[p['val']].items() if k in ('sym','w','sgn','type','in','out','inout')} for key in ('in','out') for p in ports[key]]
        graph['ops']=[];graph['declaredSymbols']=[]
        def val(name,w):
            graph['vals'].append(dict(sym=name,w=w,sgn=False,type='logic',**{'in':False,'out':False,'inout':False}));return name
        def op(kind,inputs,outputs,attrs=None,name=None):
            name=name or 'behavior_op_'+str(len(graph['ops']))
            graph['ops'].append(dict(sym=name,kind=kind,**{'in':inputs,'out':outputs},attrs={k:_attr(v) for k,v in (attrs or {}).items()}));return name
        def const(n,w=32):
            name=val('behavior_const_'+str(len(graph['vals'])),w);op('kConstant',[],[name],dict(constValue=f"{w}'d{n}"));return name
        memory=op('kMemory',[],[],dict(width=width,row=total,isSigned=False),name='behavior_storage')
        args=[const(1,1),portvals['reset']]
        for i in range(len(updates)):args += [portvals[f'u{i}_{s}'] for s in ('valid','index','data')]
        args.append(portvals['clock'])
        # Empty vectors have an explicit typed tag; no schema guessing.
        op('kBehaviorStateGroup',args,[],{'memSymbol':memory,'eventEdge':['posedge'],
            'behavior.version':1,'behavior.observerOnly':True,'behavior.rows':[a['rows'] for a in arrays],
            'behavior.widths':[a['width'] for a in arrays],'behavior.names':[a['name'] for a in arrays],
            'behavior.initial':[str(x) for x in initial],'behavior.updateArrays':updates})
        graph['ops'][-1]['attrs']['behavior.updateArrays']['t']='int[]'
        for i,a in enumerate(reads):
            local=portvals[f'r{i}_index'];out=portvals[f'r{i}_data']
            # Explicit local mask is also needed for the one-row v0 array.
            masked=val(f'behavior_masked_{i}',32)
            op('kAnd',[local,const(arrays[a]['rows']-1)],[masked])
            address=val(f'behavior_address_{i}',32)
            op('kAdd',[masked,const(offsets[a])],[address])
            data=val(f'behavior_read_{i}',width)
            op('kMemoryReadPort',[address],[data],dict(memSymbol=memory))
            if arrays[a]['width']==width:op('kAssign',[data],[out])
            else:op('kSliceStatic',[data],[out],dict(sliceStart=0,sliceEnd=arrays[a]['width']-1))
    if found!=set(bindings):raise ValueError('unbound behavior modules: '+str(sorted(set(bindings)-found)))
    if not found:raise ValueError('grouped lowering requested without any bound model')
    bind_observer_domains(result,manifest)
    return result
