# 检测器生成报告

**生成时间**: 20260423_191258
**分析器模式**: codeql
**状态**: ⚠️ 已生成但未通过功能验证

---

## 总览

- 运行摘要: codeql=142.5s(agent) | tokens=56978
- CodeQL 阶段耗时: agent=132.1s, validation=10.3s, first_action=112.2s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CodeQL | ✅ | ⚠️ 仅执行成功 | 0 | validation_outcome @ semantic_validation: semantic_no_hits [coverage=empty] |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **检测器名称**: N/A
- **迭代次数**: 0

## CodeQL

- **生成状态**: ✅ 已生成
- **功能验证**: ⚠️ 仅执行成功
- **功能验证摘要**: 功能验证执行成功，但未命中验证目标
- **查询名称**: EmptySequenceGetrefWithoutSizeGuard
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证状态**: ✅ 成功
- **诊断数量**: 0

### CodeQL 验证反馈
- validation_outcome @ semantic_validation: semantic_no_hits [coverage=empty]

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。
  - CodeQL 适合跨函数、跨文件的数据流和 API 语义扩展。

### 候选排序
- codeql: score=9.0, accepted=N, semantic=N, evidence=0, missing=0, degraded=N

## 错误信息

```
检测器已生成，但未通过功能验证
```
