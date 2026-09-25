# 检测器精炼报告

**生成时间**: 20260617_164208
**分析器模式**: csa
**状态**: ⚠️ 保持当前产物；本轮精炼未产生可采纳更新
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=722.1s(agent) | tokens=198502
- CSA 阶段耗时: agent=393.5s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 2 | Baseline validation (csa): Knighter基线扫描报告数=1, 人工确认bug=1, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-s |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **基线来源**: KNighter old-version canonical valid checker
- **Knighter 基线统计**: reports_before=1, manual_bugs=1, manual_not_bugs=0, group=positive_scan_report_count
- **指标口径说明**: patch-local semantic validation (buggy-object hit / fixed-object silence) is a local validity gate and not the same metric as Knighter full-kernel report_count/FPR
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 0
- **精炼尝试**: 2 轮
- **精炼采纳**: ⚠️ 未采纳，当前保持原产物
- **最近候选失败**: 模型未返回可解析的 JSON。
- **证据数量**: 11
- **语义切片**: 0
- **验证反馈数量**: 0
- **证据反馈成效**: 新增 7 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 2
- **Warning 数量**: 2

### CSA 验证诊断（最多10条）
- [warning] drivers/firmware/arm_scmi/driver.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 1/2 on drivers/firmware/arm_scmi/driver.o
- [warning] drivers/firmware/arm_scmi/driver.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 2/2 on drivers/firmware/arm_scmi/driver.o

### CSA 验证反馈
- Baseline validation (csa): Knighter基线扫描报告数=1, 人工确认bug=1, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/19_39b13dce1a91_Double_Free/csa/refinements/20260617_163006/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests double_free semantics across 1 file(s); added guards=0, removed risky operations=0, fix patterns=3, metadata refs=1.
- **计划证据**: patch_fact, allocation_lifecycle, state_transition
- **推荐分析器**: csa
- **验证反馈历史**:
  - [csa] phase=knighter_e2_patch_local_baseline: Baseline validation (csa): Knighter基线扫描报告数=1, 人工确认bug=1, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=baseline_reused_validation: Baseline validation (csa): Knighter基线扫描报告数=1, 人工确认bug=1, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=2, 修复版误报数=1, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 double_free，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=90.8, accepted=Y, semantic=Y, evidence=11, missing=0, degraded=N


## E2 Strict Object Count Audit

- baseline: buggy_alerts=2, fixed_alerts=1
- refined automatic: buggy_alerts=0, fixed_alerts=0
- result: buggy miss; PDS=false
- note: per user instruction, latest automatic refine checker is retained without further manual repair.
