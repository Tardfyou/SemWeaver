# 检测器精炼报告

**生成时间**: 20260424_115555
**分析器模式**: csa
**状态**: ⚠️ 保持当前产物；本轮精炼未产生可采纳更新

---

## 总览

- 运行摘要: csa=22.9s(validation) | tokens=23450
- CSA 阶段耗时: agent=2.5s, validation=11.8s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ⚠️ 仅执行成功 | 0 | validation_outcome @ semantic_validation: semantic validation passed with 140 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ⚠️ 仅执行成功
- **功能验证摘要**: 功能验证执行成功，但未命中验证目标
- **检测器名称**: UncheckedNegativeCountToSizeChecker
- **迭代次数**: 0
- **精炼尝试**: 1 轮
- **精炼采纳**: ⚠️ 未采纳，当前保持原产物
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 0
- **Warning 数量**: 0

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 140 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe190_binutils_cve201714745/primary/csa/20260423_133731/csa/refinements/20260424_115532/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 140 diagnostics [coverage=full]
  - [csa] phase=baseline_reused_validation: - validation_outcome @ semantic_validation: semantic validation passed with 140 diagnostics [coverage=full]

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=17.0, accepted=N, semantic=N, evidence=0, missing=0, degraded=N

## 错误信息

```
检测器已生成，但未通过功能验证
```
