# RAW 解析与高光重建维护指南

本文记录 Shadow 当前 RAW 主路径、CFA 高光重建、后续高光压缩和交互缓存合同。它的目的
不是代替源码，而是把容易在局部优化时被破坏的跨阶段不变量放在一起。修改实现时仍以本文
列出的源码所有者、版本身份和合同测试为准。

本文只描述 Shadow 自己拥有的 provider-neutral `RawFrame` 路径。LibRaw 或私有解码器已经
生成 RGB 的兼容路径，以及不再保留 CFA photosite 的 AI Camera RGB foundation，必须继续被
视为不同的源合同。

## 最短版：不可破坏的十条规则

1. RAW 解码只负责交付未经处理的完整传感器 `uint16` 平面和经过验证的描述符；不能偷偷做
   黑电平、白平衡、裁切、方向、去马赛克、降噪或高光修复。
2. 黑电平是 LibRaw 的 `black + cblack[site]`；物理编码白点是 `maximum`，不是
   `linear_max`。`linear_max` 只能作为可选的线性响应末端，用于降低色彩置信度。
3. 白平衡在 CFA photosite 上、去马赛克之前应用，并保留大于 `1.0` 的 fp32 场景余量。
   物理白点拓扑必须另行携带，不能把某个独立饱和 CFA 相位重新截到公共 `1.0`。
4. 默认高光修复的写入所有权属于损坏的 CFA photosite。邻域只提供候选值，不能扩大写入
   区域；任何“先模糊/膨胀 mask，再在 RGB 上混色”的实现都不属于普通 Bayer 主路径。
5. 修复是单向的：目标 photosite 只能从实测值向上移动到局部 opposed reference，不能
   被压暗；邻域只能提供局部标量候选，不能把邻居 hue 或写入权跨边界扩散。
6. 一色发光体必须保持一色。只有两个独立 CFA 颜色同时失去余量，或三个颜色都进入共享
   物理白点时，才能降低不可信的相机域色度。
7. 缩略预览必须先逐 photosite 修复，再按真实覆盖面积、按 CFA 颜色分别积分。一个输出
   像素同时覆盖亮灯和暗灯架时，暗灯架贡献必须保持原值；不能把整个输出像素涂成黄/灰。
8. 普通 Bayer 源完成 CFA 修复后，不得再运行 post-demosaic RGB 高光表面替换。第二次写入
   无法再分辨同一重建像素里的灯面与灯架，会重新画出粉色色环、黄色扩边或灰色轮廓。
9. CPU、一次性 Metal、resident Metal、缩略预览、细节和导出必须使用同一组阈值和公式。
   性能合同是不新增完整帧 side buffer、额外 pass 或 host/device 往返。
10. 改变任何阈值、写入所有权、头部余量、CPU/Metal 数学或 source-stage 行为时，必须同步
    更新缓存可见身份和合同测试；不能让旧缓存与新语义共用身份。

## 端到端流程

```text
照片文件
  │
  ├─ JPEG/HEIF ───────────────────────────────→ raster source（不进入本文 RAW 路径）
  │
  └─ RAW
      │
      ├─ 公共 LibRaw 能完整解码 ──────────────┐
      ├─ Nikon HE/HE*：LibRaw 元数据/预览     │
      │                   + 私有 provider RAW ├─→ provider-neutral RawFrame
      └─ 其他窄格式私有 provider ─────────────┘
                                                   │
                                                   ├─ 计划协商、DCP/矩阵、白平衡编译
                                                   ├─ 原始剪切 mask 与 R8 色度风险投影
                                                   ├─ 可选 CFA 降噪
                                                   └─ CFA 重建（CPU 或 Metal）
                                                        │
                   ┌────────────────────────────────────┼─────────────────────────┐
                   │                                    │                         │
              面积缩略预览                        原生 balanced/fast         原生 high
          photosite 修复后分色积分                 3x3 bilinear             edge-aware
                   │                                    │                         │
                   └────────────────────────────────────┴─────────────────────────┘
                                                        │
                                              相机域色度置信处理
                                                        │
                                             camera → scene-linear RGB
                                                        │
                                      DCP 后矩阵阶段、光学校正、Recipe
                                                        │
                                      交互预览 / 细节瓦片 / 导出 / 显示
```

