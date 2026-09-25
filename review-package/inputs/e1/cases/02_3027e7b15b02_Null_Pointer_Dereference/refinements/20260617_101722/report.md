# 检测器精炼报告

**生成时间**: 20260617_102503
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=461.2s(agent) | tokens=105568
- CSA 阶段耗时: agent=341.8s, validation=102.9s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 1 | validation_outcome @ semantic_validation: semantic validation passed with 16 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 4
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 15
- **语义切片**: 7
- **切片覆盖**: partial
- **合成目标模式**: null_dereference
- **已选语义切片**: 4
- **修复指令**: 1
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 0
- **Warning 数量**: 0

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 16 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/02_3027e7b15b02_Null_Pointer_Dereference/csa/refinements/20260617_101722/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests null_dereference semantics across 1 file(s); added guards=2, removed risky operations=0, fix patterns=3, metadata refs=2.
- **计划证据**: patch_fact, dataflow_candidate, path_guard, call_chain
- **推荐分析器**: codeql, csa
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=baseline_reused_validation: Baseline validation (csa): Knighter基线扫描报告数=75, 人工确认bug=17, 人工非bug=12, 候选组=local_triage_manual_fp。漏洞版命中=true, 漏洞版patch-scoped告警数=19, 修复版误报数=2, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=refine_candidate: Baseline validation (csa): Knighter基线扫描报告数=75, 人工确认bug=17, 人工非bug=12, 候选组=local_triage_manual_fp。漏洞版命中=true, 漏洞版patch-scoped告警数=19, 修复版误报数=2, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 16 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 null_dereference，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=107.0, accepted=Y, semantic=Y, evidence=15, missing=0, degraded=N
