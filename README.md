# chost：基于 Carla 的独立音频插件宿主

`chost` 是一个直接链接 libcarla 的独立插件宿主，有图形界面版（`chost`）和命令行版（`chost-cli`），支持 Windows 和 Linux。

## 功能

- 插件格式：VST3、VST2、LV2、LADSPA、SF2/SFZ（由 Carla 加载）
- 插件浏览器：扫描系统插件目录，按名称、厂商、格式搜索，双击加入机架
- 机架：插件从上到下串联处理，可启用/旁路、上移/下移、移除
- 参数面板：每个参数一个滑块，输出类参数（电平、增益衰减等）实时刷新
- 打开插件自带的界面窗口
- 加载音频文件（WAV/FLAC/MP3/OGG 等）当信号源，输出电平表
- 保存和打开 `.carxp` 工程（与 Carla 的工程格式兼容）
- 音频驱动：Windows 上为 WASAPI/DirectSound/ASIO，Linux 上为 PulseAudio/JACK/ALSA，另有用于无声卡测试的 Dummy

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

图形界面版启动时也可以带参数：`chost --load <插件> --file <音频文件>`。

## 已知问题

- Carla 2.5.x 在加载短于 30 秒、且采样率高于声卡采样率的音频文件时会越界读取并崩溃（Carla 主分支已修复，但还没发布）。chost 会先把 WAV/FLAC/MP3 转换到声卡采样率再交给 Carla，其他格式没有这个保护。
- 插件浏览器里的 VST2/VST3 只显示文件名，厂商等信息要加载后才能看到。

## 目录结构

```
src/core/             宿主核心：封装 libcarla、插件扫描、音频文件转换
src/gui/              Qt 图形界面
src/cli/              命令行版
scripts/              Windows 下载 Carla 和打包脚本
third_party/dr_libs/  WAV/FLAC/MP3 解码（公有领域 / MIT-0）
```

## 许可证

libcarla 使用 GPL-2.0-or-later，因此本程序也必须以兼容 GPL 的方式发布。
