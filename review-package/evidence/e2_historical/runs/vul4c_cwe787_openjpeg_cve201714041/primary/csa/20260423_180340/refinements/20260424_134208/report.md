# 检测器精炼报告

**生成时间**: 20260424_134253
**分析器模式**: csa
**状态**: ❌ 精炼失败

---

## 总览

- 运行摘要: csa=44.2s(agent) | tokens=45858
- CSA 阶段耗时: agent=40.4s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ❌ | ❌ 未生成 | - | 生成失败 |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **功能验证**: ❌ 未生成
- **功能验证摘要**: 生成失败
- **检测器名称**: FormattedInputWidthBoundChecker
- **迭代次数**: 2
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 11
- **语义切片**: 4
- **切片覆盖**: full
- **合成目标模式**: buffer_overflow
- **已选语义切片**: 4
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe787_openjpeg_cve201714041/primary/csa/20260423_180340/csa/refinements/20260424_134208/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests stack_overflow semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=3, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 29 diagnostics [coverage=full]
  - [csa] phase=baseline_reused_validation: - validation_outcome @ semantic_validation: semantic validation passed with 29 diagnostics [coverage=full]
  - [csa] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic validation passed with 29 diagnostics [coverage=full]

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。

### 候选排序
- csa: score=-87.2, accepted=N, semantic=N, evidence=11, missing=0, degraded=N

## 错误信息

```
结构审查或编译未通过
```
