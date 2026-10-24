# chost：基于 Carla 的独立音频插件宿主

`chost` 是一个直接链接 libcarla 的独立宿主程序（目前是命令行版本，图形界面后面再加）。

## 已支持

- 插件格式：LV2、VST3、VST2、LADSPA、SF2/SFZ（全部由 Carla 加载）
- 机架模式：插件按加载顺序串联处理
- 加载音频文件当作信号源（Carla 内置的 audiofile 播放器）
- 按序号、名称或符号设置参数，旁路插件，打开插件自带的编辑器窗口
- 保存和加载 `.carxp` 工程（与 Carla 自身的工程格式兼容）
- 音频驱动：JACK、ALSA、PulseAudio、SDL，以及用于无声卡测试的 Dummy 驱动

## 编译（Linux）

```sh
sudo apt install carla carla-dev cmake g++ pkg-config
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 使用

```sh
./build/chost drivers                 # 列出音频驱动
./build/chost scan all                # 扫描 LV2 / VST3 插件
./build/chost run --driver PulseAudio \
    --file song.wav \
    --plugin lv2:urn:zamaudio:ZamComp \
    --plugin vst3:/usr/lib/vst3/ZamDelay.vst3 \
    --set 1:Threshold=-20             # 进入交互命令行，输入 help 查看命令
```

## 许可证

libcarla 使用 GPL-2.0-or-later，因此本程序也必须以兼容 GPL 的方式发布。
