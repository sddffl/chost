# chost：基于 Carla 的独立音频插件宿主

`chost` 是一个直接链接 libcarla 的独立插件宿主，有图形界面版（`chost`）和命令行版（`chost-cli`），支持 Windows 和 Linux。

## 界面

![chost 界面](docs/screenshot.png)

仿照 Cubase 的布局：顶部是走带控制条（回到开头、停止、播放、时间和小节显示），左边是检查器（选中轨道的名称、音频信息、插入效果），中间是多条音轨和时间线，右边是插件浏览器，底部是调音台（每条轨道一个通道：插入效果、静音 M、独奏 S、推子、电平表，最右边是 Stereo Out 总输出推子）。

- 把音频文件拖进窗口（可以一次拖多个），每个文件变成一条音轨；拖到某条轨道上则替换它的音频
- 点时间线定位，空格键播放/暂停，Ctrl+滚轮缩放，滚轮上下滚动轨道，Shift+滚轮左右滚动
- 右键轨道：重命名、替换音频、AI 自动分轨、添加插入效果、删除
- 截图里是“自动分轨”之后的样子：原曲和被细分的“人声”“鼓”自动静音，主唱/和声、底鼓/军鼓等放在各自分组下面

## AI 自动分轨

选中一条有音频的轨道，点工具栏的 **AI 分轨**（或右键轨道 → AI 自动分轨）。可以选三种细度：

| 选项 | 分出的轨道 | 模型 |
|---|---|---|
| 4 轨 | 人声、鼓、贝斯、其他 | Demucs htdemucs_ft |
| 6 轨 | 再分出吉他、钢琴 | Demucs htdemucs_6s |
| 细分（默认） | 6 轨，鼓再拆成底鼓、军鼓、通鼓、踩镲、叮叮镲、吊镲，人声再拆成主唱、和声 | 再加 MDX23C DrumSep 和 Mel-Roformer Karaoke |

分离用的是开源的 [audio-separator](https://github.com/nomadkaraoke/python-audio-separator)，在你电脑上本地运行，不上传音频。第一次使用时：

1. 先装 Python 3.10 或更新的版本（python.org，安装时勾选 “Add python.exe to PATH”）
2. 在分轨窗口里点 **安装 AI 组件**：chost 会在自己的文件夹（`%LOCALAPPDATA%\chost\chost\ai-env`）里建一个独立的 Python 环境，有 NVIDIA 显卡时装 GPU 版
3. 第一次分轨时自动下载模型（约 1 GB，存在 `%LOCALAPPDATA%\chost\chost\models`），网络不好可以开代理

分好的音轨保存在原音频旁边的 `<文件名>_stems` 文件夹里。

## 你自己的插件

插件浏览器下面的 **插件文件夹…** 可以添加你装插件的文件夹（比如 `D:\VST`），VST2、VST3、LV2 都会扫描进来。双击插件就加到选中轨道的插入效果里，单击插槽打开插件自己的界面。

## 功能

- 插件格式：VST3、VST2、LV2、LADSPA、SF2/SFZ（由 Carla 加载）
- 插件浏览器：扫描系统插件目录，按名称、厂商、格式搜索，双击加入机架
- 多轨：每条轨道是“音频 → 插入效果 → 推子”，所有轨道在 Stereo Out 混合（用 Carla 的 patchbay 模式）
- 插入效果：从上到下串联处理，可启用/旁路、上移/下移、移除
- 参数面板：每个参数一个滑块，输出类参数（电平、增益衰减等）实时刷新
- 打开插件自带的界面窗口
- 加载音频文件（WAV/FLAC/MP3/OGG 等）当信号源，输出电平表
- 保存和打开 `.chost` 工程（旁边会生成同名的 `.carxp`，里面是插件和它们的设置）
- 音频驱动：Windows 上为 DirectSound 和 ASIO（Carla 2.5 的 Windows 版没有 WASAPI），Linux 上为 PulseAudio/JACK/ALSA，另有用于无声卡测试的 Dummy（Dummy 不支持多轨混音，没有声音）

## Windows 下载

每次推送到 main，GitHub Actions 都会自动编译 Windows 版：打开仓库的 **Actions** 页，点最新一次成功的 `build`，在页面底部 **Artifacts** 里下载 `chost-windows-x64`，解压后双击 `chost.exe`。

打 `v*` 标签（例如 `v0.2.0`）时，还会在 **Releases** 页生成 `chost-windows-x64.zip`。

插件默认从这些目录扫描：
- VST3：`C:\Program Files\Common Files\VST3`
- VST2：`C:\Program Files\VSTPlugins`、`C:\Program Files\Steinberg\VSTPlugins`、`C:\Program Files\Common Files\VST2`
- LV2：`%APPDATA%\LV2`、`C:\Program Files\Common Files\LV2`

也可以用环境变量 `VST3_PATH`、`VST2_PATH`、`LV2_PATH` 覆盖（多个目录用 `;` 分隔）。

## 在 Linux 上编译

```sh
sudo apt install carla carla-dev qt6-base-dev cmake g++ pkg-config
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/chost
```

## 在 Windows 上编译

在 MSYS2 的 MINGW64 终端里：

```sh
pacman -S --needed git unzip mingw-w64-x86_64-{gcc,cmake,ninja,qt6-base}
scripts/fetch-carla-windows.sh "$PWD/carla-root"      # 下载 Carla 2.5.9 的库和头文件
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCARLA_ROOT="$PWD/carla-root"
cmake --build build
scripts/package-windows.sh build carla-root dist/chost  # 打包成可直接运行的文件夹
```

## 命令行版

```sh
chost-cli drivers                 # 列出音频驱动
chost-cli scan                    # 扫描插件
chost-cli run --file song.wav \
    --plugin lv2:urn:zamaudio:ZamComp \
    --plugin vst3:/usr/lib/vst3/ZamDelay.vst3 \
    --set 1:Threshold=-20         # 进入交互命令行，输入 help 查看命令
```

多轨混音（每个 `--track` 一条轨道，打印总输出的峰值，用于测试）：

```sh
chost-cli mix --driver PulseAudio --track drums.wav --track bass.wav \
    --insert 1:lv2:urn:zamaudio:ZamComp --mute 0 --save song.chost
chost-cli mix --open song.chost
```

图形界面版启动时也可以带参数：`chost --file a.wav --file b.wav --load <插件> --session <工程.chost>`。

## 已知问题

- Carla 2.5.x 在加载短于 30 秒、且采样率高于声卡采样率的音频文件时会越界读取并崩溃（Carla 主分支已修复，但还没发布）。chost 会先把 WAV/FLAC/MP3 转换到声卡采样率再交给 Carla，其他格式没有这个保护。
- 插件浏览器里的 VST2/VST3 只显示文件名，厂商等信息要加载后才能看到。

## 目录结构

```
src/core/             宿主核心：封装 libcarla、多轨工程（Session）、插件扫描、音频文件转换
src/gui/              Qt 图形界面
src/cli/              命令行版
tools/                AI 辅助脚本（chost_separate.py：自动分轨）
tests/                测试用的音频生成脚本
scripts/              Windows 下载 Carla 和打包脚本
third_party/dr_libs/  WAV/FLAC/MP3 解码（公有领域 / MIT-0）
```

## 许可证

libcarla 使用 GPL-2.0-or-later，因此本程序也必须以兼容 GPL 的方式发布。
