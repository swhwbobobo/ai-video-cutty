# AI Video Cutty · Windows 播放原型

原生 C++ / Win32 / Media Foundation / Direct3D 11 / WIC。目标为 Windows 10 与 Windows 11 x64，不依赖 .NET、WebView2、Python 或 FFmpeg，不注册后台服务，不改系统文件关联。

这是播放验证原型。编译通过与 Windows 10/11 实际播放通过是两种不同证据；当前状态以 PR/构建结果为准。未完成双系统实测前，不宣称兼容性和性能已验收。

## 打开和操作

直接运行 `AIVideoCutty.exe`，点“打开…”或拖入一个本机视频/音频。也可以传入文件路径：

```powershell
.\AIVideoCutty.exe "C:\素材\片段.mp4"
```

| 操作 | 按键 |
| --- | --- |
| 打开文件 | Ctrl+O |
| 播放 / 暂停 | Space |
| 暂停时前后逐帧 | ← / →，也可点逐帧按钮 |
| 播放时临时穿梭 | 按住 ← / →；松开恢复选定的正向速度 |
| 前后跳 5% | Ctrl+← / Ctrl+→ |
| 倒向 / 暂停 / 正向穿梭 | J / K / L，重复 J 或 L 提高速度至 8× |
| 设置 A / B | I / O |
| 清除 A/B、切换循环 | X / P |
| 从头播放 | R |
| 音量、静音 | ↑ / ↓，M |
| 全屏 / 恢复 | F11 |
| 退出全屏；窗口模式下关闭 | Esc |

倍速下拉框提供 0.5×、1×、1.5×、2×、4×、8×。只设 A 或只设 B，另一个端点分别默认为片尾或片头；清除后循环整个文件。倒放和倍速取决于系统解码器和素材，能力不足时显示提示，不静默转码。

截图先暂停，保存为实际解码帧尺寸的 JPG，不包含窗口控件。只创建新文件；已有文件拒绝覆盖，取消不写文件。截图使用同目录临时文件，编码成功后才发布；失败清理本次临时文件。

## 在 Windows 编译

安装 Visual Studio 2022 或更新版本的“使用 C++ 的桌面开发”与 Windows SDK，开发工具只需要在构建机器安装。使用 x64 Native Tools 命令行，在仓库根目录执行：

```powershell
cmake -S Windows -B build/windows -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
cmake --install build/windows --config Release --prefix dist/windows
```

生成 `dist/windows/AIVideoCutty.exe` 与本使用说明。CRT 静态链接，普通用户无需另外安装 VC++ 运行时。未使用 Windows 11 专属接口；应用清单声明 Windows 10/11 支持、DPI 感知和长路径。

GitHub Actions 的 Windows 作业构建 Release、运行播放规则测试并输出便携 ZIP。服务器 runner 的编译/逻辑检查不能代替 Windows 10/11 桌面播放测试。

## 原型边界

- 当前实现文件入口、视频/音频播放、进度拖动、逐帧、倍速/穿梭、A/B 循环、音量/静音、全屏和 JPG 截图。
- 缩略图、波形、图像预览/编辑、裁切/压缩导出、快捷键设置、Explorer 菜单、签名和安装器属于后续阶段。
- 普通预览只使用系统编解码器；损坏或不支持的媒体显示错误。Windows N 版本可能需要微软 Media Feature Pack；HEVC 等格式依赖机器上的实际 codec。此程序不会自动安装 codec 或联网处理素材。
- 原型只接受本机普通文件，UNC 网络路径不在此阶段范围；多选/多个参数只使用第一个。
- 逐帧由异步 FrameStep 完成事件确认，5 秒未完成会停止等待并提示。A/B 循环是播放控制原型，尚未承诺不同编码的帧精确边界。
- 截图上限为 256 MiB 的原始 BGRA 像素，避免异常尺寸申请无界内存。HDR/色彩管理尚未验收；不能将 8-bit JPG 当作 HDR 无损截图。
- 暂停/空窗口不运行持续渲染定时器。播放只保留播放器必要缓冲，没有全片抽帧缓存；软件绘图回退会明确显示。
- 界面显示“首帧就绪”的耗时从文件打开请求计时，不含进程启动；该数值不是完整性能测试报告。
- 未签名原型可能出现 SmartScreen 提示，不自动修改系统安全设置。

## 必须补做的 Windows 实测

Windows 10 和 11 各验证：打开中文/空格/长路径文件，1080p 与 4K、H.264/AAC、MOV、HEVC、VFR、长 GOP、无音轨、音频文件；暂停前后逐帧、松键恢复、循环端点、拖动、截图分辨率、截图冲突和取消、连续换片、全屏/DPI/多屏及退出资源释放。

分别记录 EXE/分发体积、冷/热启动到窗口、打开到实际首帧、总 Private Bytes、CPU/GPU、播放丢帧与退出残留。当前原型没有这些实机数据，不能将“小 EXE”当作“播放一定轻”。

相关接口：[FrameStep](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengineex-framestep)、[播放速度能力](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengineex-isplaybackratesupported)、[原始帧输出](https://learn.microsoft.com/en-us/windows/win32/api/mfmediaengine/nf-mfmediaengine-imfmediaengine-transfervideoframe)。
