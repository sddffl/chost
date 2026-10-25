# chost：基于 Carla 的独立音频插件宿主

`chost` 是一个直接链接 libcarla 的独立插件宿主，有图形界面版（`chost`）、命令行版（`chost-cli`）和离线混音工具（`chost-render`），支持 Windows 和 Linux。

## 界面

![chost 界面](docs/screenshot.png)

仿照 Cubase 的布局：顶部是走带控制条（回到开头、停止、播放、时间和小节显示），左边是检查器（选中轨道的名称、音频信息、插入效果），中间是多条音轨和时间线，右边是插件浏览器，底部是调音台（每条轨道一个通道：插入效果、静音 M、独奏 S、推子、电平表，最右边是 Stereo Out 总输出推子）。

- 把音频文件拖进窗口（可以一次拖多个），每个文件变成一条音轨；拖到某条轨道上则替换它的音频
- 点时间线定位，空格键播放/暂停，Ctrl+滚轮缩放，滚轮上下滚动轨道，Shift+滚轮左右滚动
- 也可以拖 MIDI 文件（.mid），会变成一条乐器轨，轨道上直接显示音符
- 右键轨道：重命名、替换音频、AI 自动分轨、AI 转 MIDI、换乐器、添加插入效果、删除
- 走带栏显示速度（BPM）和调性：导入第一首歌时 AI 会自动识别，不对的话直接改数字，或点 **识别** 重新听选中的音轨
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

## AI 转 MIDI

点工具栏的 **AI 转 MIDI**（或右键轨道 → AI 转 MIDI），勾选要转换的音轨。每条音轨转成一条 MIDI 乐器轨，放在原音轨下面、同样的颜色，原音轨自动静音，听到的就是 MIDI 播放的声音。

