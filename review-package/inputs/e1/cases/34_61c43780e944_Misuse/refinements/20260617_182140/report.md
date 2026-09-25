# 检测器精炼报告

**生成时间**: 20260617_182754
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=374.2s(agent) | tokens=120612
- CSA 阶段耗时: agent=266.3s, validation=97.1s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 1 | validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 6
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 18
- **语义切片**: 9
- **上下文摘要**: 1
- **切片覆盖**: partial
- **合成目标模式**: api_misuse
- **已选语义切片**: 3
- **已选上下文摘要**: 1
- **修复指令**: 1
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 0
- **Warning 数量**: 0

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/34_61c43780e944_Misuse/csa/refinements/20260617_182140/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests api_misuse semantics across 1 file(s); added guards=0, removed risky operations=0, fix patterns=3, metadata refs=2.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=baseline_reused_validation: Baseline validation (csa): Knighter基线扫描报告数=0, 人工确认bug=0, 人工非bug=0, 候选组=valid_no_local_report。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=refine_candidate: Baseline validation (csa): Knighter基线扫描报告数=0, 人工确认bug=0, 人工非bug=0, 候选组=valid_no_local_report。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 api_misuse，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=95.0, accepted=Y, semantic=Y, evidence=18, missing=0, degraded=N


## E2 Strict Patch-Local Object Counts

- object: `net/devlink/port.o`
- baseline: buggy_alerts=2, fixed_alerts=1
- refined: buggy_alerts=1, fixed_alerts=0
- buggy_hit: true
- fixed_silent: true
- PDS: true
- validation_dir: `/anonymous/home/LLM-Native/research/knighter/runs/knighter-v613/validation_20260617_182857`
- validation_log: `/anonymous/home/LLM-Native/research/knighter/runs/knighter-v613/validation_20260617_182857/knighter_validation.log`

Final E2 counts are parsed from Knighter `scan-build ... make LLVM=1 ARCH=x86 net/devlink/port.o` results in `knighter_validation.log`, not HTML pages and not v2 filtered diagnostic rows. The adopted strict log has no analyzer failure markers.
