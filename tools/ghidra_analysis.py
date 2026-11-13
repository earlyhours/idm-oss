#!/usr/bin/env python3
"""Run an isolated free REA/Ghidra MCP session, retaining complete evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess


class Session:
    def __init__(self, executable, output):
        self.counter = 0
        self.log = (output / 'rea-stderr.log').open('w')
        self.process = subprocess.Popen([str(executable), '--mcp'], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.log, text=True)
        self.request('initialize', {'protocolVersion': '2024-11-05', 'capabilities': {},
                                  'clientInfo': {'name': 'idm-free-analysis', 'version': '1'}})
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'method': 'notifications/initialized'})+'\n')
        self.process.stdin.flush()

    def request(self, method, params):
        self.counter += 1
        self.process.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': self.counter,
                                            'method': method, 'params': params})+'\n')
        self.process.stdin.flush()
        for line in self.process.stdout:
            result = json.loads(line)
            if result.get('id') == self.counter:
                if 'error' in result:
                    raise RuntimeError(result['error'])
                return result['result']
        raise RuntimeError('REA server exited before response; inspect rea-stderr.log')

    def tool(self, name, arguments):
        result = self.request('tools/call', {'name': name, 'arguments': arguments})
        if result.get('isError'):
            raise RuntimeError(result)
        return result.get('structuredContent') or json.loads(next(x['text'] for x in result['content'] if x['type']=='text'))

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=15)
        self.log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--rea', required=True, type=Path)
    parser.add_argument('--ghidra', required=True, type=Path)
    parser.add_argument('--java-home', required=True, type=Path)
    parser.add_argument('--string', action='append', default=[])
    parser.add_argument('--xref', action='append', default=[])
    parser.add_argument('--function', action='append', default=[])
    parser.add_argument('--max-functions', type=int, default=8)
    args=parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    os.environ.update(GHIDRA_INSTALL_DIR=str(args.ghidra.resolve()), JAVA_HOME=str(args.java_home.resolve()),
                      GHIDRA_HEADLESS_MAXMEM='2G', REA_GHIDRA_STARTUP_TIMEOUT_MS='900000')
    session=Session(args.rea,args.output)
    index=[]
    def save(name, arguments):
        result=session.tool(name,arguments)
        destination=args.output / f'{len(index):03}-{name}.json'
        destination.write_text(json.dumps(result,indent=2)+'\n')
        index.append(dict(operation=name,arguments=arguments,path=destination.name,
                          evidence_id=result.get('evidence_id')))
        (args.output/'index.json').write_text(json.dumps(index,indent=2)+'\n')
        print(json.dumps(index[-1]),flush=True)
        return result
    try:
        save('open_binary',dict(path=str(args.binary.resolve()),provider_id='ghidra'))
        save('binary_overview',{})
        anchors=list(args.xref)
        for text in args.string:
            result=save('search_strings',dict(pattern=text))
            anchors.extend(x['address'] for x in result.get('normalized_result',[]))
        functions=dict.fromkeys(args.function)
        for anchor in dict.fromkeys(anchors):
            result=save('xrefs',dict(address=anchor))
            for site in result.get('normalized_result',[]):
                containing=save('resolve_containing_procedure',dict(address=site))
                found=containing.get('normalized_result',{})
                if found.get('found'):
                    procedure=found['procedure']
                    functions[procedure['address']]=procedure['name']
        for address in list(functions)[:args.max_functions]:
            save('procedure_pseudo_code',dict(procedure=address))
        save('export_evidence_bundle',dict(path=str((args.output/'evidence-bundle.json').resolve())))
        save('close_binary',dict(snapshot_path=str((args.output/'analysis-snapshot.json').resolve())))
    finally:
        # Preserve retained results even when a later query fails.
        try:
            if not (args.output/'evidence-bundle.json').exists():
                save('export_evidence_bundle',dict(path=str((args.output/'evidence-bundle.json').resolve())))
            if not (args.output/'analysis-snapshot.json').exists():
                save('close_binary',dict(snapshot_path=str((args.output/'analysis-snapshot.json').resolve())))
        finally:
            session.close()


if __name__=='__main__':
    main()
