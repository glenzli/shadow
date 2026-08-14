# Shadow AI 能力规划

本文档是为 Shadow 增加模型驱动能力的可执行路线图。它描述的并不是已经交付的推理能力。
当前已经实现的契约与应用集成仍记录在 [`README.md`](README.md) 中。

本方案是在现有的模型无关清单、资源/隐私准入、生成产物契约、反馈证据，以及可解释的组内相对评分基础上继续扩展。
它不会在 `shadow-ai` 旁边另建一套 AI 架构。

## 状态词汇

- **当前（Current）**：该契约或行为目前已存在于仓库中。
- **候选（Candidate）**：某个上游实现适合进入可度量的原型验证，但仍须通过许可与产物审查。
- **已规划（Planned）**：Shadow 已接受该边界，但尚未实现。
- **暂缓（Deferred）**：在所述阻塞条件发生变化之前，该选项不得进入产品。

本文档中提及的任何候选模型、provider、服务、延迟或质量结果，都不代表已经实现的事实。

## 已实现基础快照

仓库目前已经具备第一套 provider 中立的执行契约，但尚未具备模型驱动的推理：

- `AiJobRequest` 只表达应用意图，不再携带 provider、模型、checkpoint 或服务选择。
  准入流程会把该意图绑定到一组精确的本地产物、系统框架请求修订版，或远程服务身份。
  `AiObservation` 是 exact-v1 的结果信封，正常情况下只能通过消耗一次成功的 lease 输出构造；
  重复出现的 request/task/target 字段必须与运行时来源一致，解释证据最多包含 64 个信号。
- 一个仅可移动的运行时 lease 支持协作式取消、经校验且单调递增的进度、
  完整的 provider/model/framework/service 路线匹配、精确的完整计划匹配，以及一份终态回执。
  执行成功时，由被消耗的 lease（而非 provider）创建一个仅可移动的 payload 信封；
  该信封的来源绑定完整请求、输入清单、路线和执行计划。
  回执会记录该计划、资源用量，以及实际使用的是主路线，还是明确声明过的 fallback。
  应用调度器仍负责 lease 的唯一签发与撤销；这个可移植值不是身份认证令牌。
- 本地 manifest 标识一组完整的内容寻址产物。Core ML 包以可复现下载归档/blob 的形式进入该集合。
  对按规范路径排序后的清单执行域隔离、长度前缀化的 BLAKE3 摘要，所得结果就是集合身份；
  已安装的可用内容必须与之精确匹配。
  解压后的 `.mlpackage` 树、编译后的 `.mlmodelc` 以及设备专用化结果，都是可重建的运行时缓存，
  不是分发模型的身份。
  exact-v1 的 manifest/request/route/plan/provenance 解码会拒绝未知的嵌套字段、重复集合、
  不支持的版本和过大的流式向量。
  可移植包路径会在清单哈希前拒绝路径穿越、盘符/UNC/ADS 语法、保留设备名、首尾空格、
  短文件名中的 `~`、大小写别名，以及 Unicode 规范化歧义。
- 远程 provider manifest 与本地模型 manifest 相互独立。
  它们声明 rendered-RGB/蒙版上传范围、隐私上限、保留策略、训练用途、条款修订版、
  离线行为、幂等性和取消语义。
  已实现的路径首先要求应用存储层为经过净化和校验的出站对象签发回执，
  然后按出站字节而不是源字节计量。
  不透明的短期 grant 会绑定完整请求、manifest/法律事实、策略与同意修订版、
  精确的回执清单，以及稳定的幂等键。
  在做身份哈希以及生成 grant/transport 清单之前，回执都会按请求输入索引规范化。
  RAW 文件、传感器马赛克、场景线性 tile 和冻结的特征向量一律 fail closed。
  旧的本地 manifest 规划器不能授权 `RemoteApi`。
- 此 crate 仍然没有生成像素的持久化存储。
  已实现的提升事务只接受由 lease 签发的成功输出信封，
  并先针对精确字节调用应用拥有的托管存储授权，之后 Recipe 集成才能消费这些字节。
  托管授权仅可移动且不可反序列化；持久化描述符必须先通过存储校验，重载流程才能重建该授权。
- 第一版裁片边界会校验完整的 FeaturePrint 距离证据，执行确定性的贪心 complete-link 分组，
  并且只建议把相似度 medoid 作为第一张待审照片。
  它不会修改 Pick/Reject，也不会声称 medoid 是最清晰、最美观或其他意义上的“最佳”照片。
- Apple Vision FeaturePrint provider 的骨架固定到请求修订版 1 和 OS build。
  当前 Rust 构建并未链接 Vision，因此在 macOS 上会返回明确的 `adapter_not_linked` 终态
  （其他平台返回 `platform_unsupported`），绝不会伪造距离。

这些都是预发布阶段对 v1 契约的替换。仓库目前不存在持久化或跨进程的
`AiJobRequest`/`ModelManifest` 消费方；反序列化会递归校验精确版本，
拒绝未知字段与过大的清单，并在已删除的请求字段出现时 fail closed，而不是悄悄沿用过时的模型选择。

## 当前路线决策

