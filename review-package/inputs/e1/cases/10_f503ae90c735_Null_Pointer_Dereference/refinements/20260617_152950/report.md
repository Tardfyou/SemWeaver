# 检测器精炼报告

**生成时间**: 20260617_155413
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=819.1s(agent) | tokens=278014
- CSA 阶段耗时: agent=300.2s, validation=110.0s, manual_validation=129.579s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 2 | Manual E2 strict patch-local validation: buggy_alerts=2 fixed_alerts=0 |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: Manual E2 strict patch-local validation passed: buggy_alerts=2 fixed_alerts=0
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 6
- **精炼尝试**: 2 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 18
- **语义切片**: 9
- **上下文摘要**: 1
- **切片覆盖**: partial
- **合成目标模式**: unknown
- **已选语义切片**: 3
- **已选上下文摘要**: 1
- **修复指令**: 1
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: manual_e2_strict_patch_local
- **验证状态**: ✅ 成功
- **诊断数量**: 2
- **Warning 数量**: 2

### CSA 验证反馈
- Manual E2 refinement validation (csa): strict patch object buggy_alerts=2, fixed_alerts=0, buggy_hit=true, fixed_silent=true, PDS=true. Manual repair constrains the nullable mt76_connac_get_he_phy_cap return model to mt7996_mcu_sta_bfer_he, the patch function guarded by if (!vc), while preserving the strict mcu.o double-version validation gate.

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/10_f503ae90c735_Null_Pointer_Dereference/csa/refinements/20260617_152950/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=1.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=baseline_reused_validation: Baseline validation (csa): Knighter基线扫描报告数=14, 人工确认bug=4, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=5, 修复版误报数=3, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=refine_candidate: Baseline validation (csa): Knighter基线扫描报告数=14, 人工确认bug=4, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=5, 修复版误报数=3, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 5 diagnostics [coverage=full]
  - [csa] phase=manual_refine_validation: Manual E2 refinement validation (csa): strict patch object buggy_alerts=2, fixed_alerts=0, buggy_hit=true, fixed_silent=true, PDS=true. Manual repair constrains the nullable mt76_connac_get_he_phy_cap return model to mt7996_mcu_sta_bfer_he, the patch function guarded by if (!vc), while preserving the strict mcu.o double-version validation gate.

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 unknown，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=95.0, accepted=Y, semantic=Y, evidence=18, missing=0, degraded=N
