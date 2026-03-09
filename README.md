# SubtitleFontHelper

能够让你处理影片字幕的字体加载容易一些

## 介绍
本程序可以对用户存放字体文件的目录进行扫描后建立字体信息的索引，在后台监视特定进程的创建并注入Dll劫持特定API的调用，使其在真正调用相关API之前先查询索引并装载相应的字体，从而实现自动加载字体。

目前，**仅支持**使用GDI相关函数来查询/加载字体的传统Win32桌面程序，**不支持**UWP应用，**不支持**使用IDWriteFontCollection/IDWriteFontSet等DirectWrite接口来查询/加载字体的程序。

## 使用
### FontDatabaseBuilder.exe
用于创建字体索引。使用时将要创建索引的文件夹拖放至该程序上即可，请根据程序输出提示操作。
请保证输出文件位置可写，否则可能会导致您不必要地浪费时间。
额外的命令行选项请不带参数执行以查看。

### SubtitleFontAutoLoaderDaemon.exe
主程序。运行后会优先从 exe 所在目录读取 `SubtitleFontHelper.toml`；`SubtitleFontHelper.xml` 仅作为兼容旧版本配置的回退格式。程序没有界面，但是会创建一个托盘图标，以方便控制。
日志将会写入程序目录下的`SubtitleFontHelper.log`，并按大小自动轮转为`SubtitleFontHelper.log.1`到`SubtitleFontHelper.log.5`。达到约10MiB后会滚动到下一个归档文件，因此无需注册ETW或事件查看器清单。

### enableAutoStart.ps1
在当前用户的开始菜单-启动目录下创建快捷方式，以实现自动启动。

### disableAutoStart.ps1
删除上面创建的快捷方式，以禁用自动启动。

### SubtitleFontHelper.toml
配置文件使用 UTF-8 编码。**新配置请使用** `SubtitleFontHelper.toml`。`SubtitleFontHelper.xml` 仅保留给旧版本配置兼容回退。当前 TOML 读取实现面向本项目配置场景，支持**简单子集**：正整数、字符串、字符串数组，以及 `[[index_files]]` 表。

推荐的 TOML 示例：
```
wmi_poll_interval = 1000
lru_size = 100
monitor_processes = [
  'mpc-hc64_nvo.exe',
  'mpc-hc_nvo.exe',
]

[[index_files]]
path = 'E:\超级字体整合包 XZ\FontIndex.xml'
```

 - TOML 中推荐使用 `wmi_poll_interval`、`lru_size`、`monitor_processes` 和 `[[index_files]]` / `path`。
 - `SubtitleFontHelper.toml` 只要存在就会被优先读取；只有在该文件不存在时才会回退到 `SubtitleFontHelper.xml`。
 - `wmiPollInterval` / `wmi_poll_interval` 指定WMI查询的间隔时间，毫秒数。较低的值导致较高的CPU使用率。较高的值可能会导致注入进程不够及时。
 - `lruSize` / `lru_size` 指定服务启动时预加载的条目最大大小。
 - XML 的 `IndexFile` 元素和 TOML 的 `[[index_files]].path` 都用于指定索引文件位置。
 - `MonitorProcess` / `monitor_processes` 用于指定要监视的进程路径或者进程名。由于程序使用了`rundll32.exe`作为注入过程中的辅助程序，指定该进程可能会导致灾难性的后果。
 - XML 仅用于兼容旧版本配置；新配置不再提供 XML 示例。
 - XML 中未被程序使用的额外属性或元素会被忽略，便于与外部工具共享同一份配置文件；TOML 中**当前支持类型**的未知键也会被忽略。

### FontLoaderInterceptor32.dll
### FontLoaderInterceptor64.dll
注入进程使用的Dll，请保持与主程序在同一目录下。
