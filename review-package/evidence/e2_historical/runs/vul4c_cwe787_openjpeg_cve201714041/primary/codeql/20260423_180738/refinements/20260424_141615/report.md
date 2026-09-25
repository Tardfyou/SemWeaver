# 检测器精炼报告

**生成时间**: 20260424_141800
**分析器模式**: codeql
**状态**: ❌ 精炼失败

---

## 总览

- 运行摘要: codeql=105.2s(agent) | tokens=84224
- CodeQL 阶段耗时: agent=101.3s, first_action=0.0s

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
- **查询名称**: ScanfUnboundedScansetInConvert
- **精炼尝试**: 1 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe787_openjpeg_cve201714041/primary/codeql/20260423_180738/codeql/refinements/20260424_141615/codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **决策置信度**: low
- **决策摘要**: 没有检测器通过功能验证
- **组合建议**:
  - 当前没有检测器通过功能验证。

### 候选排序
- codeql: score=-135.0, accepted=N, semantic=N, evidence=0, missing=0, degraded=N

## 错误信息

```
CodeQL 本地检查或审查未通过
```