这个顺序是语义顺序，不等于必须有同样数量的内存 pass。Metal 会把 CFA 降噪、重建、相机矩阵、
可执行的 DCP continuation 和剪切投影尽量合并在一个 tiled transaction 中。

## 1. Provider 路由与 RAW 解码

### 1.1 路由原则

`src/decoder/photo_decoder_router.cpp` 先处理 JPEG 等 raster 输入。对 RAW，公共 LibRaw 是正常
首选：只要它能提供可用的 reference-RGB 路径，就直接使用公共 session。若 LibRaw 只能打开
元数据或嵌入预览，则本地私有 provider 获得认领文件的机会。

Nikon Z9 HE/HE* 是这个组合路由的典型情况：公共 LibRaw 可以保留事实元数据和相机内 JPEG，
但它不能把这类压缩声明为可用 `RawFrame`；私有 provider 负责交付传感器平面。如果两者都
可用，组合 session 的元数据、RAW 与 RAW 计划来自私有 provider，只有浏览预览来自公共
LibRaw。嵌入 JPEG 绝不能变成可编辑 RAW 源。

私有 provider 是显式本地插件，不是公共仓库中的 vendor SDK。插件必须精确匹配当前：

- private decoder ABI；
- `RawDevelopmentPlan` schema；
- `RawFrame` schema；
- 跨模块类型布局 token。

任何不匹配都要求重新构建插件，而不是尝试兼容旧二进制。私有 SDK 若有进程级共享状态，
只在同一模块身份内串行；不同公共 LibRaw session 仍可并行。

### 1.2 `RawFrame` 的含义

`include/shadow/image/raw_frame.hpp` 定义唯一 provider-neutral 边界。`samples` 是完整 storage
sensor coordinates 下紧密排列的 native-endian `uint16` 单平面。它包含 active rectangle
以外的 margin，并且在交付时尚未执行：

- crop 或方向变换；
- 黑电平扣除和白点归一化；
- 白平衡；
- DNG opcode；
- CFA 降噪；
- 去马赛克；
- 相机到工作空间的颜色转换；
- 高光重建。

描述符至少携带：provider 身份、storage/active dimensions、四边 margin、LibRaw orientation、
CFA layout 与 2×2 相位、位深、每相位黑/白点、可选线性响应末端、四相位 as-shot neutral、
可选噪声模型、相机矩阵和待处理 DNG opcode 声明。

`RawFrame::valid()` 是硬门槛：尺寸与样本数量必须完全相等，active rectangle 加 margin 必须
精确覆盖 storage，黑点必须小于白点，可选 response limit 必须位于 `(black, white]`，矩阵
必须有限且非零。Bayer 2×2 必须恰好包含一个 R、两个 G、一个 B；X-Trans 不能因为左上角
恰巧像 RGGB 就被误认成 Bayer。

### 1.3 LibRaw 字段映射

`src/decoder/libraw_decoder.cpp` 只在 `unpack()` 后复制 `rawdata.raw_image`。关键映射如下：

| Shadow 字段 | LibRaw 来源 | 约束 |
| --- | --- | --- |
| storage / active / margins | `sizes` 与统一 geometry owner | 仍是未旋转 RAW 坐标 |
| CFA 颜色 | `COLOR(row, col)` + `cdesc` | X-Trans 明确拒绝 Bayer 路径 |
| black | `color.black + color.cblack[index]` | `cblack` 是修正量，不是替代值 |
| white | `color.maximum` | 物理编码白点，各相位共享该帧校准值 |
| linear response limit | `color.linear_max[index]` | 四相位全部合法才启用；不能代替 white |
| as-shot neutral | DNG `AsShotNeutral`，否则 `1 / cam_mul` | fallback 以两个绿色相位均值归一化 |
| camera → linear sRGB D65 | `rgb_cam` | 两个绿色输入合并到一个 canonical G 列 |
| XYZ D65 → camera | `cam_xyz` | 用于把温度/色调白点投影为物理 camera neutral |

把小的 `cblack` 当作完整黑点会把一个 CFA 通道抬高数百 DN，在高调/剪切区域形成粉色。
把 `linear_max` 当作 white 会过早逐通道丢弃传感器余量，也会制造颜色断层。这两条都是已经
出现过的真实退化，不能重新引入。

## 2. 计划、颜色与一次性源准备

`src/raw/raw_frame_source_preparation.*` 和 `raw_frame_development_plan.*` 负责把 `RawFrame`
变成不可变的 prepared source：

