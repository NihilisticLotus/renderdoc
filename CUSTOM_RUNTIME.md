# 本机 RenderDoc 修复与验证（2026-09-23）

## 日常使用

运行桌面的 **RenderDoc Custom**，或 `C:\RenderDocCustom\qrenderdoc.exe`。
桌面快捷方式已设为以管理员身份启动；直接打开 EXE 时也建议以管理员身份运行。
在 Launch Application 中载入游戏设置，确认可执行文件和工作目录后直接点击 **Launch**。
这条路径使用标准 `ExecuteAndInject`，不要求先启用 Global Hook，也不需要手工写注册表、启动脚本或逐个修改 UE 工程。
游戏进入可渲染画面后，可用 RenderDoc 的 **Capture Frame(s) Immediately** 或游戏内 F12 截帧。
Global Hook 仍保留为显式的诊断/兼容入口，但不是标准 Launch 的前置条件。

FFXVI 例外说明：该游戏二进制硬编码拒绝 `renderdoc.dll`、Nsight/PIX 等捕获模块名，
检测后会主动调用 `ExitProcess`。构建输出会自动生成与 `renderdoc.dll` 字节一致的
`rdoc.dll`；标准 Launch/ExecuteAndInject 在目标进程中加载中性文件名，宿主 API 仍使用
`renderdoc.dll`，不需要用户设置环境变量或修改游戏目录。Global Hook 的 shim 也优先使用
同目录的 `rdoc.dll`。因此部署时必须同时保留 x64 和 `x86` 子目录中的两个 DLL。

Steam 是单实例启动器：如果 Steam 已经在运行，标准 Launch 会先把本次 RenderDoc
注入到现有 Steam 根进程，再提交 `-applaunch` 请求；这样不会落回旧 Steam 进程的旧捕获路径。

鬼武者已通过游戏菜单将“超分辨率技术”设为“不使用”，保留 3840×2160、100% 渲染比例、
TAA，以及原本关闭的帧生成。该设置已保存，下次截帧无需重复调整。
这是当前 DLSS 捕获缺失的兼容方案，并不代表已实现 DLSS 回放；重新启用 DLSS 后，
本次验证过的场景仍存在最终输出黑屏的问题。原生 4K 的 GPU 开销会高于 DLSS Quality。

源码与构建输出在 `E:\RenderDocProject\renderdoc`；日常运行副本在 `C:\RenderDocCustom`。
编译后执行 `deploy_custom_runtime.ps1` 更新运行副本。部署前关闭使用该运行副本的程序；编译时不必关闭。
脚本在复制前检查目标文件是否被占用，不会自动结束游戏或编辑器。

## 已有证据支持的故障

0. **标准 Launch 的两个独立问题。** 鬼武者等 Steam 游戏存在两类启动行为：
   Steam 叠加层可能已经把 DXGI 导出改成 E9 跳转；部分游戏还会先启动一个 bootstrap，
   随后退出或转交给安装目录内的真实进程。旧实现把任意 E9 误认为 RenderDoc 自己的补丁，
   并且只注入初始 PID，所以会出现“启动崩溃”或“进程运行但没有注入”。
   现在 `ApplyExportPatch` 只把自己记录过的补丁当作幂等命中；遇到已有外部 E9 时保留其
   目标作为调用链，再安装 RenderDoc 自己的跳转，并防止 DXGI 工厂重复包装。
   `LaunchAndInjectIntoProcess` 还会从 Steam manifest 自动补充对应 AppID 环境变量，
   在初始注入成功后监视同镜像替换进程或同一安装目录的 Steam 子进程，并将同一捕获参数
   自动转交给真实进程。这是通用流程，不包含鬼武者、FFXVI 或卡赞的专用分支。
   证据：`work\normal_steam_overlay_proof.log`、`work\normal_steam_crash_renderdoc.log`、
   `work\normal_steam_fixed.log`；最终 C: 运行副本的 qrenderdoc 日志还记录了
   `RenderDoc_2026.09.23_15.31.13.log` 中的 manifest 检测、PID `52856` 注入和目标握手。

