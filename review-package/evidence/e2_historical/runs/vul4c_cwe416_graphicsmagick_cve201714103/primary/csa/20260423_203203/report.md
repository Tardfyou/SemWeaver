# 检测器生成报告

**生成时间**: 20260423_203459
**分析器模式**: csa
**状态**: ❌ 生成失败

---

## 总览

- 运行摘要: csa=171.5s(agent) | tokens=66291
- CSA 阶段耗时: agent=171.3s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ❌ | ❌ 未生成 | - | 生成失败 |

## CSA (Clang Static Analyzer)

- **生成状态**: ❌ 失败
- **功能验证**: ❌ 未生成
- **功能验证摘要**: 生成失败
- **检测器名称**: UseAfterFreeChecker
- **迭代次数**: 3
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化

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

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。

### 候选排序
- csa: score=-138.0, accepted=N, semantic=N, evidence=0, missing=0, degraded=N

## 错误信息

```
模型未返回可解析的 JSON。
```