1. 协商 requested/effective RAW plan；
2. 验证当前只接受 Bayer 2×2、orientation `0/3/5/6`，且没有尚不能执行的 DNG opcode；
3. 选择精确 DCP 或 provider generic matrix；
4. 从 as-shot 或绝对温度/色调编译 camera neutral 与四相位 CFA gain；
5. 在降采样与降噪之前，从全 RAW 采样一次 99% scene-linear luminance 作为校准统计；
6. 确定预览/诊断几何和 CFA 降噪计划；
7. 生成带 provider、plan、DCP、denoise、backend 和高光算法身份的 receipt/cache identity。

DCP 的 native camera matrix 与 CFA 白平衡需要成对绑定：白平衡在 CFA 侧执行后，矩阵输入
基需要乘回 camera neutral，保证未剪切样本与 DCP 标定一致。若 decoder 已提供完整
camera→linear-sRGB 矩阵，不能再把另一个坐标系中编写的 DCP HueSatMap/LookTable/ToneCurve
硬接到其后；匹配 DCP 此时只可提供可兼容的 neutral/矩阵能力。

普通 RAW 的一次性准备会保留降噪后的 CFA frame。温度/色调单独改变时，只重新编译颜色
binding 并发布新的不可变 warm session；不能重新打开文件、重新解码或重复降噪。质量、DNG
opcode、denoise、高光策略、源、foundation 或 optics 改变时，必须退出窄 rebind 路径，回到
完整源准备。

## 3. 两类高光证据必须分开

### 3.1 物理剪切 `SensorClippingMask`

物理归一化始终按当前 CFA 相位计算：

```text
sensor = (sample - black[site]) / (white[site] - black[site])
```

这里的 `white` 是编码白点。投影到显示方向的每个目标像素时：

- 任意所属 RAW 样本达到 white → `sensor_highlight_clipped`；
- 所有所属 RAW 样本不高于 black → `sensor_shadow_clipped`；
- R/G/B 三种 CFA 颜色在该重建 footprint 内全部达到 white →
  `sensor_shared_highlight_clipped`；
- 高 5 bit 保存共享剪切覆盖率，值为三个颜色中最小的
  `clipped_count / observed_count`，量化为 31 级。

原生输出的一个像素只直接覆盖一个 photosite，因此 shared fact 使用与 bilinear 相同的
3×3 CFA footprint；缩略输出已经有真实 area bin，直接使用该 bin。公开 bridge 只暴露
highlight/shadow 两个既有诊断 bit；shared bit 和高 5 bit 连续覆盖率保持为内部源算法证据。

### 3.2 连续色度风险 `HighlightChromaRiskMap`

色度风险不是另一张“需要修的空间 mask”，更不能当作膨胀半径。它回答的是：当前输出 bin
里的 CFA 通道比例还值不值得相信。

每个 photosite 先形成两种 shoulder evidence：

```text
response = smoothstep(0.84, 1.00, normalized_to_linear_response_limit)
balanced = 0.92 * smoothstep(0.78, 1.02,
                            normalized_to_physical_white * canonical_as_shot_gain)
evidence = max(response, balanced)
```

其中 canonical as-shot gain 为 `max(as_shot_neutral) / as_shot_neutral[site]`，不受用户随后
调整的白平衡影响。它用于识别“RAW 容器中红蓝还没到 white，但中性灯经过相机白平衡后
三个颜色已经属于同一亮表面”的 CFA 相位齿。

一个输出 bin 对 R/G/B 分别求平均 evidence，排序为 `e0 <= e1 <= e2`：

```text
disagreement = e1 * smoothstep(0.02, 0.35, e2 - e0)
shared_terminal = smoothstep(0.75, 0.98, e0)
risk = max(disagreement, shared_terminal)
```

`disagreement` 要求至少两个独立颜色失去余量，并保护一色发光体；`shared_terminal` 处理
三个颜色一起压平、但没有通道不一致可供前项发现的太阳核心。结果编码为 R8 `0...255`。

普通 CFA 主路径在完成 source-owned 修复后会把 risk sidecar 标记为 consumed，并把原始
shoulder topology 压缩为仅存在于 exact shared-terminal coverage 的弱残余置信度：

```text
residual = source_risk * shared_coverage^2 * 0.25
```

