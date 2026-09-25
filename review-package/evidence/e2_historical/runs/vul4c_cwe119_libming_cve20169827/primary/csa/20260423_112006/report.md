# 检测器生成报告

**生成时间**: 20260423_112229
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: preflight=12.4s | csa=127.3s(agent) | tokens=76344
- CSA 阶段耗时: agent=113.5s, validation=13.5s, first_action=85.9s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 108 | validation_outcome @ semantic_validation: semantic validation passed with 108 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: StringFieldRawReadMismatchChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 108
- **Warning 数量**: 108

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/displaylist.c:656 - Access to field 'onPlace' results in a dereference of a null pointer (loaded from variable 'character') [core.NullDereference]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/displaylist.c:702 - Access to field 'onPlace' results in a dereference of a null pointer (loaded from variable 'character') [core.NullDereference]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/videostream.c:284 - Value stored to 'ic' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/videostream.c:372 - Value stored to 'ichar' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/videostream.c:373 - Value stored to 'ichar' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/jpeg.c:131 - Although the value stored to 'c' is used in the enclosing expression, the value is never actually read from 'c' [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/jpeg.c:134 - Although the value stored to 'c' is used in the enclosing expression, the value is never actually read from 'c' [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/jpeg.c:486 - 2nd function call argument is an uninitialized value [core.CallAndMessage]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/mp3.c:164 - Division by zero [core.DivideZero]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe119_libming_cve20169827/vulnerable/src/blocks/mp3.c:168 - Division by zero [core.DivideZero]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 108 diagnostics [coverage=full]

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

- **机制摘要**: Patch suggests unknown semantics across 1 file(s); added guards=0, removed risky operations=0, fix patterns=0, metadata refs=0.
- **计划证据**: patch_fact, dataflow_candidate, semantic_slice, call_chain
- **推荐分析器**: codeql, csa
- **覆盖缺口**:
  - dataflow_candidate is not covered by selected analyzers ['csa']
  - call_chain is not covered by selected analyzers ['csa']
- **验证反馈历史**:
  - [csa] phase=initial_validation: - validation_outcome @ semantic_validation: semantic validation passed with 108 diagnostics [coverage=full]

## Portfolio

- **首选分析器**: csa
- **首选产物**: StringFieldRawReadMismatchChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，主模式 unknown，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=55.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
