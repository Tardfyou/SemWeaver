# 检测器生成报告

**生成时间**: 20260423_202341
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=240.9s(agent) | tokens=58012
- CSA 阶段耗时: agent=205.5s, validation=35.1s, first_action=192.5s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 12 | validation_outcome @ semantic_validation: semantic validation passed with 12 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: ColormapIndexBoundsChecker
- **迭代次数**: 3
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 12
- **Warning 数量**: 12

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:444 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:452 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:460 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:581 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:591 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:623 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:665 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:666 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:667 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe125_graphicsmagick_cve201712937/vulnerable/coders/sun.c:1045 - Array read from image->colormap uses an index without a preceding VerifyColormapIndex(image, index) check [custom.ColormapIndexBoundsChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 12 diagnostics [coverage=full]

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

- **首选分析器**: csa
- **首选产物**: ColormapIndexBoundsChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
