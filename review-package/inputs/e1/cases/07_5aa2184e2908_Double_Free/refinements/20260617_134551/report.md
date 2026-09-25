# 检测器精炼报告

**生成时间**: 20260617_140340  
**分析器模式**: csa  
**状态**: 精炼成功（手工补全最后精炼产物）

## 总览

- 基线 patch-local: buggy=10, fixed=9, fixed_silent=false, PDS=false
- 精炼后 patch-local: buggy=1, fixed=0, fixed_silent=true, PDS=true
- 验证日志: `/anonymous/home/LLM-Native/research/knighter/runs/knighter-v613/validation_20260617_140340/knighter_validation.log`
- 手工验证结果: `research/knighter/e2/cases/07_5aa2184e2908_Double_Free/manual_validation/manual_validation_result.json`

## CSA

- 检测器: `SAGenTestChecker`
- 精炼产物: `/anonymous/home/LLM-Native/research/knighter/e2/cases/07_5aa2184e2908_Double_Free/csa/refinements/20260617_134551/csa/SAGenTestChecker.cpp`
- 语义建模: `hws_definer_conv_match_params_to_hl()` 失败时，`mt->fc` 尚未建立，不能跳入 `free_fc` 释放成员字段；修复版跳到 `free_match_hl` 后静默。
- 验证状态: target_hit
- 诊断数: 1

## 产物位置

- CSA 目录: `/anonymous/home/LLM-Native/research/knighter/e2/cases/07_5aa2184e2908_Double_Free/csa/refinements/20260617_134551/csa`
- 验证反馈: `validation_feedback.json`
- 整合报告: `final_report.json`
