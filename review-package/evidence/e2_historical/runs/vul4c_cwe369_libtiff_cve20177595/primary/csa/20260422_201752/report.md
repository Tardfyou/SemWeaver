# 检测器生成报告

**生成时间**: 20260422_202130
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: preflight=12.5s | csa=200.1s(agent) | tokens=79818
- CSA 阶段耗时: agent=117.9s, validation=81.9s, first_action=96.8s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 105 | validation_outcome @ semantic_validation: semantic validation passed with 105 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: InvalidSamplingFactorValidationChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: divide_by_zero
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 105
- **Warning 数量**: 105

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_dirread.c:2730 - Dereference of null pointer [core.NullDereference]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_jpeg.c:1687 - Division or remainder uses sampling/size-like value derived from metadata without a prior zero-value rejection guard [custom.InvalidSamplingFactorValidationChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_jpeg.c:1693 - Division or remainder uses sampling/size-like value derived from metadata without a prior zero-value rejection guard [custom.InvalidSamplingFactorValidationChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_jpeg.c:1701 - Division or remainder uses sampling/size-like value derived from metadata without a prior zero-value rejection guard [custom.InvalidSamplingFactorValidationChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_luv.c:1272 - The result of the '/' expression is undefined [core.UndefinedBinaryOperatorResult]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_pixarlog.c:644 - The result of the '/' expression is undefined [core.UndefinedBinaryOperatorResult]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/libtiff/tif_predict.c:724 - Division or remainder uses sampling/size-like value derived from metadata without a prior zero-value rejection guard [custom.InvalidSamplingFactorValidationChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/tools/pal2rgb.c:167 - Although the value stored to 'rowsperstrip' is used in the enclosing expression, the value is never actually read from 'rowsperstrip' [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/tools/raw2tiff.c:258 - Value stored to 'photometric' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe369_libtiff_cve20177595/vulnerable/tools/tiff2pdf.c:751 - Array access (from variable 'argv') results in a null pointer dereference [core.NullDereference]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 105 diagnostics [coverage=full]

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

- **机制摘要**: Patch suggests divide_by_zero semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=2, metadata refs=0.
- **计划证据**: patch_fact, dataflow_candidate, call_chain
- **推荐分析器**: codeql
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 105 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: InvalidSamplingFactorValidationChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 divide_by_zero，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
