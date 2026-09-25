# 检测器精炼报告

**生成时间**: 20260424_132947
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=372.2s(agent) | tokens=99776
- CSA 阶段耗时: agent=314.2s, validation=54.3s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 1 | validation_outcome @ semantic_validation: semantic validation passed with 1 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: NullDerefChecker
- **迭代次数**: 4
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 10
- **语义切片**: 4
- **切片覆盖**: full
- **合成目标模式**: unknown
- **已选语义切片**: 4
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 1
- **Warning 数量**: 1

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_binutils_cve201714940/vulnerable/bfd/dwarf2.c:3213 - DW_AT_location block-form attribute data is dereferenced before the DWARF block payload pointer is proven non-null [custom.NullDerefChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 1 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe476_binutils_cve201714940/primary/csa/20260423_171100/csa/refinements/20260424_132335/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=1, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=baseline_reused_validation: - validation_outcome @ semantic_validation: semantic validation passed with 132 diagnostics [coverage=full]
  - [csa] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic validation passed with 132 diagnostics [coverage=full]
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 1 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: NullDerefChecker
- **决策置信度**: high
- **决策摘要**: 首选 csa，主模式 unknown，置信度 high，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=97.0, accepted=Y, semantic=Y, evidence=10, missing=0, degraded=N
