"""Render the complete recorded E3 matrix to editable TikZ; no experiments."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path


def build(summary, output):
    raw=summary.read_bytes()
    data=json.loads(raw)['repeated_ablation']
    assert data['complete_paired_decodes']==36 and len(data['rows'])==12
    rows=[];counts=Counter()
    for row in data['rows']:
        cells=[]
        for repeat in row['repeats']:
            assert repeat['paired_complete']
            nv,nf=repeat['native']['alerts'];cv,cf=repeat['no_internal']['alerts']
            direction=('positive_warning_recovery' if nv>0 and cv==0 else
                       'positive_fixed_warning_reduction' if nv>0 and cv>0 and nf<cf else
                       'negative_lost_version_hit' if cv>0 and nv==0 else
                       'negative_more_fixed_warnings' if nv>0 and cv>0 and nf>cf else 'tie_or_other')
            assert direction==repeat['outcome']
            counts[direction]+=1
            cells.append(dict(repeat=repeat['repeat'],native=[nv,nf],control=[cv,cf],outcome=direction))
        assert [c['repeat'] for c in cells]==[1,2,3]
        rows.append(dict(case_id=row['case_id'],cells=cells))
    assert dict(counts)==data['decode_outcome_counts']
    lines=[r'\begin{tikzpicture}[f258,x=1bp,y=-1bp]',
           r'\path[use as bounding box] (0,0) rectangle (378,174);',
           r'\node[anchor=west,inner sep=0bp] at (1,8) {Subject};']
    for j,title in enumerate(('Decode 1 (E1)','Decode 2','Decode 3')):
        x=41+j*112
        lines.append(rf'\node[inner sep=0bp] at ({x+56},8) {{{title}}};')
    for i,row in enumerate(rows):
        y=18+i*10.54
        lines.append(rf'\node[anchor=west,inner sep=0bp] at (1,{y+5.27:.3f}) {{{row["case_id"].split("_")[0]}}};')
        for j,cell in enumerate(row['cells']):
            x=41+j*112;outcome=cell['outcome']
            fill,sign=('F258MidLow','+') if outcome.startswith('positive') else (
                ('F258MidHigh',r'$-$') if outcome.startswith('negative') else ('F258Region','='))
            nv,nf=cell['native'];cv,cf=cell['control']
            lines.extend([rf'\fill[{fill}] ({x},{y:.3f}) rectangle ({x+112},{y+10.54:.3f});',
                rf'\node[inner sep=0bp] at ({x+13},{y+5.27:.3f}) {{{sign}}};',
                rf'\node[anchor=east,inner sep=0bp] at ({x+53},{y+5.27:.3f}) {{{nv}/{nf}}};',
                rf'\node[inner sep=0bp] at ({x+61},{y+5.27:.3f}) {{:}};',
                rf'\node[anchor=east,inner sep=0bp] at ({x+96},{y+5.27:.3f}) {{{cv}/{cf}}};'])
    for x in (41,153,265,377):
        lines.append(rf'\draw[line width=.33432bp] ({x},18) -- ({x},144.48);')
    lines.extend([
        r'\draw[line width=.33432bp] (41,18) -- (377,18);',
        r'\draw[line width=.33432bp] (41,144.48) -- (377,144.48);',
        r'\node[anchor=base west,inner sep=0bp,font=\fontencoding{T1}\fontfamily{ptm}\fontsize{7.3bp}{9bp}\selectfont] at (0,158) {Native V/F : no-internal V/F.\quad + benefit; $-$ disadvantage; = tie/other.};',
        rf'\node[anchor=base west,inner sep=0bp,font=\fontencoding{{T1}}\fontfamily{{ptm}}\fontsize{{7.3bp}}{{9bp}}\selectfont] at (0,170) {{Recovery: native {counts["positive_warning_recovery"]} / control {counts["negative_lost_version_hit"]}; report reduction: native {counts["positive_fixed_warning_reduction"]} / control {counts["negative_more_fixed_warnings"]}; ties/other {counts["tie_or_other"]}.}};',
        r'\end{tikzpicture}%'])
    output.write_text('\n'.join(lines)+'\n')
    output.with_suffix('.json').write_text(json.dumps(dict(source_file=summary.name,
        source_sha256=hashlib.sha256(raw).hexdigest(),rows=rows,outcome_counts=dict(counts)),indent=2)+'\n')


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--summary',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();build(args.summary,args.output)
