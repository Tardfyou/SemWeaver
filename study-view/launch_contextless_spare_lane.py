"""Use an idle kernel lane so long GLM replies do not block main repairs."""
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parent
original=ROOT/'launch_contextless_repair.py'

if __name__=='__main__':
    text=original.read_text()
    assert text.count("linux-e4-repeat-gpt-v10")==1
    text=text.replace('linux-e4-repeat-gpt-v10','linux-e4-repeat-glmflash-v21')
    text=text.replace(".slot-2.lock",".contextless-spare-kernel.lock")
    text=text.replace("additions = [Path(__file__),", "additions = [ROOT/'launch_contextless_spare_lane.py', Path(__file__),")
    exec(compile(text,str(original)+'[independent-repair-lane]','exec'),
         {'__name__':'__main__','__file__':str(original)})
