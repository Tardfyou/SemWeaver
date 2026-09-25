# 检测器精炼报告

**生成时间**: 20260424_101732
**分析器模式**: csa
**状态**: ✅ 精炼后命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=520.7s(agent) | tokens=207936
- CSA 阶段耗时: agent=209.0s, validation=51.7s, first_action=0.0s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 3 | validation_outcome @ semantic_validation: semantic validation passed with 3 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: MalformedHeaderFieldRangeChecker
- **迭代次数**: 2
- **精炼尝试**: 3 轮
- **精炼采纳**: ✅ 已采纳新产物
- **证据数量**: 10
- **语义切片**: 4
- **切片覆盖**: full
- **合成目标模式**: unknown
- **已选语义切片**: 4
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 3
- **Warning 数量**: 3

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/coders/psd.c:1959 - Header-derived dimension field stored into an image-like header object is later used without both rejecting zero and bounding the upper range via a barrier guard [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/coders/psd.c:1967 - Header-derived dimension field stored into an image-like header object is later used without both rejecting zero and bounding the upper range via a barrier guard [custom.MalformedHeaderFieldRangeChecker]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_imagemagick_cve20169556/vulnerable/coders/rla.c:219 - Header-derived dimension field stored into an image-like header object is later used without both rejecting zero and bounding the upper range via a barrier guard [custom.MalformedHeaderFieldRangeChecker]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 3 diagnostics [coverage=full]

## CodeQL

- **生成状态**: ❌ 失败
- **查询名称**: N/A

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/v2_experiments/runs/vul4c_cwe119_imagemagick_cve20169556/primary/csa/20260423_102141/csa/refinements/20260424_100851/csa`
- CodeQL 目录: `codeql`
- PATCHWEAVER 计划: `patchweaver_plan.json`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`

## PATCHWEAVER

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact
- **推荐分析器**: csa, codeql
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 4 diagnostics [coverage=full]
  - [csa] phase=refine_candidate: - validation_outcome @ semantic_validation: semantic validation passed with 4 diagnostics [coverage=full]
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 3 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: MalformedHeaderFieldRangeChecker
- **决策置信度**: high
- **决策摘要**: 首选 csa，主模式 unknown，置信度 high，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=97.0, accepted=Y, semantic=Y, evidence=10, missing=0, degraded=N