它不做膨胀；coverage 为零的 any-channel 边界必为零。后续 Selective Tone 只会在用户真的
负向恢复高光时，用这条至多约二分之一 Oklab 色度拉回的核心信号抑制残留色染，不能重画
原来的风险边界。显式 `disabled` 诊断路径仍可能把未消费风险交给后续阶段。

## 4. 默认 CFA 高光算法

### 4.1 归一化、白平衡和 fp32 余量

每个 CFA 样本先扣对应黑点并按物理 white 归一化。若选择了 CFA 白平衡，则乘以四相位 gain
以及 `1 / min(gains)` 的公共尺度。这个值可以大于 `1.0`。

达到物理 white 不代表应把白平衡后的值截回 `1.0`。默认策略保留所有末端相位的 fp32
白平衡余量，并用独立 physical-white topology 记录“这里已无更多实测响应”。过去只保留
共享三色核心或把孤立相位投到公共 ceiling，都会在 Bayer 相位边缘制造空间台阶。

### 4.2 局部 opposed photosite estimate

对候选坐标 `(x, y)`，在其 3×3 邻域按 CFA 颜色求白平衡后、尚未修复的均值。当前颜色为
`c`，另外两个颜色均值为 `a`、`b`，opposed reference 为：

```text
reference = ((cbrt(a) + cbrt(b)) / 2)^3
```

这沿用了 darktable opposed-colour 重建的正确所有权边界：在去马赛克之前估计当前损坏的
photosite。Darktable 的第二项不是空间模糊，而是 factual clipping 邻域中“实测当前颜色减去
局部 opposed reference”的低频均值。Shadow 在 source preparation 时以 stride 4 建立一个
每色最多 16384 条记录的相位分解 sidecar；每条只保存当前 sensor 值与 3×3 四相位总和/计数。
温度或色调改变时，使用新的四相位 gain 在这个有界 sidecar 上重新求三色 offset，不重新扫描
RAW，也不复制完整图像。

原生重建中，只有 physical-white 归一化值至少 `0.987` 的 photosite 成为 terminal candidate。
provider 的逐相位 `linear_response_limits` 仍用于连续 highlight-risk 置信度，但不再扩大
重建写入所有权。编码值继续以 coding white 归一化并保留其场景亮度，因此 exact-white
photosite 的单向修复不会把白平衡产生的 fp32 余量压成 1.0：

```text
reconstructed = max(measured, reference + cached_chrominance[channel])
```

offset 只在同一个 terminal gate 内生效，不产生膨胀 mask、RGB 羽化或第二条边。操作仍严格
单向；若一色发光体的实测通道本来更高，它完全不变。stride 4 的模型可由 probe 同 full
oracle 统计并排报告，以便继续量化采样误差，而不是靠截图猜参数。

`BayerCfaSamplingPolicy::terminal_highlight_admission` 把 gate 的归一化域变成显式策略，但
默认值固定为 `physical_white`。`linear_response_limit` 保留为离线
`--highlight-threshold-ablation` 的显式对照：两支共享同一 `RawFrame`、白平衡、已编译
chrominance offset、area sampler 和 camera matrix，只替换上述 gate 的归一化分母。这个枚举
不是 Recipe 参数，也不进入桌面 UI。生产 CPU/Metal、preview/detail/export 使用同一个
physical-white gate；最窄 highlight identity 已更新，因此旧缓存不会冒充新语义。

### 4.3 缩略 area preview 的特殊顺序

缩略预览不能先把一个 area bin 平均成 RGB，再决定是否修复。当前实现对 bin 中每个 CFA
photosite 先走与 point/detail 完全相同的 terminal gate：只有 physical-white 归一化值至少 `0.987`
的 photosite 才能进入 opposed estimate，并且只允许单向抬高：

```text
reconstructed = measured < 0.987 ? measured : max(measured, reference)
```

然后用目标像素与各 source photosite 的真实几何 overlap 作为权重，R/G/B 分开累加。
`response evidence` 和 physical-white coverage 仍作为诊断 sidecar 计算，但默认路径不再把
它们变成第二个写入 mask，也不再拆出或中和 RGB damaged layer。结果有三个关键性质：

- 只改 terminal-gate photosite；
- 同一 preview bin 中未进入 terminal gate 的灯架和天空仍按原值积分；
- 斜边和不足一个源像素的覆盖天然抗锯齿，不需要扩张 mask。

