# 检测器生成报告

**生成时间**: 20260423_175559
**分析器模式**: csa
**状态**: ✅ 命中验证目标并通过功能验证
**首选分析器**: csa

---

## 总览

- 运行摘要: csa=288.1s(agent) | tokens=82374
- CSA 阶段耗时: agent=278.9s, validation=9.0s, first_action=251.3s

| 分析器 | 生成 | 验证状态 | 诊断数 | 关键提示 |
| --- | --- | --- | --- | --- |
| CSA | ✅ | ✅ 命中目标 | 52 | validation_outcome @ semantic_validation: semantic validation passed with 52 diagnostics [coverage=full] |

## CSA (Clang Static Analyzer)

- **生成状态**: ✅ 已生成
- **功能验证**: ✅ 命中目标
- **功能验证摘要**: 命中验证目标并通过功能验证
- **检测器名称**: EmptySequenceReferenceGuardChecker
- **迭代次数**: 4
- **证据数量**: 0
- **语义切片**: 0
- **切片覆盖**: missing
- **合成目标模式**: unknown
- **验证反馈数量**: 1
- **证据反馈成效**: 新增 1 条反馈后证据
- **验证阶段**: semantic
- **验证状态**: ✅ 成功
- **诊断数量**: 52
- **Warning 数量**: 52

### CSA 验证诊断（最多10条）
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_seq.c:303 - Value stored to 'data' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_seq.c:434 - Array access results in a null pointer dereference [core.NullDereference]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_icc.c:616 - Value stored to 'info' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_icc.c:1079 - Value stored to 'txtdesc' during its initialization is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_icc.c:1277 - Value stored to 'lut8' during its initialization is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_stream.c:330 - Value stored to 'openflags' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_stream.c:597 - Value stored to 'm' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_stream.c:609 - Value stored to 'm' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_image.c:1355 - Value stored to 'numinauxchans' is never read [deadcode.DeadStores]
- [warning] /anonymous/home/LLM-Native/v2_experiments/datasets/curated/vul4c_cwe476_jasper_cve201610248/vulnerable/src/libjasper/base/jas_image.c:1358 - Value stored to 'numoutchans' is never read [deadcode.DeadStores]

### CSA 验证反馈
- validation_outcome @ semantic_validation: semantic validation passed with 52 diagnostics [coverage=full]

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
- **首选产物**: EmptySequenceReferenceGuardChecker
- **决策置信度**: medium
- **决策摘要**: 首选 csa，置信度 medium，通过功能验证，且综合得分最高
- **推荐组合**: csa
- **组合建议**:
  - CSA 适合路径敏感、本地状态和生命周期约束验证。

### 候选排序
- csa: score=47.0, accepted=Y, semantic=Y, evidence=0, missing=0, degraded=N
