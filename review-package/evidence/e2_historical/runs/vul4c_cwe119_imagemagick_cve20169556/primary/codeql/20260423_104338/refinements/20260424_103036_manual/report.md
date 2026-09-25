# 检测器精炼报告

**生成时间**: 20260424_102824
**分析器模式**: codeql
**状态**: ❌ 精炼失败

---

## 总览

- 运行摘要: codeql=499.9s(agent) | tokens=401364
- CodeQL 阶段耗时: agent=141.6s, validation=62.6s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CodeQL | ❌ | ❌ 未生成 | - | 生成失败 |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **检测器名称**: N/A
- **迭代次数**: 0

## CodeQL

- **生成状态**: ❌ 失败
- **功能验证**: ❌ 未生成
- **功能验证摘要**: 生成失败
- **查询名称**: SgiHeaderDimensionRangeValidation
- **精炼尝试**: 3 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 9
- **语义切片**: 3
- **切片覆盖**: partial
- **合成目标模式**: unknown
- **已选语义切片**: 3
- **修复指令**: 1
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe119_imagemagick_cve20169556/primary/codeql/20260423_104338/codeql/refinements/20260424_102004/codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [codeql] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic validation passed with 2 diagnostics [coverage=full]
  - [codeql] phase=initial_validation: - validation_outcome @ semantic_validation: semantic_no_hits [coverage=empty]
  - [codeql] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic_no_hits [coverage=empty]

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。

### 候选排序
- codeql: score=-97.8, accepted=N, semantic=N, evidence=9, missing=0, degraded=N

## 错误信息

```
模型未返回可解析的 JSON。
```