1. **Global Hook 路径要求。** 本机 E: 禁用了 8.3 短文件名，标准
   `RENDERDOC_StartGlobalHook` 从 E: 构建目录运行会明确返回 short paths disabled。
   C: 启用了短文件名；将完整运行副本部署到 C: 后，同一标准 API 成功。
2. **GUI 管理员误判。** 原实现用能否写 HKLM\SOFTWARE 判断是否提升权限。
   实测令牌已提升、实际 AppInit 子键可写，但父键的广泛写权限被拒绝。
   `QRDUtils.cpp` 改用 `TokenElevation` 检查真实令牌。没有修改系统 ACL 或安全设置。
3. **捕获成功但回放挂起。** DRED 定位到曝光直方图的 `CompactHistogramCS`。
   原捕获的常量缓冲布局不匹配，回放返回 `DXGI_ERROR_DEVICE_HUNG`。
   在 GPU_UPLOAD 缓冲 Unmap 时，CPU 与 GPU 回读的 89,478,485 字节完全一致；
   随后提交时的前 256 KB 与 Unmap 快照相差 6,257 字节，各次提交快照一致。
   Map/Unmap 跟踪没有遗漏嵌套映射；提交前发生的后续内存变化没有进入原捕获。
   `d3d12_command_queue_wrap.cpp` 现在在捕获期间补充读取被提交引用、已取消映射的
   GPU_UPLOAD 缓冲，将数据写入同一命令提交记录。持久映射缓冲继续走原有差分路径。
   此兼容处理会增加截帧时的 GPU 回读与数据量，不影响未截帧时的执行路径。
4. **构建输出被占用。** 使用构建目录运行 GUI/注入 DLL 会令 Windows 锁住该 DLL。
   已将 GUI、全局 Hook、文件关联、Vulkan 注册及 UE 默认注册路径切换到 C: 运行副本。
   在 GUI、Hook 和游戏同时运行时，E: 构建 DLL 已通过独占读写打开检查，并成功构建。
5. **DLSS 场景回放黑屏。** GPU_UPLOAD 修复后，开启 DLSS 的场景捕获可加载，但最终画面为黑色。
   在 `Onimusha_GUI_GlobalHook_Scene_frame26949.rdc` 中，事件 11262 的场景颜色资源 18069
   有正常内容；事件 11304 清零了 3840×2160 的资源 444347，后续没有记录生成其内容的
   超分辨率调度。事件 11347 的 `PreTonemap2_PS` 读取该黑色纹理，黑色沿后处理传到最终输出。
   游戏菜单确认使用 NVIDIA DLSS 310.5.2，帧生成关闭。仅将超分辨率技术切换为“不使用”，
   用相同部署的 DLL 和普通 GUI Global Hook 再次捕获后，真实 3D 场景正确回放。
   证据定位到 DLSS 工作未被完整记录，尚未证明具体缺失的是哪个 NVIDIA 私有接口。

### 2026-09-25 标准 ExecuteAndInject 回归

使用 E: Development 构建的 `renderdoccmd.exe`（与 qrenderdoc 共用同一 `renderdoc.dll`），
通过同一 `RENDERDOC_ExecuteAndInject` 路径重新验证，未启用 Global Hook、未向游戏目录复制 DLL：

- FFXVI：`captures\ffxvi_clean_standard_validation_frame1013.rdc`，780,431,013 字节；
  D3D12 presenting 成功，缩略图为 Square Enix 启动画面，进程未崩溃。
- 鬼武者：`captures\onimusha_standard_validation_frame589.rdc`，301,307,393 字节；
  D3D12 presenting 成功，缩略图为着色器优化界面。