这使生产 area 输出可逐点对照 Darktable opposed oracle：oracle 没有写入的 photosite，Shadow
也不能借由连续 shoulder evidence 或输出 RGB 色度混合间接修改。缩略所需的平滑只来自真实
几何面积积分，不来自扩大的颜色修复范围。

### 4.4 bilinear 与 high-quality detail

balanced/fast 原生路径在 3×3 内按颜色重建 bilinear camera RGB。high-quality 路径使用
directional green 加局部 colour-difference interpolation，demosaic halo 为 3 个 sensor
sites。

方向梯度一旦跨过物理剪切点，就可能锁定 Bayer 相位而不是场景边缘，表现为灯边横向毛刺。
因此 high-quality 只在高光 frontier 做额外 7×7 证据扫描；普通像素不付出该成本。只在亮侧，
根据参与颜色的 physical-white coverage 和亮度支持，把 directional 结果连续拉回已经计算好
的 bilinear 结果。这不增加 sensor read pass，也不抹掉暗边纹理。

### 4.5 相机域色度置信处理

去马赛克样本仍携带每个颜色的 response evidence 与 physical-white coverage，供诊断、风险
投影和显式 `aggressive` 路径使用。默认 CPU/Metal 输出不再依据这些 sidecar 对 camera RGB
做第二次 neutralization；默认写入所有权在 CFA terminal photosite reconstruction 结束。

只有显式 `aggressive` 诊断路径才计算下列空间色度项：

```text
two_channel = second_sorted_evidence
imbalance = max_evidence - min_evidence
bright_support = smoothstep(0.55, 0.85, max(camera_rgb))
disagreement_neutralization = two_channel * imbalance * bright_support

shared_neutralization = min(R_white_coverage,
                            G_white_coverage,
                            B_white_coverage)

aggressive_neutralization = max(disagreement_neutralization, shared_neutralization)
```

若显式启用，应用发生在 camera matrix 之前。相机域亮度使用
`0.25 R + 0.5 G + 0.25 B`，各分量只按 neutralization 比例向这个亮度靠拢，再乘
camera→scene-linear matrix。默认路径不会执行该步骤。

`aggressive` 只是历史诊断别名：它把 shoulder 提前到 `0.88`，并加入半径 3 的有界证据
feather。桌面没有单独开关，也不应把它重新做成默认；它曾经缓解粉色，却更容易扩大黄色
混色边界。

### 4.6 source 完成语义

普通 Bayer 的 preview、detail、export 在上述 CFA 采样器里已经完成修复。随后
`complete_cfa_owned_highlight_reconstruction()` 只做两件事：

1. 把 `source_surface_reconstructed` 标为 true；
2. 用对齐的 `SensorClippingMask` 把 R8 risk samples 收缩为
   `source_risk * shared_coverage^2 * 0.25`。

它不能再次写 scene RGB，也不能从 neighbourhood 推导所有权。这个步骤既防止后续恢复逻辑
重新解释物理剪切 mask，也为强力压暗时仍显露的一点 terminal 色染留下有界处理余量。

## 5. Camera RGB / AI foundation 的独立 fallback

若源已经不再保留 CFA photosite，就无法使用上面的精确写入所有权。仅这些兼容源可进入
`src/raw/clipped_highlight_reconstruction.*` 的 post-demosaic fallback：

- guide 最长边上限为 384；
- 可靠颜色 guide 排除物理剪切和高 CFA 风险样本；
- observed-light guide 单独保留局部灯光形状；
- push-pull 中实测 boundary seed 始终固定，推断单元不能成为新的 seed；
- 只有 shared three-colour clipping coverage 能驱动低频亮度表面；单/双通道剪切不能；
- risk-owned 未剪切像素最多修复色度，亮度保持实测；
- dark-edge 与局部亮度 gate 阻止写入跨到暗灯架或轮廓外；
- shared core 的亮度使用局部锚定、单调的 logarithmic shoulder；
- 完成后同样消费 risk，避免 Selective Tone 第二次描边。

这个 fallback 的缓存身份目前仍为 `clipped-highlight=boundary-propagated-scene-shoulder-v17`。
它不是普通 Bayer v18 CFA 路径的一部分，不能为了复用代码而重新接回普通 RAW。

## 6. 后续 Highlights / Whites 为什么不会把灯压成灰盘