- 先用“AI 分轨”分成单个乐器再转最准：主唱、和声、贝斯按单音旋律转，钢琴、吉他按和弦转，底鼓、军鼓、踩镲、通鼓、镲片转成对应的 General MIDI 鼓音符
- 自己导入的音轨，按名字猜是什么乐器（比如名字里有 bass、贝斯、kick、钢琴）
- 音高识别用 Spotify 开源的 [Basic Pitch](https://github.com/spotify/basic-pitch) 模型（已经打包在 `tools/models` 里，不用下载），在你电脑上本地运行；需要先在分轨窗口里装好 AI 组件
- MIDI 文件保存在原音频旁边，同名 `.mid`，可以拖到别的软件（Cubase、FL Studio、打谱软件）里用

### 乐器轨

MIDI 乐器轨默认用自带的 General MIDI 音色（TimGM6mb）播放，会按 MIDI 里的乐器自动选音色（贝斯用贝斯、钢琴用钢琴，鼓用鼓组）。在检查器里可以：

- 在下拉框里换音色
- 点 **换乐器…** 换成你自己的乐器插件（Kontakt、Serum、任何 VST 乐器），或者选一个 SF2/SFZ 音色库文件
- 点 **打开界面** 打开乐器自己的窗口
- 点 **记住这个音色**：把这个乐器连同你在里面选好的音色（比如 Kontakt 里加载的贝斯音色库）记下来，以后转出来的同一种乐器自动用它

### 自动挂真实音源

**插件 → 乐器设置** 里可以给每种乐器（主唱、和声、贝斯、吉他、钢琴、鼓……）选一个音源。点 **自动匹配我的乐器**，chost 会从你装的插件里找专门的乐器（Superior Drummer、EZdrummer、Addictive Drums、Trilian、Keyscape、Pianoteq、Omnisphere 等）自动配上。点 **应用到现有 MIDI 轨**（或 AI → MIDI 轨全部换成真实音源）把工程里的 MIDI 轨一次全换掉。

鼓的各个部件（底鼓、军鼓、踩镲……）转完会合成一条“鼓 MIDI”轨，只开一个鼓音源。

### 编辑音符（钢琴卷帘）

双击轨道上的 MIDI（或检查器里的 **编辑音符**）打开钢琴卷帘，用来修正转错的音：

- 单击空白处加音符，按住往右拖决定长短；拖动音符移动，拖右边缘改长短
- 右键、双击或 Delete 删除；Ctrl+拖动框选；方向键移动（Shift+上下移一个八度）
- 网格可选 1/4 到 1/32 和三连音，**量化** 把音符对齐到网格；Ctrl+Z 撤销
- 点音符或左边的琴键会用这条轨的乐器发声；改动马上生效，播放时就能听到
- 改过的 MIDI 存成 `<原名>_edit.mid`，不会覆盖原文件；鼓轨左边显示鼓件名称
- **音阶**：自动用歌曲的调（还没识别调时，按音符猜），也可以自己选主音和音阶（大调、小调、和声/旋律小调、教会调式、五声宫调式/羽调式、布鲁斯）。调内的音那几行亮起来，主音标蓝，调外的音符画橙色框，顶上显示有几个音在调外
- **吸附**：勾上后，加音符、拖动音符、上下方向键都只走调内的音（拖和弦会按音阶平移，和弦还在调内）；Shift+上下仍然是一个八度
- **修正到调内**：把选中的（没选就是全部）调外音挪到最近的调内音，常用来修转 MIDI 弹错的音；不满意 Ctrl+Z

## AI 自动编曲

**AI → 自动编曲…**（Ctrl+Shift+A，或走带栏的 **AI 编曲** 按钮）按风格自动写出鼓、贝斯、键盘、吉他和弦乐，每样一条 MIDI 乐器轨，用你在“乐器设置”里给每种乐器选的音源（比如 Kontakt、Superior Drummer）播放。

- 和弦可以**跟着一条轨道**：一首歌的录音会自动听出每半小节的和弦和段落（安静的地方编得轻，热闹的地方编得满），一条 MIDI（比如钢琴、旋律）也行
- 也可以**自己写和弦**：`C G | Am F | Dm7 G7 | C`，竖线分小节，一小节可以写两个和弦；支持 m、7、maj7、m7、sus4、dim、add9、C/E 这样的写法
- 风格：流行、摇滚、抒情、民谣、电子、中国风；自己写和弦时自动排成 前奏-主歌-副歌-主歌-副歌-尾奏，换段落处有加花和镲
- 生成的每条轨道都能双击打开钢琴卷帘修改；再生成一次会替换上一次的编曲轨道

## AI 修音

**AI → AI 修音…**（Ctrl+Shift+T，走带栏的 **AI 修音** 按钮，或检查器里的 **AI 修音…**）把人声（或吉他、萨克斯这类一次只发一个音的乐器）里跑调的音逐个修准：

- 先听出每 10 毫秒唱的音高，再分成一个个音；每个音**整体**移到调里最近的音上，所以颤音、滑音这些表情都保留，不会变成机器人声
- 调性可以跟工程一样、让 AI 从这段人声里自己听，或者手动选；唱得很准的调外音（经过音、布鲁斯音）默认不动，只修模棱两可的
- 修法：自然（推荐）、轻微（听不出修过）、精准（音内的漂移也拉直）、电音效果（Auto-Tune 那种音高直接跳到音上）；力度可以调
- 用的是按声带振动周期切片再重新拼接的方法（PSOLA），音色不变，没有音高的地方（气声、齿音）一个采样都不改
- 修好的声音直接替换这条轨道的音频，插入效果和音量都不变；窗口里的 **A/B** 按钮随时切回原声对比，下面的图显示每个音修正前（灰线）后（橙线）的音高
- 修好的文件叫 `原文件名_修音.wav`，放在原文件旁边，原文件不会被改动

效果最好的是单独的人声：如果是整首歌，先用 AI 自动分轨把人声分出来，再对人声轨修音。

## 导出混音

**文件 → 导出混音（WAV）…**（Ctrl+E）把整个工程按现在听到的样子（音量、静音、独奏、所有插件）混成一个 WAV 文件，可选 24 位、16 位或 32 位浮点。导出不用等歌曲播完，比实时播放快得多；勾选“同时把每条轨道单独导出”还会在旁边的 `_分轨` 文件夹里给每条轨道各导出一个文件。

导出由单独的 `chost-render` 程序完成，就算某个插件在导出时崩溃，chost 本身也不会跟着退出。

## AI 母带

**AI → AI 母带…**（或走带栏的 **AI 母带** 按钮）把整首混音做成可以发布的母带，像母带工程师一样分四步：

1. 音色平衡：用线性相位 EQ 把整体频谱往平衡的方向轻轻推（最多几 dB），选了参考歌曲就往参考歌曲的音色靠
2. 120 Hz 以下的低频收成单声道，低音在手机和大音箱上都稳
3. 轻微的整体压缩（glue），让声音更“粘”在一起
4. 推到目标响度（按 ITU-R BS.1770 / EBU R128 测量，各音乐平台都用这个标准）并用 4 倍过采样的真峰值限制器把峰值压在 -1 dB 以下

风格可选 自然 / 温暖 / 明亮 / 有力；响度可选 流媒体 -14 LUFS、短视频 -10 LUFS、最响 -8 LUFS，或和参考歌曲一样响。做完会显示前后的响度、峰值和 EQ 曲线；点 **放到新轨道上试听对比**，再用 **A/B** 按钮在母带和原混音之间切换。

识别速度调性、AI 母带和 AI 修音用的是 AI 分轨装好的那套 AI 组件，第一次要先在“AI 分轨”窗口里点 **安装 AI 组件**。

## 你自己的插件

插件浏览器下面的 **插件文件夹…** 可以添加你装插件的文件夹（比如 `D:\VST`），VST2、VST3、LV2 都会扫描进来。双击插件就加到选中轨道的插入效果里，单击插槽打开插件自己的界面。

chost 会在后台逐个识别 VST 插件（名称、厂商、是乐器还是效果），每个插件在单独的进程里检查，坏掉的插件不会让 chost 崩溃。第一次识别插件多的话要几分钟，结果会记住，以后启动就很快。Waves 这种一个文件里装很多插件的（WaveShell，VST2 版）会拆成一个个插件；老的 32 位 VST2 插件也能用。

## 和弦板（和弦提示）

**AI > 和弦板**（Ctrl+Shift+K）把歌曲调性里的和弦排成两排：上排是本调的 7 个和弦，下排是流行歌常借用的色彩和弦（Vsus4、副属和弦、iv、bVI、bVII 等），每个都标了级数（I、vi、V7…）。

- 点一下（或按键盘 1–7、Q–U）就用选中的乐器轨弹出这个和弦，转位会自动选得最顺。还没有乐器轨的话，点“新建钢琴轨”
- 每弹一个和弦，最适合接在后面的几个会亮起来、带 ★，下面一行也会写“C 后面可以接：F（IV）、G（V）…”
- 点过的和弦依次排成“和弦进行”，可以拖动换顺序，右键改长短（半小节到 4 小节）或删除，点“试听进行”按歌曲速度听一遍
- **识别和弦**：听出一首歌每个小节的和弦，填进和弦进行。之后播放这首歌时，和弦板会跟着标出现在弹到哪个和弦，并提示接下来可以用什么
- **写成 MIDI 轨**：可以选柱式和弦、分解和弦或流行钢琴节奏，写成一条乐器轨，之后可以在钢琴卷帘里改；**交给 AI 自动编曲** 会让鼓、贝斯、吉他、弦乐都跟着这串和弦

## 让 Claude 操控 chost（MCP）

chost 自带一个 MCP 服务器 `chost-mcp.exe`。连上以后，在 Claude 桌面版里直接说“把鼓调小 3 dB”“给人声加个压缩，阈值 -18”“把这首歌分轨，再转成 MIDI”“导出混音”，Claude 就会在 chost 里动手做，你在窗口里能看到每一步，不满意按 Ctrl+Z 撤销。

- 连接：**AI > 连接 Claude…**，点“一键连接 Claude 桌面版”，然后完全退出 Claude 桌面版再打开。用 Claude Code 的话，对话框里有一行命令可以复制
- chost 没开着时，Claude 第一次用工具会自动把 chost 打开
- Claude 能做的事：给和弦建议、写和弦轨、把 MIDI 轨的调外音修正到调内；看工程（轨道、推子、插件、速度调性）；加删轨道、改名、推子、静音独奏、换音频；查找本机插件、加删和旁路插入效果、读写插件参数、打开插件窗口；换乐器；播放、停止、定位、看电平；撤销、重做、看操作历史；AI 识别速度调性、分轨、转 MIDI、修音、编曲、母带、导出混音（这些在后台运行，Claude 会等它们做完）；保存、打开、新建工程（有没保存的改动时，Claude 不会擅自丢掉）
- 只有本机、你自己的账户能连 chost，别的电脑和别的用户连不上

## 撤销、自动备份

- **编辑 > 撤销**（Ctrl+Z）和 **重做**（Ctrl+Y）：加删轨道、换音频、推子、静音/独奏、插入效果、换乐器、速度和调性，以及 AI 分轨、编曲、母带、修音，都能一步步撤回（最近 100 步）。删掉的轨道和插件撤销回来时设置都还在
- **编辑 > 操作历史…** 列出做过的每一步，点哪一步就回到哪一步
- 有没保存的改动时，标题栏的工程名后面有个 `*`，关闭时会问要不要保存
- **自动备份**：有没保存的改动时，每 2 分钟在 chost 自己的文件夹里悄悄存一份（不会覆盖你的工程）。电脑死机、断电或 chost 崩溃之后，下次打开会问“要恢复吗？”
- **以前的版本**：每次保存工程，被覆盖掉的那一版会留在工程旁边的 `工程名_备份` 文件夹里（最近 10 版）。**文件 > 以前的版本…** 可以打开其中任意一版，再保存就换回那一版

## 功能

- 插件格式：VST3、VST2、LV2、LADSPA、SF2/SFZ（由 Carla 加载）
- 插件浏览器：扫描系统插件目录，按名称、厂商、格式搜索，双击加入机架
- 多轨：音轨是“音频 → 插入效果 → 推子”，乐器轨是“MIDI → 乐器 → 插入效果 → 推子”，所有轨道在 Stereo Out 混合（用 Carla 的 patchbay 模式）
- 插入效果：从上到下串联处理，可启用/旁路、上移/下移、移除
- 参数面板：每个参数一个滑块，输出类参数（电平、增益衰减等）实时刷新
- 打开插件自带的界面窗口
- 加载音频文件（WAV/FLAC/MP3/OGG 等）当信号源，输出电平表
- 保存和打开 `.chost` 工程（旁边会生成同名的 `.carxp`，里面是插件和它们的设置；速度和调性也存在工程里）
- 导出混音和分轨文件（离线渲染，比实时快）
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
chost-cli mix --driver PulseAudio --midi bass.mid@sf2:resources/sounds/TimGM6mb.sf2   # MIDI 乐器轨
```

离线混音（把 `.chost` 工程混成 WAV，`--solo` 只混某几条轨道，`--mute` 去掉某几条）：

```sh
chost-render --session song.chost --output mix.wav --bits 24
```

音阶（`--guess` 按音符猜调，`--fit` 把调外的音移进音阶）：

```sh
chost-cli scale --scale "A harmonic minor"
chost-cli scale --guess melody.mid --fit melody.mid --output fixed.mid
```

图形界面版启动时也可以带参数：`chost --file a.wav --file b.mid --load <插件> --open <工程.chost>`。

## 已知问题

- Carla 2.5.x 在加载短于 30 秒、且采样率高于声卡采样率的音频文件时会越界读取并崩溃（Carla 主分支已修复，但还没发布）。chost 会先把 WAV/FLAC/MP3 转换到声卡采样率再交给 Carla，其他格式没有这个保护。
- Carla 2.5 加载 VST3 时只能用一个文件里的第一个插件，所以 Waves 请用 VST2 版的 WaveShell。
- Kontakt 这类采样器加载后是空的，要先在它的界面里选音色，再点“记住这个音色”。

## 目录结构

```
src/core/             宿主核心：封装 libcarla、多轨工程（Session）、插件扫描、音频文件转换
src/gui/              Qt 图形界面
src/cli/              命令行版
src/render/           chost-render：离线混音
src/control/          远程控制的工具定义（chost 和 chost-mcp 共用）
src/mcp/              chost-mcp：给 Claude 等 AI 助手用的 MCP 服务器
tools/                AI 辅助脚本（chost_separate.py：自动分轨，chost_transcribe.py：转 MIDI，
                      chost_analyze.py：速度和调性，chost_master.py：母带，chost_arrange.py：编曲，
                      chost_tune.py：修音）
tools/models/         Basic Pitch 音高识别模型（Apache-2.0）
resources/sounds/     自带的 General MIDI 音色 TimGM6mb（GPL-2）
tests/                测试用的音频生成，以及转 MIDI、编曲、修音的准确度检查脚本
scripts/              Windows 下载 Carla 和打包脚本
third_party/dr_libs/  WAV/FLAC/MP3 解码（公有领域 / MIT-0）
```

## 许可证

libcarla 使用 GPL-2.0-or-later，因此本程序也必须以兼容 GPL 的方式发布。