- 第一狂战士卡赞：`captures\khazan_standard_validation_frame728.rdc`，335,345,858 字节；
  `KZ.exe` 的 bootstrap 与 `BBQ-Win64-Shipping.exe` 子进程控制端均连接成功，
  缩略图为补丁说明界面。

这些是“注入、图形拦截、触发截帧”证据，不把启动画面/补丁说明误称为主菜单；要验证主菜单，
必须在游戏自身完成首次着色器/公告初始化后再输入确认键。冷启动对照中，Steam 根进程残留
`rdoc.dll` 会让后续直接启动继承旧注入状态，因此诊断前应清理整个 Steam 进程树。

## 系统接入

- 原 1.45 MSI 已卸载；卸载注册项复查未发现 RenderDoc 1.45。
- 桌面快捷方式原件保存在 `E:\RenderDocProject\work\RenderDoc Custom.before-elevation.lnk`。
- HKLM\SOFTWARE\Classes\RenderDoc.RDCCapture.1\DefaultIcon 指向
  `C:\RenderDocCustom\qrenderdoc.exe`，.rdc 打开命令使用同一程序。
- x64 / x86 Vulkan 隐式层分别注册到 C: 运行副本及其 x86 子目录。
- 检查的 UE 插件会从上述 DefaultIcon 的父目录加载 renderdoc.dll。
  工程若自行显式配置 `renderdoc.BinaryPath`，该配置仍会优先于系统默认路径。
- `start_global_hook.ps1` 旧的手工 AppInit 操作已停用；该入口只打开标准 GUI。

## 验证与边界

### 本轮普通启动与 F12 验证

- Steam 正常启动 PID 62388：成功创建并包装交换链，进入 3D 场景。
  保持 Steam 叠加层加载，直接按 F12 生成
  `E:\RenderDocProject\captures\Onimusha_Steam_F12_frame17486.rdc`，
  4,265,589,700 字节。未通过目标控制 API 触发这次捕获。
- 该帧回放成功：2,401 个 action、327 次 draw、610 次 dispatch、5,887 张纹理，
  最后事件 12331。最终 3840×2160 画面已导出为同名 `.rdc.png`，
  与 `.rdc.thumbnail.png` 对照，木桥、人物、树林、水面及雾效正常。
  日志：`work\normal_steam_f12_replay.txt` / `.log`。
  日常 GUI 也成功打开此帧，状态栏为 `loaded. No problems detected.`。
- 在资源管理器直接双击 `OnimushaWotS.exe`，游戏自动转交 Steam 重启为 PID 60188，
  成功注入且无启动崩溃。再次按 F12 得到
  `E:\RenderDocProject\captures\Onimusha_Explorer_F12_frame3573.rdc`，
  319,942,305 字节；回放成功（204 actions、53 draws、60 dispatches，881 textures），
  画面为自动存档说明界面。日志：`work\normal_explorer_fixed.log`、
  `work\normal_explorer_f12_replay.txt` / `.log`。
- 两次游戏均正常退出，Steam 记录退出码 0。
- 最终部署后，再由 Steam 界面点击“开始游戏”，PID 58800 同样正常进入自动存档
  说明画面并显示 RenderDoc F12 提示；日志 `work\normal_steam_button_final.log`。
- x64 / Win32 Development 构建通过；最终 x86 运行 DLL 也已重新部署。
  游戏、GUI、Global Hook 同时运行时，E: 构建 DLL 独占读写检查通过。
- 权限复核：本机正常 Explorer、由 Explorer 启动的 Steam 与 RenderDoc 的令牌
  均为 elevated；这是现有系统状态。没有为游戏修改兼容性标记或系统安全配置。
- 收尾时游戏已退出，Global Hook 通过 GUI 关闭，x64 / x86 的
  `LoadAppInit_DLLs=0`，两个 Hook helper 均已退出。GUI 留在本轮场景帧。

### 前一轮验证记录