`src/edit/guided_selective_tone.*` 在以 18% 灰为固定原点的 scene EV 上工作。引导 mask 在
level-zero 使用半径 48，并做两次 box pass；跨边界平滑属于调色 mask，不拥有 RAW 修复。

正向 Highlights/Whites 仍使用开放式 soft hinge。负向恢复使用两个有限、C2 连续的
quintic shoulder，而不是叠加两个无界 hinge：

| 控件 | 起点 | 跨度 | -100 最大预算 |
| --- | ---: | ---: | ---: |
| Highlights | `0.0 EV` | `3.4 EV` | `0.55 stop` |
| Whites | `0.3 EV` | `5.5 EV` | `0.70 stop` |

这使极限组合仍有效，但不会把所有 super-white 值压进很窄的一段，或把太阳/灯芯变成中灰。
quintic window 在两端导数回到零，超高光最终恢复 1:1 局部斜率，所以不同的 scene-linear
头部能量仍可区分。

只有 source risk 非零时，负向恢复才在 Oklab 中降低 `a/b`：恢复量超过 `0.05 EV`
后，在 `0.50 EV` 内平滑达到强度，色度拉回系数为：

```text
chroma_pull = recovery_smoothstep * sqrt(source_risk)
```

普通 CFA 默认路径已经在 source stage 消费 broad risk，只保留 exact shared-terminal 的弱
残余置信度，因此不会再被二次描边。CPU 与 resident Metal 使用相同常数和公式。

可选的 R/G/B 高光通道修复仍是同一 Selective Tone pass 内的逐像素操作，但它不能只看
非零残余 risk：某些经过 source repair 的大面积亮云或灯面已经把 risk 消费为零，显示颜色
仍可能需要人工收尾。因此它先确认会话确实带有 RAW 高光 evidence，再取下列支持度的较大值：

```text
support = max(sqrt(source_risk), smootherstep(source_EV, 1.25 EV, 3.00 EV))
```

亮度门取 tone 之前的不可变 scene-linear source，因此 Highlights/Whites 压暗后不会让选区
突然消失；它不做 blur、dilation 或邻域读取。每个被选通道也不再整体乘 gain，而只削减高于
另外两通道 cube-root mean 的正向 excess：

```text
opposed_c = ((cbrt(other_0) + cbrt(other_1)) / 2)^3
out_c = in_c - max(0, in_c - opposed_c) * authored_relative_c * support
```

三条滑块的共同分量先被减掉，所以相等的 R/G/B 值仍是精确 no-op；削减后恢复原 Oklab L，
亮度仍归 Highlights/Whites 所有。由于写值不会跨过 opposed reference，红色抑制不会把亮面
翻成青色。普通非 RAW 图像没有 evidence surface，即使像素很亮也不会进入这条路径。

## 7. CPU、Metal 与交互性能合同

CPU 是可读参考，Metal 必须数学对等：

- `bayer_sampling.cpp` ↔ `metal_raw_common_msl.hpp`：归一化、WB 余量、opposed estimate、
  response evidence、相机域 neutralization；
- `raw_frame_region_development.cpp` ↔ `metal_raw_reconstruction_msl.hpp`：bilinear、
  edge-aware、方向 frontier fallback、area preview、方向映射和 camera matrix；
- `sensor_clipping.cpp` ↔ Metal clipping projection：flags 和共享覆盖率；
- `guided_selective_tone.cpp` ↔ `warm_edit_gpu_msl.hpp`：负向有限 shoulder 与可选 risk 去色。

一次性 Metal 路径分块输出，可把 CFA denoise、重建、DCP continuation 和 clipping projection
放进一次 transaction。resident RAW 让 sensor/DCP buffer 常驻，细节请求只开发精确 source
preimage，并保留同样的 demosaic/denoise halo；不能为了算法方便读回完整 fp32 RGB。

交互最短路径如下：

- 普通 Recipe 滑杆：复用不可变 scene-linear source 和 R8 evidence buffer，只重跑受影响的
  edit stages；
- 仅温度/色调：复用已经降噪的 CFA frame 和 resident Metal source，只重新编译颜色 binding、
  在有界 opposed sidecar 上重算三个 offset，再执行 CFA 重建及后续必要阶段；不扫描整张 RAW；
- 仅 AI strength：复用原始与 full-strength Camera RGB bases；
- 质量、降噪、高光策略、source/foundation/optics：缓存身份改变，回完整 source preparation；
- 新请求可以取消旧 preview，但不能覆盖已发布 session 的不可变输入。

