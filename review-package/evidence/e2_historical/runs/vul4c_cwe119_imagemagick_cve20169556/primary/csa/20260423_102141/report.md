# 检测器生成报告

**生成时间**: 20260423_103014
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: preflight=12.3s | csa=493.6s(validation) | tokens=78755
- CSA 阶段耗时: agent=138.3s, validation=355.0s, first_action=114.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 6801 | validation_outcome @ semantic_validation: semantic validation passed with 6801 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: MalformedHeaderFieldRangeChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 6801
- **Warning 数量**: 6801

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:322 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:362 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:365 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:372 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:373 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:374 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:376 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:378 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:384 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/MagickCore/annotate.c:386 - Sensitive arithmetic or control expression uses a header-derived field without a preceding semantic range check [custom.MalformedHeaderFieldRangeChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 6801 diagnostics [coverage=full]

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

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 6801 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: MalformedHeaderFieldRangeChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 unknown，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=55.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