首份成功回放：`E:\RenderDocProject\captures\Onimusha_SubmitFix_frame2112.rdc`。
Replay API 返回成功，统计为 204 个 action、51 次 draw、61 次 dispatch、898 张纹理；
最终呈现纹理导出为同名 `.rdc.png` 并已检查。该帧显示自动存档说明界面。
日志保存在 `E:\RenderDocProject\work\onimusha_first_replay_success.log`。

**前一轮真实场景验证（不代表普通 Steam 启动验证）：**
`E:\RenderDocProject\captures\Onimusha_GlobalHook_DLSSoff_frame38248.rdc`

- 文件大小 4,146,150,714 字节（约 3.86 GiB）。
- 普通 GUI Enable Global Hook 注入，但游戏通过设置 Steam 环境变量后直接启动；
  目标 API 确认 D3D12 presenting=1、supported=1。
- Replay API 无 SDK 路径覆盖，返回成功：3,088 个 action、180 次 draw、675 次 dispatch、
  5,812 张纹理，最后事件 14432。
- 同名 `.rdc.png` 为回放导出的 3840×2160 最终画面；`.rdc.thumbnail.png` 为捕获时嵌入的画面。
  已目视对照，树林、河流、天空和雾效吻合。不同于此前 DLSS 开启时的全黑最终输出。
- 日常 `C:\RenderDocCustom\qrenderdoc.exe` 也成功打开该帧，Texture Viewer 显示正常场景，
  状态栏为 `loaded. No problems detected.`，当前留在该帧供检查。
- 验证日志：`work\onimusha_dlss_off_capture.txt`、`work\onimusha_dlss_off_replay.txt`、
  `work\onimusha_dlss_off_replay.log`（均位于 `E:\RenderDocProject` 下）。
- 调试回放仍有 descriptor heap 无法进一步扩容的警告；本次最终画面回放成功，
  尚未逐一验证 Shader Debug、像素历史等额外分析功能。
- 游戏已正常退出，本次 Global Hook 已经 GUI 关闭。x64/x86 的 LoadAppInit_DLLs 均为 0，
  AppInit_DLLs 无有效路径，两个 Hook helper 均已退出。

游戏配置备份：`E:\RenderDocProject\work\onimusha_config_before_final_dlss_ab.ini`。
关闭游戏后如需恢复原图像设置，可将备份复制回
`E:\SteamLibrary\steamapps\common\OnimushaWotS\config.ini`。
前后文件对比仅两项变化：`UpscaleTypeOption=DLSS` → `None`、
`SelectedPreset=PCHighest` → `PCCustom`（游戏因单项更改自动标记为自定义）。

本轮修复后的 x64 构建输出与 C: 运行副本的 `renderdoc.dll`/`rdoc.dll` SHA256 一致：
`F7BC3BF8CA5355B9B7FEF680602363145541D1915ABCB4CE82A67B42115C3C4E`。

这些结论定位并修复了本机可复现的问题。没有取得另一台电脑的磁盘、权限、GPU 与
驱动证据，因此不能声称已经证明两台电脑的唯一差异。没有证据表明需要重装 VC++
运行库或关闭系统安全功能。游戏 DLSS 的一次性兼容调整及其验证见上文。
调试时下载的匹配 D3D12 SDK Layers
仅用于定位回放故障，并未替换游戏文件或安装系统组件。

### 标准 Launch 跨游戏验证

本轮重新以同一个中性运行副本验证：

- FFXVI：`captures\ffxvi_reuse_capture2_frame1652.rdc`，约 799 MB，Steam 已运行时仍能
  返回目标控制、完成 D3D12 presenting，缩略图为 Creative Studio 画面，进程未崩溃。
- 鬼武者：`captures\onimusha_standard_neutral_frame416.rdc`，约 302 MB，缩略图为正常
  的寺院/树林场景。
