# 检测器精炼报告

**生成时间**: 20260617_103805
**分析器模式**: csa
**状态**: ❌ 精炼失败

---

## 总览

- 运行摘要: csa=315.4s(agent) | tokens=64432
- CSA 阶段耗时: agent=302.7s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ❌ | ❌ 未生成 | - | 生成失败 |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **功能验证**: ❌ 未生成
- **功能验证摘要**: 生成失败
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 3
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 13
- **语义切片**: 4
- **切片覆盖**: partial
- **合成目标模式**: uninitialized_data_use
- **已选语义切片**: 4
- **修复指令**: 1
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/05_c48a4497356f_Uninit_Data/csa/refinements/20260617_103250/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests uninitialized_data_use semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=5, metadata refs=1.
- **计划证据**: patch_fact, dataflow_candidate, call_chain
- **推荐分析器**: codeql
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=knighter_e2_patch_local_baseline: Baseline validation (csa): Knighter基线扫描报告数=5, 人工确认bug=1, 人工非bug=1, 候选组=local_triage_manual_fp。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=baseline_reused_validation: Baseline validation (csa): Knighter基线扫描报告数=5, 人工确认bug=1, 人工非bug=1, 候选组=local_triage_manual_fp。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=refine_candidate: Baseline validation (csa): Knighter基线扫描报告数=5, 人工确认bug=1, 人工非bug=1, 候选组=local_triage_manual_fp。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。

### 候选排序
- csa: score=-98.6, accepted=N, semantic=N, evidence=13, missing=0, degraded=N

## 错误信息

```
模型未返回可解析的 JSON。
```
