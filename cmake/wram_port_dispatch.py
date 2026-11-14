"""Add an exact, zero-delta WMDATA route to a reviewed runtime overlay."""
import argparse
import hashlib
from pathlib import Path
import re

def function_span(text, name):
    matches=list(re.finditer(r"(?m)^void\s+"+re.escape(name)+r"\s*\(",text))
    if len(matches)!=1:raise ValueError('Expected one function '+name)
    start=matches[0].start();opening=text.index('{',matches[0].end());depth=0
    tokens=re.finditer(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',text[opening:])
    for token in tokens:
        if token.group()=='{':depth+=1
        elif token.group()=='}':
            depth-=1
            if not depth:return start,opening+token.end()
    raise ValueError('Unbalanced '+name)

def patch(source, beam):
    start,end=function_span(source,'WriteReg')
    if hashlib.sha256(source[start:end].encode()).hexdigest() != "be79d8393678f7e02c7d9a0d53d861d15c0553d4434ee1f33bad27db2ca2184f":
        raise ValueError('Review changed WriteReg contract')
    a,b=function_span(beam,'snes_sync_master_clock')
    if hashlib.sha256(beam[a:b].encode()).hexdigest() != "2805eafc6e8862da145be57115a1467223e998d1ccb1c433749650440b129d71":
        raise ValueError('Review changed zero-delta beam contract')
    opening=source.index('{',start)+1
    return source[:opening]+"\n"+'  /* A synced WMDATA byte has no remaining beam work. Keep the original\n   * B-bus handler and observer; only redundant generic dispatch is omitted. */\n  if (reg == 0x2180u && g_snes &&\n      g_snes->beamMasterLast == g_cpu.master_cycles) {\n    snes_writeBBus(g_snes, 0x80u, value);\n    debug_server_on_reg_write(reg, value);\n    return;\n  }\n'+source[opening:]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True)
    p.add_argument('--beam',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    result=patch(a.input.read_text(encoding='utf-8'),a.beam.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    if not a.output.exists() or a.output.read_text(encoding='utf-8')!=result:
        a.output.write_text(result,encoding='utf-8')

if __name__=='__main__':main()
