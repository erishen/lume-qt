# 样例 DICOM 数据的来源与再分发说明

> 状态：**来源与许可未确认，因此不随本仓库或任何对外分享的导出包分发。**

## 本机数据的位置与范围

开发机上 `resources/dicom/brain-t1/` 有一份 384 张、约 78 MB 的 MRI 序列（`N2D_0001.dcm` 起）。该目录由 [`.gitignore`](../.gitignore) 的 `/resources/dicom/` 规则排除，**从未入库、从未 push**；`git archive HEAD` 生成的干净导出包也不含它。缺少这份序列时，DICOM 演示与相关单测会跳过，不影响其余功能。

元数据实测（`dcmdump +L N2D_0001.dcm`）：

| 标签 | 值 |
|---|---|
| PatientName | `Jane_Doe` |
| PatientID | `02` |
| PatientBirthDate / PatientSex / PatientAge | `19660101` / `F` / `42` |
| StudyDescription | `Hanke_Stadler^0024_transrep` |
| Manufacturer / ManufacturerModelName | `BIOLAB` / `nifti2dicom` |
| InstitutionName / AccessionNumber | 空 |
| SOP/Study/Series Instance UID | 前缀 `1.2.826.0.1.3680043.2.1143.*` |

这些是**占位式、去标识**的取值（`Jane_Doe`、`nifti2dicom` 转换工具标记），可以判断不是真实患者 PHI。但「看起来是假人」**不等于**「可以再分发」：文件名、StudyDescription 与 UID 前缀指向某个外部数据集/转换流程，仓库里没有记录其来源、作者、许可条款或引用要求；UID 根的归属也未核实。

## 因此采取的规则

1. **不打包**：干净导出包、`git archive`、截图/演示材料均不包含该序列。
2. **不宣称授权**：本文不声明该数据属于公有领域、CC 或任何开放许可，也不为其背书。
3. **自备**：需要跑 DICOM 演示时，自行把一份**有明确许可**的序列放到 `resources/dicom/brain-t1/`，并在此处补记来源、许可与必要的署名。
4. **若要公开使用该序列**：先向来源方确认再分发条款与署名要求，补写本文的「来源 / 许可 / 引用」小节，并重新评估是否含 PHI；无法确认时，改用自建合成 DICOM 或明确授权的公开数据集（如各机构发布的 teaching phantom）。

## 待补字段

- 来源 URL / 数据集名称 / 版本：
- 作者与版权主体：
- 许可名称与许可文本链接：
- 要求的署名格式：
- 是否已去标识、由谁确认：

## 相关文档

- 第三方组件许可：[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)
- 隐私与合规审计（含对外分享审计）：[`privacy-compliance.md`](privacy-compliance.md)
