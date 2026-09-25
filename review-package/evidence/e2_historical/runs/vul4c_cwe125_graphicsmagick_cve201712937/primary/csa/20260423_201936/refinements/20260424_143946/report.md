# 检测器精炼报告

**生成时间**: 20260424_144701
**分析器模式**: csa
**状态**: ⚠️ 精炼已执行，但未通过功能验证

---

## 总览

- 运行摘要: csa=434.5s(agent) | tokens=101274
- CSA 阶段耗时: agent=392.8s, validation=37.9s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ❌ 执行失败 | 0 | validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ❌ 执行失败
- **功能验证摘要**: 功能验证执行失败
- **检测器名称**: ColormapIndexBoundsChecker
- **迭代次数**: 4
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 10
- **语义切片**: 4
- **切片覆盖**: partial
- **合成目标模式**: out_of_bounds_read
- **已选语义切片**: 4
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ❌ 失败
- **诊断数量**: 0
- **Warning 数量**: 0

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe125_graphicsmagick_cve201712937/primary/csa/20260423_201936/csa/refinements/20260424_143946/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests out_of_bounds_read semantics across 1 file(s); added guards=0, removed risky operations=0, fix patterns=3, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=baseline_reused_validation: - validation_outcome @ semantic_validation: semantic validation passed with 12 diagnostics [coverage=full]
  - [csa] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic validation passed with 12 diagnostics [coverage=full]
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed]

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=51.0, accepted=N, semantic=N, evidence=10, missing=0, degraded=N

## 错误信息

```
检测器已生成，但未通过功能验证
```