- 第一狂战士卡赞：`captures\khazan_standard_neutral_frame29.rdc`，约 641 MB，先发现
  `KZ.exe` 后连接 `BBQ-Win64-Shipping.exe` 子进程，缩略图为正常加载画面。

这些捕获均由标准 `ExecuteAndInject` 触发，未使用按游戏名称分支的脚本注入。

- **鬼武者**：通过 qrenderdoc 的标准 Launch 启动 Steam 入口，日志记录初始 PID
  `60376` 后检测到替换 PID `61344` 并再次注入；目标控制连接建立后成功截帧。
  普通 Steam 与资源管理器直接启动的 F12 文件分别为
  `captures\Onimusha_Steam_F12_frame17486.rdc` 和
  `captures\Onimusha_Explorer_F12_frame3573.rdc`，前者回放为正常 4K 木桥、人物、
  树林、水面和雾效画面。
- **最终幻想 XVI**：标准 `ExecuteAndInject` 返回成功并建立 D3D12 presenting，
  生成 `captures\FFXVI_StandardLaunchProbe7_frame2.rdc`（约 143 MB）。该次截帧发生在
  启动黑屏阶段，证明注入和捕获链路，不作为游戏场景画面质量结论。
- **第一狂战士卡赞**：初始 KZ bootstrap PID `55404` 后自动发现真实子进程
  `BBQ-Win64-Shipping.exe` PID `48160` 并完成二次注入，生成
  `captures\Khazan_StandardLaunchProbe3_frame2.rdc`（约 5.7 MB）。同样是在启动阶段
  截帧，缩略图包含 RenderDoc 捕获提示；它验证的是通用 bootstrap handoff，而非场景画面。

这三项使用同一套标准 Launch 实现，没有给某个游戏增加名称判断或额外启动步骤。

### 2026-09-25 追加的独立证据与边界

- 重新构建了 E: Development 的 `renderdoc.dll`、`rdoc.dll` 和 `qrenderdoc.exe`，并部署到
  `C:\RenderDocCustom`；三者运行副本与构建副本一致，当前 SHA256 前缀为
  `61B2BFF7F2AF8EAC2E180A8CAF5CF442342177CBEBF2613BAF8A97F8FF...`。
- FFXVI 的同一套标准 `ExecuteAndInject` 产生
  `captures\ffxvi_clean_standard_validation_frame1013.rdc`（780,431,013 字节），目标模块明确
  为 `C:\RenderDocA\probe\rdoc.dll`，D3D12 presenting=1，回放缩略图为 Square Enix 启动画面，
  进程未发生启动崩溃。
- 鬼武者的标准注入产生
  `captures\onimusha_standard_validation_frame589.rdc`（301,307,393 字节），回放缩略图为
  游戏着色器优化界面；F12 在目标进程中使 HUD 的捕获计数从 1 增到 2，说明控制端和帧触发均有效。
- 卡赞的标准注入已进一步用 F12 在真实目标子进程生成
  `captures\khazan_menu_validation2_frame136027.rdc`（428,964,834 字节），回放缩略图明确显示
  中文主菜单“新游戏 / 继续 / 加载游戏 / 绝命征途 / 制作人员 / 设置 / 结束”。
- 本次没有把启动画面、着色器优化界面或自动存档提示误称为主菜单：FFXVI 和鬼武者的当前证据
  仍是注入/截帧证据，不是主菜单证据。鬼武者在自动存档提示上拒绝了脚本合成的 F/Enter/鼠标
  输入；该现象发生在游戏 DirectInput/Steam 输入层，不能据此改写 RenderDoc 注入链。
- 本次构建首次报 LNK1201 时，证据是遗留 `BBQ-Win64-Shipping.exe` 仍加载 `rdoc.dll`；结束该
  精确子进程后两个项目均成功构建。不能只结束 KZ 父进程就假设所有 RenderDoc 使用者都已退出。

临时 GPU 内存转储、导出函数跳转日志及 Dispatch 标记均已从最终源码移除。
