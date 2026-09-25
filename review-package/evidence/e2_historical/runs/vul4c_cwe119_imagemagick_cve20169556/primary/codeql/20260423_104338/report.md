# 检测器生成报告

**生成时间**: 20260423_104620
**分析器模式**: codeql
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: codeql

---

## 总览

- 运行摘要: preflight=9.2s | codeql=147.9s(agent) | tokens=69478
- CodeQL 阶段耗时: agent=87.7s, validation=59.9s, first_action=45.7s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CodeQL | ✅ | ✅ 命中目标 | 2 | validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **检测器名称**: N/A
- **迭代次数**: 0

## CodeQL

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **查询名称**: SgiHeaderDimensionRangeValidation
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证状态**: ✅ 成功
- **诊断数量**: 2

### CodeQL 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact, dataflow_candidate, semantic_slice, call_chain
- **推荐分析器**: codeql, csa
- **验证反馈历史**:
  - [codeql] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: codeql
- **首选产物**: SgiHeaderDimensionRangeValidation
- **决策置信度**: medium
- **决策摘要**: 首选 codeql，主模式 unknown，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: codeql
- **组合建议**:
  - CodeQL 适合跨函数、跨文件的数据流和 API 语义扩展。

### 候选排序
- codeql: score=55.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
