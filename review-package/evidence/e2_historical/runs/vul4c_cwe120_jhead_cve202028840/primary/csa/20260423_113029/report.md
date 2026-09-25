# 检测器生成报告

**生成时间**: 20260423_113339
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: preflight=13.8s | csa=173.2s(agent) | tokens=86075
- CSA 阶段耗时: agent=169.8s, validation=3.1s, first_action=112.7s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 27 | validation_outcome @ semantic_validation: semantic validation passed with 27 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: OutOfBoundsLookaheadReadChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: out_of_bounds_read
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 27
- **Warning 数量**: 27

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:369 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:379 - Call to function 'mktemp' is insecure as it always creates or uses insecure temporary file.  Use 'mkstemp' instead [security.insecureAPI.mktemp]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:390 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:396 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:1617 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:1619 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:1624 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/jhead.c:1629 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/exif.c:1201 - Null pointer passed to 1st parameter expecting 'nonnull' [core.NonNullParamChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe120_jhead_cve202028840/vulnerable/exif.c:1551 - Array lookahead read uses index + constant offset without an explicit upper-bound guard proving the extra byte is within the buffer length [custom.OutOfBoundsLookaheadReadChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 27 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests out_of_bounds_read semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=4, metadata refs=0.
- **计划证据**: patch_fact, dataflow_candidate, call_chain
- **推荐分析器**: codeql
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 27 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: OutOfBoundsLookaheadReadChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 out_of_bounds_read，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
