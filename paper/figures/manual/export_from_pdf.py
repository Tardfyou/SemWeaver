"""Crop author-exported PDF slides without re-rendering or changing typography.

Requires pypdf==6.1.1. The default input is the retained three-page source PDF.
No PowerPoint rendering, font substitution, model calls or diagram generation.
"""
import argparse
import hashlib
import json
from pathlib import Path
from pypdf import PdfReader, PdfWriter
from pypdf.generic import RectangleObject

FIGURES=[('motivating-example',[231.0,133.5,717.0,382.5]),
         ('architecture',[9.0,42.0,680.0,335.0]),
         ('study-design',[143.5,49.5,877.0,457.0])]


def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    here=Path(__file__).resolve().parent
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=here/'source-slides.pdf')
    parser.add_argument('--pages',default='1,2,3')
    parser.add_argument('--out',type=Path,default=here)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    indices=[int(x)-1 for x in args.pages.split(',')];assert len(indices)==3
    source=PdfReader(args.source);selected=PdfWriter()
    for index in indices:
        page=source.pages[index]
        assert abs(float(page.mediabox.width)-960)<1 and abs(float(page.mediabox.height)-540)<1
        selected.add_page(page)
        if '/Annots' in selected.pages[-1]:selected.pages[-1].annotations=None
    selected.add_metadata({'/Title':'SemWeaver author-exported source slides','/Author':''})
    selected_path=args.out/'source-slides.pdf'
    # Do not replace the input while reading it on an ordinary reproducibility run.
    if args.source.resolve()!=selected_path.resolve():
        with selected_path.open('wb') as handle:selected.write(handle)
    rows=[]
    for i,(name,crop) in enumerate(FIGURES):
        reader=PdfReader(selected_path);page=reader.pages[i]
        x0,top,x1,bottom=crop;h=float(page.mediabox.height)
        bounds=RectangleObject([x0,h-bottom,x1,h-top])
        page.cropbox=bounds;page.mediabox=bounds
        page.trimbox=bounds;page.artbox=bounds
        writer=PdfWriter();writer.add_page(page)
        writer.add_metadata({'/Title':'SemWeaver '+name,'/Author':''})
        path=args.out/(name+'.pdf')
        with path.open('wb') as handle:writer.write(handle)
        rows.append(dict(name=name,source_slide=i+1,crop_top_left=crop,
                         width_bp=x1-x0,height_bp=bottom-top,sha256=sha(path)))
    manifest=dict(source_pdf_sha256=sha(selected_path),figures=rows,
        operation='Crop boxes only; author text, objects, colors and embedded fonts retained.',
        source_description='Mac PowerPoint export; SemWeaver section, original deck pages 6-8.')
    (args.out/'EXPORT_MANIFEST.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest))


if __name__=='__main__':main()
