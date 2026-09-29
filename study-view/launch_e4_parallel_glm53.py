"""Execute the original planned GLM53 cells on an independent kernel lane."""
from pathlib import Path

ROOT=Path(__file__).resolve().parent
original=ROOT/'launch_e4_parallel_luna.py'

if __name__=='__main__':
    source=original.read_text()
    source=source.replace("p['model']=='gpt-6-luna'","p['model']=='glm-5.3'")
    source=source.replace('linux-e4-repeat-gpt-recovery-v10','linux-e4-glm53-v11-clean')
    source=source.replace('.e4-parallel-luna-kernel.lock','.e4-parallel-glm53-kernel.lock')
    source=source.replace('PARALLEL_LUNA_PLAN.json','PARALLEL_GLM53_PLAN.json')
    source=source.replace("str(ROOT/'resume_verified_queues.py'):module.sha(ROOT/'resume_verified_queues.py')}",
                           "str(ROOT/'resume_verified_queues.py'):module.sha(ROOT/'resume_verified_queues.py'),str(original):module.sha(original)}")
    exec(compile(source,str(original)+'[original-GLM53-cells]','exec'),
         {'__name__':'__main__','__file__':str(Path(__file__).resolve()),'original':original})
