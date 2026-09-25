# 检测器生成报告

**生成时间**: 20260423_211652
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=327.9s(agent) | tokens=59096
- CSA 阶段耗时: agent=324.1s, validation=3.6s, first_action=316.5s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 2 | validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: UnboundedFormattedWriteChecker
- **迭代次数**: 3
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 2
- **Warning 数量**: 2

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/git_cwe120_xrdp_cve202223468/vulnerable/xrdp/xrdp_login_wnd.c:725 - Unbounded formatted write into a local array via sprintf-like API; use a size-bounded formatting function [custom.UnboundedFormattedWriteChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/git_cwe120_xrdp_cve202223468/vulnerable/xrdp/xrdp_login_wnd.c:731 - Unbounded formatted write into a local array via sprintf-like API; use a size-bounded formatting function [custom.UnboundedFormattedWriteChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **首选分析器**: csa
- **首选产物**: UnboundedFormattedWriteChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
