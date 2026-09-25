# 检测器生成报告

**生成时间**: 20260423_181032
**分析器模式**: codeql
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: codeql

---

## 总览

- 运行摘要: codeql=173.5s(agent) | tokens=70749
- CodeQL 阶段耗时: agent=162.7s, validation=10.8s, first_action=108.8s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CodeQL | ✅ | ✅ 命中目标 | 1 | validation_outcome @ semantic_validation: semantic validation passed with 1 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **检测器名称**: N/A
- **迭代次数**: 0

## CodeQL

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **查询名称**: ScanfUnboundedScansetInConvert
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证状态**: ✅ 成功
- **诊断数量**: 1

### CodeQL 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 1 diagnostics [coverage=full]

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **首选分析器**: codeql
- **首选产物**: ScanfUnboundedScansetInConvert
- **决策置信度**: medium
- **决策摘要**: 首选 codeql，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: codeql
- **组合建议**:
  - CodeQL 适合跨函数、跨文件的数据流和 API 语义扩展。

### 候选排序
- codeql: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
