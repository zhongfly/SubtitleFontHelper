# SubtitleFontHelper

能够让你处理影片字幕的字体加载容易一些

## 介绍
本程序可以对用户存放字体文件的目录进行扫描后建立字体信息的索引，在后台监视特定进程的创建并注入Dll劫持特定API的调用，使其在真正调用相关API之前先查询索引并装载相应的字体，从而实现自动加载字体。

目前，**仅支持**使用GDI相关函数来查询/加载字体的传统Win32桌面程序，**不支持**UWP应用，**不支持**使用IDWriteFontCollection/IDWriteFontSet等DirectWrite接口来查询/加载字体的程序。

程序本身负责的是“预加载候选字体”，不是替目标程序做最终选字：它会在被劫持的 GDI 调用真正执行前，按 `family name`、`FullName`、`PostScriptName` 从索引中补充加载可能相关的字体；最终实际采用哪一个字体，仍由目标程序以及底层 GDI 字体匹配流程决定。因此，如果某些程序或库（例如 `libass`）后续还有自己的字体选择逻辑，本程序不会取代那部分行为。

### fork版修改
* 自动热重载配置文件和字体索引文件：修改配置文件和字体索引文件后，不再需要重启软件或者手动reload
* 在设置字体文件夹后，自动生成字体索引，并在新增/修改/删除字体时，自动更新字体索引文件
* 支持相对路径，可以与字体一起便携移动
* 配置文件支持使用TOML，更加方便修改、自定义
* 日志保存到文件，并增加更多信息
* 增加系统通知，字体索引建立、更新时清晰掌握情况
* 也许优化了性能、速度、内存占用，参考速度：在机械硬盘上，为17000+的vcb字体完整包构建全部字体索引需要约90s（冷启动）或者约20s（有缓存）
* 增加更多bug

## 使用
### SubtitleFontAutoLoaderDaemon.exe
主程序。运行后会优先从 exe 所在目录读取 `SubtitleFontHelper.toml`；`SubtitleFontHelper.xml` 仅作为兼容旧版本配置的回退格式。程序没有界面，但是会创建一个托盘图标，以方便控制。
日志将会写入程序目录下的`SubtitleFontHelper.log`，并按大小自动轮转为`SubtitleFontHelper.log.1`到`SubtitleFontHelper.log.5`。达到约10MiB后会滚动到下一个归档文件。

运行前需要安装 [Visual C++ 运行时](https://learn.microsoft.com/zh-cn/cpp/windows/latest-supported-vc-redist?view=msvc-170#latest-supported-redistributable-version)

### enableAutoStart.ps1
在当前用户的开始菜单-启动目录下创建快捷方式，以实现自动启动。

### disableAutoStart.ps1
删除上面创建的快捷方式，以禁用自动启动。

### SubtitleFontHelper.toml
配置文件使用 UTF-8 编码。**新配置请使用** `SubtitleFontHelper.toml`。`SubtitleFontHelper.xml` 仅保留给旧版本配置兼容回退。

推荐的 TOML 示例：
```
wmi_poll_interval = 1000
lru_size = 100
# managed_index_notifications = true
# missing_font_notifications = true
monitor_processes = [
  'mpc-hc64_nvo.exe',
  'mpc-hc_nvo.exe',
]

[[index_files]]
path = 'indexes/FontIndex.xml'
source_folders = [
  'fonts',
]

[[index_files]]
path = 'indexes/FontIndex-2.xml'
source_folders = [
  '/another_fonts',
  '/another_fonts_2',
]
```

 - 优先读取 `SubtitleFontHelper.toml`，只有在它不存在时才会回退到 `SubtitleFontHelper.xml`。
 - `wmi_poll_interval` 指定WMI查询的间隔时间，毫秒数。较低的值导致较高的CPU使用率。较高的值可能会导致注入进程不够及时。
 - `lru_size` 指定服务启动时预加载的条目最大大小。
 - `managed_index_notifications` 统一控制字体索引相关系统通知。默认关闭；只有设置为 `true` 时，才会在索引开始建立、建立完成、更新完成、失败以及长时间处理中的进度变化时发出系统通知。托盘里的“构建中/更新中”状态不受这个开关影响。
 - `missing_font_notifications` 控制缺失字体提示。默认关闭；当字体既不在索引里、系统也没有对应字体时，会弹出一次系统通知。设为 `true` 可开启。
 - 每一节 `[[index_files]]` 都表示一个字体索引文件的配置：其中 `path` 用来设置字体索引文件的保存位置和名称； `source_folders` 表示字体索引文件所覆盖的字体文件来源，在字体索引文件已经存在时，可省略，省略后将不再监视其中的字体文件变化。
 - TOML 里的 `[[index_files]].path` 与 `source_folders[]` 支持相对路径，基准目录是 `SubtitleFontHelper.toml` 所在目录；绝对路径仍然可用。XML 格式的配置不支持使用相对路径。 
 - 字体索引文件中 的 `FontFace/@path` 会在可行时写成相对索引文件目录的路径；程序读取后会统一解析成绝对路径。
 - 字体索引文件 的 `.state.bin` 快照会在可行时写成相对快照文件目录的路径，这个快照用于记录建立字体索引时的字体文件位置，以便更快速地、增量更新字体索引文件。
 - `MonitorProcess` / `monitor_processes` 用于指定要监视的进程路径或者进程名。由于程序使用了`rundll32.exe`作为注入过程中的辅助程序，指定该进程可能会导致灾难性的后果。
 - XML 仅用于兼容旧版本配置；新配置不再提供 XML 示例。
 - XML 中未被程序使用的额外属性或元素会被忽略，便于与外部工具共享同一份配置文件；TOML 中**当前支持类型**的未知键也会被忽略。

### FontLoaderInterceptor32.dll
### FontLoaderInterceptor64.dll
注入进程使用的Dll，请保持与主程序在同一目录下。

### FontDatabaseBuilder.exe
用于手动创建字体索引，在主程序配置里使用`[[index_files]].source_folders`时不需要手动创建。
使用时将要创建索引的文件夹拖放至该程序上即可，请根据程序输出提示操作。
请保证输出文件位置可写，否则可能会导致您不必要地浪费时间。
额外的命令行选项请不带参数执行以查看。
