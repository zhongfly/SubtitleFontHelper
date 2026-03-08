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
主程序。运行后会从exe所在目录下的SubtitleFontHelper.xml读取配置文件。程序没有界面，但是会创建一个托盘图标，以方便控制。
日志将会写入程序目录下的`SubtitleFontHelper.log`，并按大小自动轮转为`SubtitleFontHelper.log.1`到`SubtitleFontHelper.log.5`。达到约10MiB后会滚动到下一个归档文件，因此无需注册ETW或事件查看器清单。

### enableAutoStart.ps1
在当前用户的开始菜单-启动目录下创建快捷方式，以实现自动启动。

### disableAutoStart.ps1
删除上面创建的快捷方式，以禁用自动启动。

### SubtitleFontHelper.xml
配置文件，使用UTF-8编码。样例如下所示：
```
<?xml version="1.0" encoding="UTF-8"?>
<ConfigFile wmiPollInterval="1000" lruSize="100">
<IndexFile>E:\超级字体整合包 XZ\FontIndex.xml</IndexFile>
<MonitorProcess>mpc-hc64_nvo.exe</MonitorProcess>
<MonitorProcess>mpc-hc_nvo.exe</MonitorProcess>
</ConfigFile>
```
 - `wmiPollInterval` 指定WMI查询的间隔时间，毫秒数。较低的值导致较高的CPU使用率。较高的值可能会导致注入进程不够及时。
 - `lruSize` 指定服务启动时预加载的条目最大大小。
 - `IndexFile`元素 每个元素指定了索引文件的位置，在这里列出程序所使用的索引。元素开始和结束之间的**所有**字符（包括换行等字符）将会被当作文件路径使用，若提示找不到文件请检查相关内容。
 - `MonitorProcess`元素 每个元素指定了要监视的进程的路径或者进程名。由于程序使用了`rundll32.exe`作为注入过程中的辅助程序，指定该进程可能会导致灾难性的后果。
 - 未被程序使用的额外属性或元素会被忽略，便于与外部工具共享同一份配置文件。

### FontLoaderInterceptor32.dll
### FontLoaderInterceptor64.dll
注入进程使用的Dll，请保持与主程序在同一目录下。
