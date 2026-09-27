#!/usr/bin/env python3
"""Render an editable three-panel scientific figure matching the original layout."""
import argparse
import hashlib
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyBboxPatch, Circle, FancyArrowPatch

W,H=1160,620
ORANGE='#e59b00'; TEAL='#008e64'; PURPLE='#79238f'; GRAY='#92989e'

def render(manifest_path, output):
    data=json.loads(manifest_path.read_text())
    candidate=Path(data['selected_candidate'])
    if not candidate.is_file():
        candidate=manifest_path.parent/'runs/quality_r4/SAGenTestChecker.cpp'
    if hashlib.sha256(candidate.read_bytes()).hexdigest()!=data['candidate_sha256']:
        raise ValueError('selected checker changed')
    after=data['paired_after']
    if not(after['execution_valid'] and after['pds']):
        raise ValueError('figure requires a validated paired result')
    output.mkdir(parents=True,exist_ok=True)
    plt.rcParams.update({'svg.fonttype':'none','pdf.fonttype':42,'font.family':'DejaVu Sans'})
    fig=plt.figure(figsize=(W/72,H/72)); ax=fig.add_axes([0,0,1,1])
    ax.set_xlim(0,W);ax.set_ylim(H,0);ax.axis('off')
    def text(x,y,s,size=11,color='#222222',bold=False,mono=False,ha='left'):
        return ax.text(x,y,s,fontsize=size,color=color,weight='bold' if bold else 'normal',family='DejaVu Sans Mono' if mono else 'DejaVu Sans',va='center',ha=ha)
    def box(x,y,w,h):
        ax.add_patch(Rectangle((x,y),w,h,fill=False,linewidth=1.35,linestyle=(0,(3,2)),edgecolor='#222222'))
    def arrow(a,b,color,rad=0,dashed=False):
        ax.add_patch(FancyArrowPatch(a,b,arrowstyle='-|>',mutation_scale=12,linewidth=1.5,color=color,connectionstyle=f'arc3,rad={rad}',linestyle=(0,(3,2)) if dashed else '-'))
    def callout(n,x,y,label,width):
        ax.add_patch(FancyBboxPatch((x+12,y-11),width,22,boxstyle='round,pad=0.7,rounding_size=9',facecolor='#ffcf27',edgecolor='black',linewidth=1.25))
        ax.add_patch(Circle((x,y),10,facecolor='black',edgecolor='white',linewidth=0.7))
        text(x,y,str(n),15,'white',True,ha='center');text(x+20,y,label,16,'black',True)
    def code(lines,x,y,prefix,step=20,fontsize=11.0,colors=None):
        locations={}
        for i,s in enumerate(lines,1):
            yy=y+(i-1)*step;locations[i]=(x,yy)
            text(x-31,yy,f'{prefix}{i}',14,'#666666',mono=True)
            text(x,yy,s,fontsize,(colors or {}).get(i,'#444444'),mono=True)
        return locations

    text(16,18,'G21 / Linux commit 97cba232: off-by-one look-ahead access',20,bold=True)
    text(16,40,'A model-produced capacity check repairs a fixed-side false alarm',17)
    arrow((891,18),(923,18),'black');text(931,18,'control flow',9.7)
    arrow((891,38),(923,38),GRAY,dashed=True);text(931,38,'checker-to-code link',9.7)
    box(10,58,612,500);box(639,58,511,215);box(639,287,511,271)
    text(608,79,'#A. Patch and declared array extent',20,PURPLE,True,ha='right')
    text(1137,79,'#B. Before refinement',20,PURPLE,True,ha='right')
    text(1137,308,'#C. After automatic refinement',20,PURPLE,True,ha='right')

    a=[
      'enum { N = MAX_PIPES * 2 };  // figure alias',
      'struct dc_link *links[N];',
      '// get_host_router_total_dp_tunnel_bw(...)',
      '...',
      '- for (uint8_t i=0; i<N; ++i) {',
      '+ for (uint8_t i=0; i<N-1; ++i) {',
      '    if (!dc->links[i] || ...) continue;',
      '    ...',
      '    if (hr_index_temp == hr_index) {',
      '      primary = dc->links[i];',
      '      secondary=dc->links[i+1];',
      '      ...;',
      '      break;',
      '    }',
      '  }',
    ]
    ax.add_patch(Rectangle((47,174),384,20,facecolor='#fff1d2',edgecolor='none'))
    ax.add_patch(Rectangle((47,196),384,20,facecolor='#e3f5ed',edgecolor='none'))
    ax.add_patch(Rectangle((47,306),389,20,facecolor='#fff1d2',edgecolor='none'))
    code(a,49,96,'M',22,20,{5:'#c64123',6:TEAL,11:'#c64123'})
    callout(1,473,118,'capacity N',125)
    callout(2,464,184,'bound B = N',135)
    callout(4,464,206,'bound B = N-1',145)
    callout(3,464,317,'look-ahead i+1',145)
    arrow((598,186),(600,310),ORANGE,rad=-0.2)
    arrow((585,208),(580,309),TEAL,rad=0.16)
    text(38,490,'Vulnerable: max(i+1) = N       (out of bounds)',17,'#a56300',True,mono=True)
    text(38,526,'Fixed:      max(i+1) = N - 1   (in bounds)',17,TEAL,True,mono=True)

    b=['checkASTCodeBody(F):',
       '  for each recognized loop i < B:',
       '    find look-ahead access A[i + 1]',
       '    if no recognized body guard:',
       '      report(A[i + 1])',
       '  // Missing: compare loop reach',
       '  // with the declared capacity of A']
    code(b,679,106,'B',20,20,{5:'#b17500'})
    before=data['paired_before']
    text(660,255,f'Linux V/F: {before["vulnerable_alerts"]}/{before["fixed_alerts"]}',17,PURPLE,True,mono=True)
    callout(5,876,255,'missing bound proof',247)

    c=['checkASTCodeBody(F):  // retained',
       '  retain loop and access discovery',
       '  N = declared capacity of A',
       '  max_index = (B - 1) + 1',
       '  if max_index < N is established:',
       '    suppress the false alarm',
       '  otherwise retain the warning',
       '  // normalize supported guards/tests',
       '  // no patch-name or line allowlist']
    code(c,679,333,'C',20,20,{3:TEAL,4:TEAL,5:TEAL,6:TEAL})
    text(660,537,f'Linux V/F: {after["vulnerable_alerts"]}/{after["fixed_alerts"]}',17,PURPLE,True,mono=True)
    callout(6,876,537,'capacity-aware predicate',247)
    arrow((646,186),(618,317),ORANGE,rad=-0.13,dashed=True)
    arrow((645,373),(619,118),GRAY,rad=0.13,dashed=True)
    arrow((645,413),(618,207),TEAL,rad=0.06,dashed=True)

    text(16,585,'Solid arrows: patch control flow.  Dashed arrows: semantic links.  Checker panels are abridged logic.',16)
    text(16,607,'Frozen candidate + model trace + paired execution + perturbation results are supplied with the figure.',14,'#555555')
    for ext in ('pdf','svg','png'):
        fig.savefig(output/f'fig-motivating-g21.{ext}',dpi=220,facecolor='white')
    plt.close(fig)
    (output/'PANEL_TEXT.json').write_text(json.dumps({'A':a,'B':b,'C':c,'note':'Abbreviated logic; use the hash-bound full candidate for exact implementation.'},indent=2)+'\n')
    print(json.dumps({'output':str(output),'candidate_sha256':data['candidate_sha256']}))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--selection',required=True,type=Path);p.add_argument('--output-dir',required=True,type=Path)
    args=p.parse_args();render(args.selection,args.output_dir)
