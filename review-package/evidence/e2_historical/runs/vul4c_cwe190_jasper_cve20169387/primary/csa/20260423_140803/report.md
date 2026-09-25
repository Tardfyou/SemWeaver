# 检测器生成报告

**生成时间**: 20260423_141231
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: preflight=32.3s | csa=231.9s(agent) | tokens=79079
- CSA 阶段耗时: agent=222.1s, validation=9.6s, first_action=166.7s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 51 | validation_outcome @ semantic_validation: semantic validation passed with 51 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: IntegerOverflowChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: integer_overflow
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 51
- **Warning 数量**: 51

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_seq.c:321 - Value stored to 'data' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_seq.c:454 - Array access results in a null pointer dereference [core.NullDereference]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_icc.c:618 - Value stored to 'info' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_icc.c:1081 - Value stored to 'txtdesc' during its initialization is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_icc.c:1279 - Value stored to 'lut8' during its initialization is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_stream.c:333 - Value stored to 'openflags' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_stream.c:600 - Value stored to 'm' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_stream.c:612 - Value stored to 'm' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_image.c:1406 - Value stored to 'numinauxchans' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe190_jasper_cve20169387/vulnerable/src/libjasper/base/jas_image.c:1409 - Value stored to 'numoutchans' is never read [deadcode.DeadStores]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 51 diagnostics [coverage=full]

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

- **机制摘要**: Patch suggests integer_overflow semantics across 1 file(s); added guards=1, removed risky operations=0, fix patterns=4, metadata refs=0.
- **计划证据**: patch_fact, dataflow_candidate, semantic_slice, state_transition, call_chain
- **推荐分析器**: codeql, csa
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 51 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: IntegerOverflowChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 integer_overflow，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=55.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
