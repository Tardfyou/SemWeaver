# 检测器精炼报告

**生成时间**: 20260617_142802
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=327.4s(agent) | tokens=64040
- CSA 阶段耗时: agent=235.5s, validation=79.1s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 1 | validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 3
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 7
- **语义切片**: 5
- **上下文摘要**: 1
- **切片覆盖**: unknown
- **合成目标模式**: unknown
- **已选语义切片**: 5
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

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/08_768f17fd25e4_Integer_Overflow/csa/refinements/20260617_142234/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=88.6, accepted=Y, semantic=Y, evidence=7, missing=0, degraded=N