任何“修复高光”改动若新增 slider-time decode、完整帧 CPU↔GPU copy、完整帧 guide、第二次
scene-RGB pass 或禁止 resident detail，都应先被视为性能回归，而不是画质改进。

## 8. 当前版本身份

这些字符串是缓存和诊断协议，不是装饰性注释：

| 合同 | 当前身份 |
| --- | --- |
| `RawFrame` schema | `2026082201` |
| Shadow RawFrame developer | `2026082202` |
| sensor clipping mask schema | `3` |
| highlight chroma risk schema | `5` |
| 默认 CFA 高光 | `sensor-highlights=cfa-opposed-point+cached-chrominance@20260826.1` |
| 默认 recovery | `local-opposed+cached-global-chrominance` |
| 头部余量 | `physical-white-wb-fp32` |
| ordinary clipped highlight | `clipped-highlight=cfa-opposed-physical-white-chrominance-v24` |
| aggressive 诊断 | `cfa-opposed-cached-chrominance-feathered@20260826.1` |
| Camera RGB fallback | `clipped-highlight=boundary-propagated-scene-shoulder-v17` |

完整 `pipeline_identity` 还包含 provider/version、requested/effective plan、backend、denoise、
DCP catalog/profile/status 和 source-stage identity。只要输出语义可能变化，就必须更新最窄的
相关身份；改变 `RawFrame` 字段或语义还必须同步 private plugin ABI/token。

## 9. 源码所有权地图

| 责任 | 所有者 |
| --- | --- |
| 公共/私有 provider 路由 | `src/decoder/photo_decoder_router.cpp` |
| LibRaw 元数据与未处理平面 | `src/decoder/libraw_decoder.cpp` |
| provider-neutral sensor 合同 | `include/shadow/image/raw_frame.hpp` |
| 私有插件握手 | `include/shadow/image/private_decoder_plugin.hpp` |
| 计划、WB、矩阵、DCP、receipt | `src/raw/raw_frame_development_plan.*` |
| 一次性源准备与 pipeline identity | `src/raw/raw_frame_source_preparation.*` |
| 物理 mask 与连续风险 | `include/shadow/image/sensor_clipping.hpp`, `src/raw/sensor_clipping.cpp` |
| CFA opposed / bilinear / edge / area | `src/raw/bayer_sampling.*` |
| CPU full/region 变换 | `src/raw/fused_raw_development.cpp`, `raw_frame_region_development.*` |
| Metal 对等算法 | `src/raw/metal_raw_common_msl.hpp`, `metal_raw_reconstruction_msl.hpp` |
| 一次性与 resident Metal transaction | `src/raw/metal_raw_reconstruction.mm`, `metal_resident_raw_source.mm` |
| 普通 source 完成 / fallback 表面 | `src/raw/clipped_highlight_reconstruction.*` |
| 白平衡交互 rebind | `src/raw/raw_preview_rebinding.*` |
| Highlights / Whites | `src/edit/guided_selective_tone.*`, `src/proxy/warm_edit_gpu_msl.hpp` |

## 10. 已知错误方向与症状

| 错误改法 | 典型症状 | 原因 |
| --- | --- | --- |
| 用 `linear_max` 当 white | 分通道过早截断、粉色顶边 | 响应末端被误当物理编码末端 |
| WB 后把各相位截到 `1.0` | Bayer 相位台阶、毛刺 | 把颜色增益变成空间剪切 |
| 只在精确 white 才降色度 | 极限拉高光时出现粉色圈 | 末端 shoulder 已失真但风险直到最后一个码值才出现 |
| 对 risk/mask 做空间膨胀后写 RGB | 黄色/灰色扩边 | 候选支持被误当写入所有权 |
| 在 area average 后修整个像素 | 灯边多出一圈平行条纹，大片天空出现灰/暖硬切 | 同一个 bin 的可靠内容被当作受损亮面一起重写 |
| 普通 Bayer 再跑 RGB surface fallback | 双轮廓、暗岛或亮穹顶 | CFA 修复被后续阶段重复解释 |
| 对一通道 clip 直接中性化 | 彩色灯被洗白 | 一色高光仍有可靠颜色语义 |
| opposed 修复允许向下写 | 灯芯变暗、额外断层 | 用估计值覆盖了仍有效的实测能量 |
| scene-global chroma offset 直接进生产 | 末端色环被放大 | 全局偏移跨越了局部 CFA 不连续 |
| CPU 改常数但 Metal 未改 | 预览/导出或设备间不一致 | 两条主路径失去同一数学合同 |
| 算法变了但身份没变 | 旧缓存“随机复现”旧症状 | 不同输出语义共用 cache key |