| 能力 | 路线与开放性 | 部署/身份 | 当前决策 |
| --- | --- | --- | --- |
| 相似连拍分组 | Apple Vision FeaturePrint，闭源系统框架 | 固定 Vision 请求修订版 + OS build；不宣称拥有权重 | 首个要链接并进行基准测试的原生 adapter；只提供相似度证据 |
| 可提示蒙版 | Apple [SAM 2.1 Tiny Core ML](https://huggingface.co/apple/coreml-sam2.1-tiny)，模型卡为 Apache-2.0，float16 encoder/prompt/decoder 集合 | 精确的多归档产物集合；本地 Core ML | 在包注册表/运行时完成后，作为首个通用蒙版原型；Small 作为可选质量层级另行度量 |
| 系统前景蒙版 | Apple Vision 前景实例蒙版，闭源系统框架 | 固定请求/OS 身份 | 面向受支持场景的低成本比较路线，不作为通用 fallback |
| 本地模型对象移除 | LaMa 方法/源码开放；当前引用的 checkpoint 权利尚未厘清 | 侧载的本地原型 + 生成栅格来源 | 合成像素必须遵循与远程生成器相同的披露/提升契约；完成精确 checkpoint/数据审计前不得捆绑或自动下载 |
| 远程生成式填充 | Adobe 异步 Fill、BFL FLUX.1 Fill、Stability inpaint；闭源服务 | 独立的远程 manifest、明确上传渲染后 crop/蒙版、provider 请求回执 | 仅作为选择加入的比较项；绝不成为 Heal 的隐式 fallback，也绝不上传 RAW |
| RGB 降噪 | NAFNet/Restormer 开源候选 | 精确本地包、RGB 工作域回执 | 只做 Core ML 对比测试；绝不称为 RAW 降噪 |
| Mac 系统级真 RAW 渲染 | Apple [Core Image RAW 9](https://developer.apple.com/videos/play/wwdc2026/305/)，macOS 27 上的闭源系统框架 | `CIRAWFilter` decoder v9 + OS build + 精确相机/受支持属性快照；由 OS 交付、在 ANE 上运行的 tiled Core ML 联合去马赛克/降噪 | 首个可调用的 Mac-only 真 RAW 质量/性能基准；受可用性约束，不是可移植替代方案 |
| Shadow 自有真 RAW 降噪 | 尚未选择产品 checkpoint；LED/PMN 仅作为研究参考 | 未来的 CFA/噪声 profile 模型与基准身份 | 长期的可移植、可控路线；确定性的 RAW 降噪仍为 fallback |
| 保真 2x | 经典 SwinIR x2 开源候选 | 精确本地包；确定性的固定修订版 | 首个保守基线 |
| 创意/修复型放大 | Real-ESRGAN 开源候选；Adobe/Stability 闭源服务 | 生成栅格来源与提升 | 与默认保真路线分开的生成模式/比较项；Imagen 4 preview 暂缓 |

## 产品不变量

每一种 AI 能力都必须维持以下不变量：

1. AI 永不删除原图，也不写入人工 Pick/Reject/评分账本。
2. 生成结果在用户接受之前必须可重建。用户接受的编辑是一条不可变、非破坏性的依赖，
   带有精确来源。即使执行发生在本地、具有确定性或可以重复，模型修补的像素仍然属于合成像素；
   保真优先的模型放大结果仍然属于带精确来源的派生栅格。
3. 本地与远程是部署、隐私、成本和可用性维度；它不会削弱生成内容的披露或来源要求。
   默认使用本地处理。远程处理必须针对具体能力、明确可见并由用户选择加入，
   且默认永不接收 RAW 文件。
4. 模型结果绝不能隐藏其源修订版、模型修订版、预处理契约、置信度或 fallback 路线。
5. 人工保护与人工编辑的优先级高于模型策略。低置信度只能生成可编辑建议，不能悄悄替用户做产品决策。
6. 当许可、隐私、资源或产物身份无法被如实准入时，该能力必须保持不可用。
   Shadow 不会用伪造的评分、蒙版或像素替代真实结果。

## 能力-provider 协议

现有的 [`AiCapability`](src/contract.rs) 和 [`AiTaskKind`](src/contract.rs)
枚举是唯一规范的能力分类。Provider 路线只细化这些值；
不得另建平行能力枚举来区分可提示与语义蒙版、本地与生成式 inpaint，或 RGB 与传感器域降噪。

### 待扩展的当前责任所有者

| 当前所有者 | 当前契约 | 规划中的扩展 |
| --- | --- | --- |
| [`src/contract.rs`](src/contract.rs) | Provider 中立的 `AiJobRequest`、`AiCapability`、`AiTaskKind`、`AiObservation`、`TaskPriority` 和 `PrivacyClass` | 保持应用意图不依赖于路线选择 |
| [`src/runtime/`](src/runtime/) | 精确的已准入路线与完整计划、路线所有的估算、仅可移动的 lease、取消、单调进度、运行时签发的来源信封、终态回执和明确的 fallback 披露 | 增加应用调度/进程隔离与真实 provider adapter |
| [`src/manifest.rs`](src/manifest.rs) | 精确的多 blob 产物集合，包括 Core ML 包归档、tensor 契约、执行目标、资源要求和许可/分发事实 | 让内容寻址的包注册表消费该 manifest |
| [`src/remote.rs`](src/remote.rs) | 精确远程服务 manifest、准备好的出站存储回执，以及不透明且 fail-closed 的请求 grant 索引 | 在法律/隐私/产品批准前不增加 transport adapter |
| [`src/resource.rs`](src/resource.rs) | 确定性的本地 RAM/VRAM/thread `admit()` 策略和不可变 `RunPlan`；`RemoteApi` 永远不会通过本地准入 | 为 scratch 存储提供独立授权；把实测路线用量反馈给估算，但不把请求 hint 当作安全预算 |
| [`src/generated.rs`](src/generated.rs) | 类型化的主体蒙版与降噪参数、可重建的生成 soft-mask/降噪栅格身份，以及 `DenoiseDomainPolicy` | 增加缺失的 inpaint 与超分辨率值；把蒙版、inpaint 和降噪变体表达为现有任务下的路线 |
| [`src/derived_raster.rs`](src/derived_raster.rs) | 消费 lease 来源的存储提升，以及经存储校验后重新加载不透明托管授权 | 实现应用拥有的托管派生存储及 Recipe 引用 |
| [`src/culling.rs`](src/culling.rs) | 完整的成对特征距离校验、确定性的贪心 complete-link 分组、以相似度 medoid 作为审核起点 | 校准 Vision 阈值，并且只通过单独评估的证据组合 |
| [`src/technical.rs`](src/technical.rs) 与 [`src/score.rs`](src/score.rs) | 无模型的技术证据与可解释的组内相对选择 | 准入模型派生的缺陷、相似度、美学与独特性证据，但不创建泛化的“裁片 observation”能力 |
| [`src/feedback.rs`](src/feedback.rs) | 只追加的展示/决策证据，以及仅接受显式反馈的训练准入 | 只馈入已准入的本地学习流水线；不允许 provider 推断隐式负样本 |

路线图与当前分类的对应关系如下：

| 规范能力/任务 | 规划中的 provider 路线 |
| --- | --- |
| `SimilarityEmbedding` / `ExtractSimilarityEmbedding` | 可比较的视觉特征提取与冻结的距离契约 |
| `TechnicalQuality`、`BurstGrouping` 和 `PersonalRanking` | 由当前评分策略消费的独立裁片信号 |
| `SubjectMask` / `ProposeSubjectMask` | 点/框提示、语义选择和可选 matte 精修，全部产出当前的可编辑 soft-mask 契约 |
| `DepthEstimation` / `EstimateDepth` | 供条件蒙版生成器使用的深度证据 |
| `InpaintPatch` / `GenerateInpaintPatch` | 在一个带来源的 patch 任务下，提供确定性本地移除或显式生成式 provider 路线 |
| `Denoise` / `Denoise` | 通过当前 `DenoiseDomainPolicy` 选择 RGB 或传感器域路线 |
| `SuperResolution` / `SuperResolve` | 保真优先的放大；未来将有生成栅格 payload 契约 |

当前尚未链接的 Apple Vision 路线有意只宣告
`BurstGrouping` / `ProposeBurstGroup`：它消费由调用方选定的照片窗口，
并为分组责任所有者返回完整距离批次。
它不会把 Vision 的 FeaturePrint 字节导出或持久化为通用
`SimilarityEmbedding`，也不会宣告 `TechnicalQuality`。

**当前：**`AiJobRequest` 是 provider 中立、可序列化的 exact-v1 意图；
`admit_local_execution()` 使用 provider 自有的资源估算选择精确产物集合；
运行时 lease 定义了三个 provider 中立的生命周期操作：

1. 规划：校验当前请求和任务参数，选择 provider 路线，调用当前资源准入，
   返回不可变的路线身份和完整执行计划身份；
2. 执行：消费该已准入身份，报告经过校验且单调递增的进度，
   并且只发布一个终态结果或错误；成功结果会包装上由运行时签发的 request/input/route/plan 来源；
3. 取消：立即停止新的准入，并以协作方式停止进行中的工作；
   迟到的结果保留自身身份，由调用方丢弃。

目前没有任何 provider 执行推理。调度、进程隔离、下载 UX、持久化存储和 Recipe 集成仍由应用而非 `shadow-ai` 负责。

### 稳定的作业身份

作业身份必须包含：

- 源 representation 及源修订版 digest；
- 当推理观察编辑后像素时所使用的 Recipe/render 修订版；
- 方向、图像范围、色彩空间、传递函数、样本格式和 alpha 约定；
- 感兴趣区域、扩展上下文和规范化源坐标；
- prompt/蒙版 digest 与操作参数；
- provider、模型、产物、预处理和执行计划修订版；
- provider 暴露 seed 时所使用的 seed。

结果回执还会增加输出 digest、置信度、实际执行路线、时序/资源测量，以及 provider 请求身份。
远程回执不得持久化凭据，也不得持久化包含私有像素的 provider 响应正文。

## 模型包、许可与隐私准入

现有 manifest 是事实来源。规划中的包管理器必须在仓库之外下载或侧载模型包，
按内容寻址、校验签名与精确哈希，并对其做版本管理。
Git 中不得包含模型二进制，也不得产生隐式下载产物。
在适合产品分发的场景，Apple 侧应使用 Background Assets；
应用自有的内容寻址注册表使用 `URLSession`；
已弃用的 `MLModelCollection` 不应成为新的架构依赖。
目前尚无包管理器、下载器或签名校验器。

Core ML 产物集合保存可哈希的下载 blob。
当上游以目录形式发布 `.mlpackage` 时，Shadow 必须先记录可复现归档或规范化的逐文件清单，
校验解压结果，然后再进行编译。
生成的 `.mlmodelc`、稳定路径缓存及逐设备专用化结果可以为了性能保留，
但它们仍然可重建，绝不能取代源产物身份。

### 许可台账

准入流程必须把以下项目作为相互独立的字段进行审查：

- host/推理代码许可；
- 模型架构或仓库许可；
- 精确权重文件的许可；
- 训练数据与基准数据条款；
- 捆绑运行时与传递依赖的许可；
- 再分发、商业使用、署名和可接受用途条件。

代码仓库采用宽松许可，并不能证明某个 checkpoint 或其训练数据可以再分发。
规划中的包注册表所准入的每个产物，都必须记录上游 URL、修订版、digest、本地转换、notice 和审计决策。
即使代码采用宽松许可，非商业权重也不得进入分发产品包。

初步上游分类如下：

| 候选 | 待核实的上游条款 | 规划决策 |
| --- | --- | --- |
| [SAM 2](https://github.com/facebookresearch/sam2/blob/main/LICENSE) | 上游仓库声明为 Apache-2.0 | Infer Runtime 能力；精确部署 checkpoint 仍由其负责 |
| [Depth Anything V2 Small](https://github.com/DepthAnything/Depth-Anything-V2#license) | Apache-2.0 | 候选 |
| [Depth Anything V2 Base/Large/Giant](https://github.com/DepthAnything/Depth-Anything-V2#license) | 上游仓库采用 CC-BY-NC-4.0 | 商业分发暂缓 |
| [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO/blob/main/LICENSE) | Apache-2.0 | 完成 checkpoint/数据审计后作为候选 |
| [ViTMatte](https://github.com/hustvl/ViTMatte/blob/main/LICENSE) | MIT | 完成 checkpoint/数据审计后作为候选 |
| [LaMa](https://github.com/advimman/lama/blob/main/LICENSE) | 源码仓库声明 Apache-2.0；目前链接的 checkpoint 托管/再分发授权不够明确 | 在精确 checkpoint 权利与来源厘清前，只能作为侧载研究原型 |
| [NAFNet](https://github.com/megvii-research/NAFNet/blob/main/LICENSE) | NAFNet 代码为 MIT；依赖条款仍须单独审查 | 仅在完整包审计后作为候选 |
| [Restormer](https://github.com/swz30/Restormer/blob/main/LICENSE.md) | MIT | 完成 checkpoint/数据审计后作为候选 |
| [LED](https://github.com/Srameo/LED/blob/main/LICENSE) | 仓库代码为 CC BY-NC 4.0，商业使用需要正式授权；精确权重与数据集仍须分别审计条款和来源 | 除非正式商业授权覆盖已准入产物，否则不进入产品/商业分发；仅作研究参考 |
| [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN/blob/master/LICENSE) | 仓库为 BSD-3-Clause；精确权重/数据仍须审计 | 作为生成式/修复式比较项，绝不作为默认保真路线 |
| [SwinIR](https://github.com/JingyunLiang/SwinIR/blob/main/LICENSE) / [HAT](https://github.com/XPixelGroup/HAT/blob/main/LICENSE) | 仓库为 Apache-2.0；精确权重/数据仍须审计 | 经典 SwinIR x2 作为首个保真基线；HAT 保留为后续度量项 |
| [FLUX.1 Fill dev](https://huggingface.co/black-forest-labs/FLUX.1-Fill-dev) | Black Forest Labs 非商业条款 | 暂缓 |
| [SUPIR](https://github.com/Fanghua-Yu/SUPIR) | 上游为非商业条款 | 暂缓 |

云 API 仍属于专有服务。其 API 文档只是技术证据，并不等于价格、保留策略、训练用途、
区域可用性或数据处理条款已经获批。每次启用远程 provider 之前，都要重新执行法律/隐私审查。

### 隐私与上传范围

**当前：**每个请求与输入产物都使用 [`src/contract.rs`](src/contract.rs)
中的规范 `PrivacyClass` 值之一：`Public`、`Personal` 或 `SensitiveBiometric`。
独立的远程准入路径应用 [`src/remote/admission.rs`](src/remote/admission.rs)
中的 `RemoteExecutionPolicy`：`Disabled`、`PublicOnly` 或 `PersonalAllowed`。
`SensitiveBiometric` 仍明确暂缓。
确定性的本地 manifest 规划器不检查远程同意，且永远不能授权 `RemoteApi` backend。

**当前契约：**应用拥有的出站存储首先剥离并编码允许发送的材料，
持久化该材料，然后返回一个仅可移动的 prepared receipt。
该回执把精确的 `AiJobRequest` 源输入绑定到其净化后的出站哈希、字节长度、媒体类型、上传范围和栅格范围。
远程准入按出站长度而非原始源长度计量，并消费这些回执来生成一个会过期的请求 grant。
上传范围不是另一种 `PrivacyClass`。RAW 文件、传感器马赛克、场景线性 tile 和冻结特征向量，
都不存在可被远程准入的范围。

在任何上传发生之前，规划中的 adapter 必须剥离 GPS 和无关 EXIF，应用方向，
只编码已准入的栅格与蒙版，估算成本并执行配置的上限。
默认仍然禁止上传完整 RAW。
远程 `SensitiveBiometric` 推理明确暂缓，等待更强的产品、隐私与法律决策。

任何后台任务都不能把本地作业升级为远程执行。
运行时校验会拒绝为本地/系统路线使用 `RemoteApi` 计划，
也会拒绝所选路线为远程的任何 fallback。
因此，远程主路线只能 fallback 到已声明的本地/系统路线；
它不能悄悄选择另一个远程 provider。

## 调度与 GPU 驻留像素

**当前：**[`TaskPriority`](src/contract.rs) 包含九个值，当前资源准入会消费这些值。
规划中的应用调度器可以从中派生四条 lane，但这些 lane 不会取代该枚举：

| 规划中的调度 lane | 当前 `TaskPriority` 值 |
| --- | --- |
| 交互输入 | `CurrentInput` |
| 可见展示与预取 | `CurrentViewport`、`VisibleThumbnail`、`NearbyPrefetch` |
| 主动分析与后台维护 | `ImportValidation`、`CurrentCollectionAnalysis`、`LibraryBackgroundAnalysis`、`CacheMaintenance` |
| 用户请求的完成任务 | `UserExport` |

规划中的运行时必须让一份资源 lease 统一拥有模型驻留、
预测与实测内存、设备队列使用、取消和卸载策略。
在测量结果证明并发安全之前，基础内存设备最多只能准入一个重量级本地模型。
后台作业必须主动让出资源，不能造成滑块、画笔或 viewport 延迟。

在 Apple 平台上，Core ML 是首个本地运行时候选。
它可以通过 [`MLComputeUnits`](https://developer.apple.com/documentation/coreml/mlcomputeunits)
选择 CPU、GPU 和 Neural Engine。
压缩与计算单元选择必须以基准测试决策为准，因为 Apple 的
[优化指南](https://apple.github.io/coremltools/docs-guides/source/opt-overview.html)
明确指出，其收益取决于具体模型和硬件。

规划中的驻留像素路线如下：

```text
Shadow render texture
  -> Metal/IOSurface-backed CVPixelBuffer
  -> Core ML 或窄职责的 Metal preprocessor
  -> GPU-resident mask/result texture
  -> display composition
```

规划中的交互路径不得编码 JPEG、解码 JPEG，也不得让完整图像通过 QML 复制。
只有在生成产物被提升到托管派生存储，或 provider 无法消费驻留契约时，
才应把像素读回 CPU。该 fallback 必须在回执中明确记录。

规划中的蒙版运行时必须按照精确的 source/render 修订版、模型产物、预处理、方向和工作范围缓存 embedding。
规划中的降噪、inpaint 和超分辨率 provider 必须使用带 overlap/halo 和接缝契约的有界 tile。
重复预览应复用同一份驻留资源 lease；缓存遥测不属于持久编辑身份的一部分。

Core ML 转换应先测试 FP16，再测试经过测量的权重压缩。
Apple 的[类型化执行](https://apple.github.io/coremltools/docs-guides/source/typed-execution.html)
与[调色板量化](https://apple.github.io/coremltools/docs-guides/source/opt-palettization-overview.html)
指南定义了平台约束。
每个被接受的包都应附带一份
[`MLComputePlan`](https://apple.github.io/coremltools/docs-guides/source/mlmodel-utilities.html)
和 Xcode 性能报告。
只有在直接 Core ML 转换无法保持模型契约时，ExecuTorch 的
[Core ML 与 MPS backend](https://docs.pytorch.org/executorch/stable/using-executorch-building-from-source.html)
才作为 fallback 候选。

## AI 辅助裁片

### 当前边界

Shadow 目前具备无模型的技术 observation、精确展示证据、人工审核账本、
组内相对评分策略，以及一个小型确定性偏好 head。
它尚无有效的特征提取器、美学模型、自动排序或自动 Pick/Reject 写入方。

### 首个度量流水线

第一个候选是一套本地证据集成：

1. 使用拍摄时间和用户可见的分组控制，对一次拍摄进行分区。
2. 使用 Apple Vision
   [`VNFeaturePrintObservation`](https://developer.apple.com/documentation/vision/vnfeatureprintobservation)
   及其[距离契约](https://developer.apple.com/documentation/vision/vnfeatureprintobservation/computedistance(_:to:))
   生成可比较的距离证据，然后形成经过阈值校准的分组。
   Apple 只承诺距离越短表示越相似；FeaturePrint 不是对焦、表情、美学或“最佳照片”模型。
   当前确定性的贪心 complete-link 分组和相似度 medoid 只是审核导航建议，不是质量排名。
3. 只有在完整分析产物和预处理修订版一致时，才加入已有的可比较技术证据。
4. 对符合条件、且局限在一次连拍中的人脸 track，加入 Vision
   [人脸拍摄质量](https://developer.apple.com/documentation/vision/selecting-a-selfie-based-on-capture-quality)
   作为独立信号，绝不把它用作身份 embedding。
   Apple 限定该评分只能比较同一张脸的多次拍摄，并把它描述为针对光照、模糊、遮挡、
   表情、姿态、对焦、位置及其他拍摄属性的整体先验，而不是孤立的清晰度证据。

   同一人脸的对应关系必须来自明确准入的临时 track 或用户确认；
   质量评分本身不能建立身份。
   只有调用方指定的每条人脸 track 都在每个候选中出现且对应无歧义时，多人脸比较才符合条件。
   把每条 track 的原始评分转换为本次比较内的百分位排名，同时保留原始值作为证据；
   然后最大化候选 tuple：先最大化最低 track 百分位，再最大化 track 百分位中位数。
   这样可以防止一张表现不佳的人脸被多张表现良好的人脸掩盖，
   又不会假装不同人的评分可以直接比较。
   如果缺少对应关系、完整的必要 track 覆盖，或至少一条共同且符合条件的 track，
   则必须放弃多人脸评分与排序，只展示逐脸证据。
   表情、姿态和遮挡仍然属于不透明的模型先验，需要在子组中复核；
   它们不是用户意图或缺陷事实，也不得被并入 FeaturePrint 距离。
5. 在同一组留出拍摄数据上，评估 Vision
   [图像美学](https://developer.apple.com/documentation/vision/calculateimageaestheticsscoresrequest)
   与已发表的 [MUSIQ 方法](https://research.google/pubs/musiq-multi-scale-image-quality-transformer/)。
   目前尚未选择 MUSIQ 实现、checkpoint 或训练产物；
   任何原型都必须先进入完整的许可与产物台账。
6. 应用现有可解释的组内相对策略，并且只从显式、可比较的成对证据中学习这个小型偏好 head。

[LAION aesthetic predictor](https://github.com/LAION-AI/aesthetic-predictor)
只用于研究：其小规模美学标签集和 CLIP 派生先验会带来显著的领域与文化偏差风险。
它不是默认候选。

UI 可以建议分组审核起点、重复项折叠，或者——只有在独立质量证据通过准入后——建议一个排序后的审核顺序。
它不能隐藏被人工保护或独特的照片，不能写入 Reject，不能把相似度 medoid 称为“最佳”，
也不能声称一个全局美学评分代表用户意图。
每条建议都必须展示其相互独立的证据类别和置信度。

### 数据与验收

使用权利清晰、不同 shoot 互不重叠的连拍数据，
覆盖人像、活动、野生动物、街拍、产品与风光摄影。
评估子集至少由三名独立标注者标注；
除非用户另行选择贡献数据，否则一名用户的显式反馈只能训练该用户自己的本地偏好状态。

报告：

- 每个建议审核压缩率下的 keeper recall；
- 可比较组内的 NDCG@k 与 rank correlation；
- 重复项 cluster 的 pairwise F1 与 adjusted Rand index；
- 对独特或人工保护照片的错误抑制，该值必须为零；
- 按相机、题材、光照、肤色和人脸数量统计的最差子组结果；
- 冷/热吞吐、峰值内存、能耗和 UI 干扰。

## 可提示、语义与条件蒙版

### 首个本地蒙版 provider

首个通用交互 provider 应对 Apple 的 Core ML
[SAM 2.1 Tiny](https://huggingface.co/apple/coreml-sam2.1-tiny) 和
[Small](https://huggingface.co/apple/coreml-sam2.1-small) 包进行基准测试。
上游 [SAM 2](https://github.com/facebookresearch/sam2) 接受点、框和蒙版 prompt。
Tiny 是基础设备候选；只有在 Small 能带来可测量的边界改进时，它才作为可选质量包。

原型应针对一个工作源修订版只运行一次 encoder。
然后，前景/背景点与框应在缓存 embedding 上运行 prompt decoder，
方式参照上游 [`sam2-studio`](https://github.com/huggingface/sam2-studio) 示例。
输出必须保持为可编辑的 soft mask；
include/exclude 画笔修正属于第一等证据，不能在精修后丢弃。

Apple Vision 的
[`GenerateForegroundInstanceMaskRequest`](https://developer.apple.com/documentation/vision/generateforegroundinstancemaskrequest)
是受支持的前景/人物场景的低成本候选。
2026 Beta 的
[`GenerateIterativeSegmentationRequest`](https://developer.apple.com/documentation/vision/generateiterativesegmentationrequest)
支持点、框和涂鸦，但在正式发布的 SDK/OS 与可下载产物行为完成校验前，必须受可用性 gate 控制。
在 Shadow 的桌面基线不能保证该能力存在时，它不能成为唯一实现。

### 条件蒙版分类

规划中的每个生成器都必须产出相同的规范化 soft-alpha 契约，
并支持通过 `AND`、`OR`、`NOT`、add、subtract 和 intersect 操作进行组合：

| 类别 | 条件 |
| --- | --- |
| 色调 | 亮度、阴影、高光、有界范围 |
| 色彩 | 色相、色度、与采样色的感知距离、单独通道 |
| 结构 | 边缘、纹理/频率、细节、对焦/清晰度、估计噪声 |
| 几何 | 线性/径向渐变、距离、方向、形状 |
| 深度 | 近/中/远及有界深度范围 |
| 语义 | 前景/背景、人物、人脸、皮肤、头发、衣物、天空、地面、水、植被、建筑、动物、文字、提示对象 |
| 关系 | 任意条件与人工画笔/渐变之间的交集或排除 |

规划中的蒙版 UX 默认让绘制蒙版保持匿名，并只属于本地 node；
其像素不适合作为 preset。
智能/条件蒙版可以保存并命名其生成器 recipe，因为该 recipe 可复用；
但生成像素仍然绑定到一个源修订版。

后续需要度量的候选包括：

- [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO) + SAM 2，
  用于通过文字选择概念；
- [Depth Anything V2 Small](https://github.com/DepthAnything/Depth-Anything-V2)，
  用于深度条件；
- [ViTMatte](https://github.com/hustvl/ViTMatte)，
  用于由 trimap 驱动的头发、皮毛、玻璃和其他困难 alpha 边界。

更大的 Depth Anything V2 权重暂缓，因为上游为它们指定了非商业条款。
Florence-2 保持探索状态，直到转换、自回归 decoder 延迟、checkpoint 许可和内存完成度量；
其[官方模型卡](https://huggingface.co/microsoft/Florence-2-base)
本身并不能证明它能生成产品质量的照片蒙版。

### 蒙版验收

照片领域 corpus 必须包含头发、皮毛、树枝、电线、透明物体、烟雾、
低对比度边界、人物重叠、微小对象和经过强烈编辑的色彩。
报告 mIoU、Dice、boundary F-score、首次点击质量、clicks-to-IoU-90、
encoder P50/P95、热 prompt P50/P95、峰值内存，以及 tile/全图一致性。

最低支持规格 Mac 上的临时交互 gate 为：

- UI thread 上不执行推理工作；
- 固定基准 proxy 上，从热 prompt 到可见蒙版的 P95 不超过 100 ms；
- 冷模型加载后，首个可用蒙版的 P95 不超过 1.5 秒；
- 调度器在 250 ms 内确认取消；
- FIT、100% 和 tiled 视图之间不存在接缝或坐标漂移。

如果 provider 未能满足交互 gate，它必须保持实验状态；
这不能成为伪造同步结果的理由。

## 模型生成的对象移除：本地与远程路线

Clone 和 Heal 仍然是源引导的确定性修正工具。
基于模型的 inpaint 是独立能力，因为它会合成像素，也可能虚构语义内容。
固定的本地 checkpoint 可以让执行可重复，但不会使输出不再属于生成内容。
因此，本地与远程改变的是隐私、成本、网络和部署策略，
而不会改变生成栅格的来源、可见披露、接受、撤销或比较契约。

### 本地模型路线

[LaMa](https://github.com/advimman/lama) 是一个有价值的本地大蒙版 inpaint 方法参考。
即使没有文字 prompt，它生成的像素仍然属于模型合成输出，
因此只能作为清晰标注为生成式、**侧载的 Object Remove (Local)** 原型进行评估。
源码仓库的许可本身不能证明托管 checkpoint 的再分发权或数据权利；
在精确 checkpoint、托管链、训练数据来源和 notice 通过准入之前，
LaMa 不能被捆绑，也不能自动下载。

LaMa 原型必须接收蒙版周围一个有界的扩展上下文。
其结果只能合成到明确扩展且经过羽化的编辑区域内。
区域外像素不变性是一项硬性验收契约。
最终交互必须允许用户比较、重新生成、丢弃或接受该建议。

### 远程填充

应在同一 adapter 契约后评估当前可调用的 provider，
不能把 API 可用性误认为它适合作为默认项：

- Adobe Firefly Services
  [异步 API 指南](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/using-async-apis/)
  列出了 Fill Image Async。
  Adobe 的[更新日志](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/)
  记录了同步 Fill Image v3 路线已在 2025 年 10 月移除，
  因此只有当前文档中记录的异步路线可以进入原型；
- Black Forest Labs
  [FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill)。

Provider API 与模型名称都可能频繁变化。
启动 adapter 原型时，必须选择并重新验证精确的 Adobe 异步 endpoint、schema、model/header 修订版，
以及 BFL service/model endpoint；这份候选清单不是冻结的 wire contract。

[Stability v2beta inpaint](https://platform.stability.ai/docs/api-reference) 与
[Vertex Imagen editing](https://docs.cloud.google.com/vertex-ai/generative-ai/docs/image/edit-images-overview)
仍是比较候选。
Vertex 已发布的 model/endpoint 迁移通知意味着，精确受支持路线也必须在原型阶段重新选择。
阿里巴巴的
[Qwen Image editing API](https://help.aliyun.com/en/model-studio/qwen-image-edit-api)
是另一个自然语言编辑比较项，不会替代精确蒙版控制。

尽管本地 [Qwen-Image-Edit](https://huggingface.co/Qwen/Qwen-Image-Edit)
的仓库条款宽松，它仍被暂缓：
其 20B 量级资源包络与精确蒙版行为不适合当前本地产品基线。

Provider 的选择要依据盲测 corpus、隐私/保留策略审查、区域可用性、
删除保证、成本上限、ICC/色彩行为以及 retry/幂等行为。
远程 fill 永远不能成为本地 Heal 或 LaMa 的静默 fallback。

Stability 的 fast/conservative/creative 放大路线仍然是生成输出：
即使名为“conservative”，它也接受 prompt/creativity 控制，
不会因为名称而成为默认保真路线。

每个本地或远程候选版本都必须记录源修订版、上下文 crop、蒙版 digest、prompt、
可用时的 seed、provider/model 版本、请求身份和输出 digest。
被接受的像素必须先提升到托管派生存储，Recipe 才能依赖它们。

验收报告应包含区域外不变性、边界梯度不连续性、接缝色差、
上下文内的 LPIPS/DISTS、人脸/文字/身份保持、多 seed 失败率、
provider 拒绝率和盲测人工偏好。
重复线条、建筑、头发、文字、人脸、天空和重复纹理是强制失败测试集。

## RGB 降噪与真 RAW 降噪

这两种能力必须始终保持为不同契约。

### RGB 降噪候选

[NAFNet](https://github.com/megvii-research/NAFNet) 和
[Restormer](https://github.com/swz30/Restormer)
是面向色彩管理 RGB 工作像素的 Core ML 对比测试候选。
它们可以支持 JPEG 清理或后期 RGB 降噪阶段，
但绝不能被描述为传感器感知的 RAW 降噪。

对比测试使用相同的 tiling、halo、色彩与资源回执。
经典确定性的 GPU 降噪器仍为 fallback。
在完成隐私与服务条款审查后，Topaz 的
[Image API 模型](https://developer.topazlabs.com/image-api/available-models)
可以充当闭源远程质量参考；它们不会进入 RAW CFA 流水线。

### 真 RAW 降噪方向

#### Mac-first 闭源系统 RAW 9 基准

Apple 官方的
[WWDC26 Core Image RAW session](https://developer.apple.com/videos/play/wwdc2026/305/)
记录了 macOS 27 上一个可调用、需主动选择的 v9 `CIRAWFilter` decoder。
RAW 9 使用 tiled Core ML 流水线，在 Apple Neural Engine 上联合进行去马赛克与降噪。
这是一条真正的传感器马赛克路线，而不是 RGB 修复模型，
因此它会成为受支持 Mac 上首个闭源系统真 RAW 基准。

准入流程首先必须观察到 `supportedDecoderVersions` 包含版本 9，
并通过 `supportedCameraModels(for:)` 校验相机。
Apple 表示初始列表覆盖主要厂商的数百款机型，原生 DNG 相机自动受支持，
而且该列表可能通过 OS OTA 更新发生变化。
因此，仅有“decoder 9”不足以构成可复现的路线身份。
未来的系统框架 adapter 与结果回执还必须绑定：

- 规范化的相机 make/model 与原生 DNG 状态；
- 精确 OS build + decoder version 9；
- 观察到的 version-9 支持相机列表 digest；
- 该 filter instance 的精确受支持 calibrated-property 集合；
- 每个实际应用的 calibrated 值，包括
  `luminanceNoiseReductionAmount`、exposure、sharpness 和 contrast；
- scale factor、输出 extent/domain/color 契约，以及交互与导出上下文策略。

RAW 9 移除或忽略了一些旧控制，包括旧的色彩噪声、细节和摩尔纹调整，
因此 adapter 必须查询属性支持，而不是按名称映射 Shadow 控制。
当前通用的 `SystemFramework` 身份不足以表达这条依赖相机与属性的路线；
在链接 adapter 之前，必须先扩展该身份。

基准必须把 RAW 9 当作一条端到端的去马赛克/降噪/渲染路线，
不能声称可以把它的降噪阶段单独剥离。
把冷启动首次渲染、热参数编辑、全分辨率导出、内存、功耗、色彩、细节、噪声和相机覆盖率，
与 Shadow 当前确定性 RAW 路径进行比较。
交互评估遵循 Apple 文档中的快速路径：
降低 `scaleFactor`、每个视图使用一个带缓存的 `CIContext`，并直接使用 Metal-backed presentation。
导出评估使用无缓存 context，并记录明确的内存上限。
不受支持的相机、早于 macOS 27 的系统，或变化后的支持快照，
必须清晰地 fallback 到 Shadow 现有流水线。

RAW 9 仍然是闭源、仅限 Apple 平台、由 OS 交付，
而且不受 Shadow 的权重、训练数据与更新控制。
因此，它是一个 Mac-first 产品候选与比较上限，
不是 Windows/Linux 的通用路线，也不能替代下述长期 Shadow 自有模型。

#### Shadow 自有的可移植方向

产品质量的可移植 RAW 降噪，需要一个 Shadow 自有的传感器域模型，并以下列条件作为输入：

- CFA pattern 与 phase；
- black level 与 white level；
- 样本布局与 bit depth；
- ISO、曝光和可用的相机噪声校准；
- 热像素、行/列噪声、shot noise 和 read noise 特征。

训练计划需要权利清晰、成对的短/长曝光、对齐连拍、暗场、平场、
多种温度与 ISO 水平，并增加 Poisson-Gaussian、条带和热像素增强。
[PMN 研究实现](https://github.com/megvii-research/PMN)
只是成对真实噪声与物理引导噪声合成的方法参考，不是通用的预训练产品模型。
SIDD、SID、ELD 和 DND 等公开数据集在进入训练或发布证据之前，
需要分别审查使用与再分发权利。
当前 RAW 修复挑战仍然把跨相机真实 RAW 降噪视为开放问题：
不能把 SIDD RGB checkpoint 或单相机低光 checkpoint 打包成通用 RAW 降噪。
在完成精确权重/数据权利，以及 Shadow 的 CFA、phase、black/white-level 和 noise-profile 契约评估之前，
LED/PMN 都只作为研究基线。

评估内容包括传感器域 PSNR/SSIM、经过色彩处理后的 Delta E 2000、
LPIPS/DISTS、MTF/细节保留、残余噪声功率谱、热像素、条带、去马赛克伪影和 tile 接缝。
星点、皮毛、头发、文字、皮肤和精细重复纹理，
用于专门测试蜡质感与虚构细节。

因此，第一个可产品化步骤是运行时加不可变的基准 harness。
在受支持的 macOS 27 硬件上，RAW 9 可以立即进入该 harness；
当前确定性 RAW 路径仍然是其明确 fallback。
Shadow 自有模型保持为长期可移植计划，而不是过早选定一个 checkpoint。
在某条路线能够在留出相机上击败确定性路径，
且不会造成不可接受的色偏、纹理损失、相机域回退或支持不稳定之前，
两者都不能成为默认路线。

## 2x 超分辨率

[SwinIR](https://github.com/JingyunLiang/SwinIR) 的经典 x2 路线是首个保守的保真基线，
但须先完成精确 checkpoint/数据审计与 Core ML 转换。
[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)
是独立的生成式/修复式比较项，因为其针对真实世界的对抗先验可能虚构纹理；
它不能成为默认保真路线。
当质量收益足以覆盖延迟与内存成本时，
[HAT](https://github.com/XPixelGroup/HAT) 作为后续度量项。

规划中的首个产品模式是 **Fidelity 2x**：

- 对固定模型修订版而言，本地且确定；
- 只在导出时或用户显式请求时执行，永不阻塞普通调整预览；
- 使用带 overlap/halo 和精确缩放倍数的 tile；
- 在模型获准的 display/RGB 域中运行，不能宣传成恢复了场景线性 RAW 真值。

未来的 **Creative Detail** 路线相互独立、清晰标注为生成式，并默认关闭。
Adobe 当前的
[Upscale 指南](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/upscale/)
记录了异步 `/v1/images/upsample-async` 路线。
其 [2026 年 4 月 GA 更新日志](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/)
把 `precise_upsampler_v1` 列为唯一受支持的 model-header 值。
这是远程参考，不是冻结的集成：
Shadow 必须在原型启动时重新选择并记录精确 endpoint、schema、model/header 与服务条款。
Adobe 对源保真度的定位与 2x 指南仍须经过留出评估；
更大缩放倍数还需要额外的幻觉审查。
Stability upscale 同样属于生成式能力。
Google Imagen 4 upscale 仍是 preview，不能定义稳定发布路线。
Topaz 或其他云服务可以定义比较上限，
但任何服务在未通过相同的隐私、成本、来源、保留策略和留出保真契约前，都不能成为默认项。

使用合成与真实退化，对 12、24 和 45 megapixel 源进行评估。
报告 PSNR/SSIM、LPIPS/DISTS、Delta E、ringing/halo 指标、OCR 正确率、
人脸身份漂移、虚假纹理/幻觉审计、接缝、P50/P95 导出时间、峰值内存和能耗。
在保守 2x 证明其价值之前，4x 模型暂缓。

## 交付顺序

### 阶段 0：运行时与证据基础

1. **当前契约：**冻结 provider plan/execute/cancel/progress 值、
   稳定路线 + 完整计划身份、lease 签发的来源信封，以及终态回执。
   **剩余工作：**应用调度器、进程隔离和真实 provider 执行。
2. 增加签名模型包注册表、精确产物审计、下载/侧载，以及仓库之外的版本化淘汰机制。
3. 实现本地 worker 隔离、资源 lease、取消、崩溃恢复和如实记录 fallback 的回执。
4. 建立 Metal/CVPixelBuffer/Core ML 驻留像素路径。
   在 macOS 27 上，增加只读的 Core Image RAW 9 能力探针，
   记录 decoder、相机列表、calibrated-property 与 OS-build 身份，但暂不改变默认解码路线。
5. 构建基准 harness、权利清晰的 golden corpus，以及不可变的结果格式。
6. **当前契约：**只消费 lease 签发的输出，通过应用托管存储授权执行提升，
   并且只有在存储完成精确字节校验后，才重建不透明的托管授权。
   **剩余工作：**在被接受的生成像素可以进入 Recipe 之前，
   实现该持久化存储与不可变的 Recipe 引用。
   Recipe-local 的 vector/spatial 蒙版修订已经存在，本阶段不重复实现。

任何用户可见的模型能力都不能绕过本阶段。

### 阶段 1：有界的本地辅助

1. 链接 Vision FeaturePrint（当前 provider 明确不可用）并对其进行基准测试，
   同时测试人脸质量与美学，并接入现有裁片证据路径；
   只有通过留出 gate 后才展示建议。
2. 集成 SAM 2.1 Tiny，作为可编辑的可提示蒙版原型；
   比较 Small 与 Vision 路线。
3. 为通用条件蒙版契约增加确定性的色调、色彩、结构、几何和构图 operator。

### 阶段 2：生成像素的本地工具

1. 在受支持的 Mac 上链接 Core Image RAW 9，并把它作为闭源、受可用性 gate 控制的系统路线进行基准测试；
   对不受支持的相机/平台保留现有流水线，并把它作为跨平台基线。
2. 只有在 checkpoint 权利审计后，才测试侧载 LaMa 的本地对象移除。
3. 只把 NAFNet 和 Restormer 作为 RGB 域降噪进行测试。
4. 把经典 SwinIR x2 作为保真基线进行测试，
   然后分别比较 HAT-S 与生成式/修复式 Real-ESRGAN。
5. 只有被接受的输出才能通过托管派生存储与 Recipe 来源流程提升。

### 阶段 3：语义深度与可选服务

1. 评估 Grounding DINO + SAM 2、Depth Anything V2 Small 和 ViTMatte。
2. 在法律/隐私批准后，进行 Adobe 与 BFL 远程 fill 的盲测。
3. 启动权利清晰的相机噪声采集与真 RAW 降噪训练计划。
4. 只把创意放大 provider 作为独立、显式模式进行评估。

## 共享基准与发布 gate

每个候选都使用相同的不可变基准记录：

- 仓库修订版、provider/model/artifact/preprocessing 修订版；
- 精确 fixture 身份与权利分类；
- 硬件、OS、运行时、计算单元、电源状态和热状态；
- 对 OS 交付的系统路线，记录 decoder/request 修订版、规范化相机身份、
  受支持相机列表 digest、受支持 calibrated-property digest、
  实际应用的属性值，以及系统支持快照是否发生变化；
- 输入尺寸/域以及冷/热状态；
- P50/P95 延迟、吞吐、峰值 RSS/统一内存/设备内存、能耗，以及取消/卸载延迟；
- 质量指标、子组结果、失败项和保留的视觉样例；
- CPU/fallback 一致性、tile/全图一致性，以及在承诺确定性时的确定性 replay。

阶段 0 必须选择并记录最低支持的 Apple Silicon 内存档位。
在可用时，M1 Pro 32 GB 和 Windows 64 GB/RTX 4070 Ti 机器被指定为基准 fixture，
而不是产品支持承诺。
只有在 Windows provider 实现后，Windows fixture 才相关。
还应使用当前更高档的 Apple Silicon fixture 测量 scaling。
图像尺寸至少覆盖 12、24 和 45 megapixel。

候选只有满足下列条件，才能成为默认项：

1. 其精确产物与完整许可台账通过准入；
2. 准确表达其隐私分类与远程行为；
3. 最低硬件档位在低内存时能够安全拒绝或降级；
4. 重量级工作不阻塞 UI thread，且相关交互 gate 全部通过；
5. 失败、取消、崩溃、过时结果与离线路径都经过测试；
6. 在留出数据上，质量击败指定的确定性/当前基线，
   且受保护子组的回退不超过已接受边界；
7. 生成像素和蒙版能通过不可变来源与非破坏性撤销完成 round-trip；
8. 包可以移除或升级，而不会让已接受编辑变得不可解释。

## 明确暂缓

以下事项不在当前实现顺序内：

- 自动删除、自动硬 Reject，或由模型写入人工审核账本；
- 后台云端上传、隐式 provider 选择，或默认上传完整 RAW；
- 在分发产品中包含本地 FLUX.1 Fill `[dev]`、SUPIR 或其他非商业权重；
- 在许可、内存、延迟与精确蒙版行为发生实质改变之前，本地运行 12B/20B 量级生成式编辑；
- 使用采用当前非商业条款的 Depth Anything V2 Base/Large/Giant；
- 声称 RGB 修复模型属于真 RAW 降噪；
- 默认生成式超分辨率，或用任何措辞暗示虚构细节是恢复出的真值；
- 完全依赖 Beta Apple Vision API 或可下载系统模型；
- 仅根据排行榜或 vendor 营销就接受模型，而没有 Shadow 自有的照片领域证据；
- 为现有 `AutoDevelopRecipe` 能力实现 provider；
- 为 `SemanticEmbedding`、`SemanticCaption`、VLM 或其他语义 caption/search 索引实现 provider；
- `QueryPlanning`、自然语言 Library 搜索或 LLM 搜索 agent；
- 人脸身份识别、持久化人物聚类或生物特征身份存储；
- 远程 `SensitiveBiometric` 推理，即使当前策略契约能够表达未来选择加入的上限；
- 隐式、联邦式或远程上传反馈、偏好样本或用户学习状态。

## 上游证据索引

本方案使用的第一手来源：

- Apple Vision：
  [概览](https://developer.apple.com/documentation/vision)、
  [美学](https://developer.apple.com/documentation/vision/calculateimageaestheticsscoresrequest)、
  [人脸拍摄质量](https://developer.apple.com/documentation/vision/selecting-a-selfie-based-on-capture-quality)、
  [FeaturePrint](https://developer.apple.com/documentation/vision/vnfeatureprintobservation)、
  [前景蒙版](https://developer.apple.com/documentation/vision/generateforegroundinstancemaskrequest)，以及
  [迭代分割](https://developer.apple.com/documentation/vision/generateiterativesegmentationrequest)。
- Apple Core ML：
  [计算单元](https://developer.apple.com/documentation/coreml/mlcomputeunits)、
  [优化](https://apple.github.io/coremltools/docs-guides/source/opt-overview.html)、
  [类型化执行](https://apple.github.io/coremltools/docs-guides/source/typed-execution.html)，以及
  [调色板量化](https://apple.github.io/coremltools/docs-guides/source/opt-palettization-overview.html)。
- Apple Core Image RAW：
  [WWDC26：探索新的 Core Image RAW 流水线](https://developer.apple.com/videos/play/wwdc2026/305/)。
- 可提示与条件蒙版：
  [SAM 2](https://github.com/facebookresearch/sam2)、
  [Apple SAM 2.1 Tiny](https://huggingface.co/apple/coreml-sam2.1-tiny)、
  [Apple SAM 2.1 Small](https://huggingface.co/apple/coreml-sam2.1-small)、
  [Grounding DINO](https://github.com/IDEA-Research/GroundingDINO)、
  [Depth Anything V2](https://github.com/DepthAnything/Depth-Anything-V2)，以及
  [ViTMatte](https://github.com/hustvl/ViTMatte)。
- 修复：
  [LaMa](https://github.com/advimman/lama)、
  [NAFNet](https://github.com/megvii-research/NAFNet)、
  [Restormer](https://github.com/swz30/Restormer)、
  [LED 仓库与许可](https://github.com/Srameo/LED/blob/main/LICENSE)、
  [PMN](https://github.com/megvii-research/PMN)、
  [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)、
  [SwinIR](https://github.com/JingyunLiang/SwinIR)，以及
  [HAT](https://github.com/XPixelGroup/HAT)。
- 远程 API：
  Adobe Firefly
  [异步 API](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/using-async-apis/)、
  [更新日志](https://developer.adobe.com/firefly-services/docs/firefly-api/getting-started/changelog/)，以及
  [Upscale 指南](https://developer.adobe.com/firefly-services/docs/firefly-api/guides/how-tos/upscale/)；
  [BFL FLUX.1 Fill](https://docs.bfl.ml/flux_tools/flux_1_fill)、
  [Stability](https://platform.stability.ai/docs/api-reference)、
  [Vertex Imagen editing](https://docs.cloud.google.com/vertex-ai/generative-ai/docs/image/edit-images-overview)、
  [Qwen Image editing](https://help.aliyun.com/en/model-studio/qwen-image-edit-api)，以及
  [Topaz Image API](https://developer.topazlabs.com/image-api/available-models)。
