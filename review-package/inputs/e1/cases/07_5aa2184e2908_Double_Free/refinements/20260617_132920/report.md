# 检测器精炼报告

**生成时间**: 20260617_133039
**分析器模式**: csa
**状态**: ⚠️ 保持当前产物；本轮精炼未产生可采纳更新
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=79.0s(agent) | tokens=43914
- CSA 阶段耗时: agent=68.8s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 10 | Baseline validation (csa): Knighter基线扫描报告数=2644, 人工确认bug=0, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patc |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **基线来源**: KNighter old-version canonical valid checker
- **Knighter 基线统计**: reports_before=2644, manual_bugs=0, manual_not_bugs=0, group=positive_scan_report_count
- **指标口径说明**: patch-local semantic validation (buggy-object hit / fixed-object silence) is a local validity gate and not the same metric as Knighter full-kernel report_count/FPR
- **检测器名称**: SAGenTestChecker
- **迭代次数**: 0
- **精炼尝试**: 1 轮
- **精炼采纳**: ⚠️ 未采纳，当前保持原产物
- **最近候选失败**: [Errno -3] Temporary failure in name resolution
- **证据数量**: 2
- **语义切片**: 0
- **验证反馈数量**: 0
- **证据反馈成效**: 验证反馈未新增证据或缺口变化
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 10
- **Warning 数量**: 10

### CSA 验证诊断（最多10条）
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 1/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 2/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 3/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 4/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 5/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 6/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 7/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 8/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 9/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o
- [warning] drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o:0 - KNighter E2 buggy patch-local scan reported custom checker alert 10/10 on drivers/net/ethernet/mellanox/mlx5/core/steering/hws/mlx5hws_definer.o

### CSA 验证反馈
- Baseline validation (csa): Knighter基线扫描报告数=2644, 人工确认bug=0, 人工非bug=0, 候选组=positive_scan_report_count。漏洞版命中=true, 漏洞版patch-scoped告警数=10, 修复版误报数=9, fixed_silent=false, PDS=false. 这些数只表示patch-local语义验证门，不等同于Knighter full-kernel report_count/FPR统计。

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/07_5aa2184e2908_Double_Free/csa/refinements/20260617_132920/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## Portfolio

- **首选分析器**: csa
- **首选产物**: SAGenTestChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=63.6, accepted=Y, semantic=Y, evidence=2, missing=0, degraded=N