## 11. 修改与验证清单

改动前先明确属于哪一层：RAW 字段解析、CFA evidence、photosite estimate、demosaic、
camera-domain neutralization、Camera RGB fallback，还是后续 tone。一次改动不要跨层调参来掩盖
另一个层的错误。

### 11.1 数值诊断

针对问题边缘记录同一坐标链，而不是只看最终截图：

```text
raw(x,y), CFA colour, sample, black, white, linear_response_limit
physical_normalized, response_normalized, CFA WB gain, fp32 measured
highlight evidence, physical-white fact, opposed reference, reconstructed
area overlap weight / bilinear or directional estimate
camera RGB before/after neutralization
scene-linear RGB, source risk consumed state
Selective Tone mask EV, requested EV, recovered EV
```

至少同时查看：剪切核心、亮面内侧、亮暗边界、暗物体外侧，以及一色发光体。邻域候选可以
跨过边界读取，但最终 write set 必须仍等于 evidence-owned photosite/output contribution。

### 11.2 合同测试

先构建改变过的 native target，再运行对应 CTest；不能把旧二进制的通过当作当前源码证据。
高光主路径至少覆盖：

```text
shadow-image-raw-frame-contract
shadow-image-sensor-clipping-contract
shadow-image-bayer-demosaic-contract
shadow-image-fused-raw-cpu-development-contract
shadow-image-fused-raw-highlight-treatment-contract
shadow-image-fused-raw-metal-execution-contract
shadow-image-raw-frame-development-plan-contract
shadow-image-raw-preview-rebinding-contract
shadow-image-resident-raw-source-contract
shadow-image-metal-resident-raw-source-contract
shadow-image-full-edit-detail-metal-raw-contract
shadow-image-guided-selective-tone-contract
shadow-image-warm-edit-gpu-contract
```

核心断言必须继续包含：

- area repair 单向且不降低任何输出亮度；
- 混合亮灯/暗灯架的 area bin 不扩大 neutral write；
- 一色饱和红等发光体与 `disabled` 精确一致；
- 两通道 shoulder 在 hard clip 前连续降低假色；
- shared plateau 消除不受支持的 WB 色度；
- high-quality 在剪切 frontier 放弃不可靠 directional phase；
- CPU/Metal 在 fp32 容差内对等；
- source 已修复时 Selective Tone 不二次去色；
- 未消费风险只在负向恢复时逐步去色；
- 极限 Highlights + Whites 保持单调、保留 midtone 与 super-white 能量；
- 白平衡 rebind 不进行第二次 decode/denoise，并继续使用 resident Metal source。

### 11.3 实图验收

合成测试之后仍要用两类本地、非入库 RAW 做显示尺寸验收：

1. Nikon 暖色灯具：检查灯面/暗框边缘无粉圈、无黄色平行扩边、无横向 CFA 毛刺；
2. Sony 太阳：检查共享剪切核心到天空的过渡，无粉色 halo，极限恢复仍保留太阳能量。

每类都比较默认与 `disabled`、CPU 与 Metal、面积预览与 100% detail，并在 Highlights、Whites
分别为 `0`、中间负值和 `-100` 时检查连续性。Capture One 可以作为视觉参照，但验收结论
必须落到上述可测不变量，不能通过猜测其私有实现来改 Shadow。

### 11.4 发布前

1. 运行共享工作区 guard，使用任务私有的外部 Cargo/CMake build 目录；
2. 同步 CPU/Metal、一次性/resident、preview/detail/export；
3. 更新最窄 cache/receipt/schema identity；
4. 更新并构建对应测试 target，再运行 CTest；
5. 完成桌面启动、交互编辑和实图 smoke；
6. 只有 canonical-debug steward 才能通过 `scripts/promote_debug_build.sh` 提升 Debug。

若一个改动只能消除粉色却产生黄色条纹，或消除黄色条纹后粉色回来，首先检查“候选支持”
是否又被当作“写入所有权”，以及 area preview 是否又变成了平均后整像素处理。不要继续在
RGB 输出上叠加另一个补偿层。
