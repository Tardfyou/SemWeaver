# 检测器精炼报告

**生成时间**: 20260424_120029
**分析器模式**: codeql
**状态**: ⚠️ 精炼已执行，但未通过功能验证

---

## 总览

- 运行摘要: codeql=261.2s(agent) | tokens=240616
- CodeQL 阶段耗时: agent=103.2s, validation=3.4s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CodeQL | ✅ | ❌ 执行失败 | 0 | validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed] |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **检测器名称**: N/A
- **迭代次数**: 0

## CodeQL

- **生成状态**: ✅ 已生成
- **功能验证**: ❌ 执行失败
- **功能验证摘要**: 功能验证执行失败
- **查询名称**: NegativeCountToSizeConsumer
- **精炼尝试**: 3 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 8
- **语义切片**: 3
- **切片覆盖**: partial
- **合成目标模式**: unknown
- **已选语义切片**: 3
- **修复指令**: 1
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证状态**: ❌ 失败
- **诊断数量**: 0

### CodeQL 验证反馈
- validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed]

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe190_binutils_cve201714745/primary/codeql/20260423_134322/codeql/refinements/20260424_115608/codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [codeql] phase=initial_validation: - validation_outcome @ semantic_validation: semantic_no_hits [coverage=empty]
  - [codeql] phase=initial_validation: - validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed]
  - [codeql] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic_execution_error [coverage=failed]

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。
  - CodeQL 适合跨函数、跨文件的数据流和 API 语义扩展。

### 候选排序
- codeql: score=45.4, accepted=N, semantic=N, evidence=8, missing=0, degraded=N

## 错误信息

```
检测器已生成，但未通过功能验证
```
