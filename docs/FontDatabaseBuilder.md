# FontDatabaseBuilder 技术文档

## 1. 目标与产物

`FontDatabaseBuilder` 的职责是扫描一个或多个字体目录，解析字体文件（`.ttf/.otf/.ttc/.otc`）的关键元数据与名称信息，生成供守护进程加载的字体索引 XML（默认 `FontIndex.xml`）。

输出数据模型来自 `SharedIncludes/PersistantData.h` 中的 `sfh::FontDatabase`，写出实现位于 `PersistantDataLib/PersistantData.cpp`（COM + MSXML）。

## 2. 代码入口与模块职责

- `FontDatabaseBuilder/FontDatabaseBuilder.cpp`：程序入口；参数解析、目录扫描、（可选）去重、并行解析、写出 XML。
- `FontDatabaseBuilder/Win32Helper.h`：Win32 工具封装（绝对路径、目录判断、文件映射、递归扫描）。
- `FontIndexCore/FontDatabaseBuild.cpp`：基于 FreeType 的字体解析与并行构建（多 face、OS/2、名称表等）。
- `FontIndexCore/FontDeduplicate.cpp`：按内容去重（先按文件大小分组，再按 XXH3 128-bit + 字节比较确认）。
- `FontDatabaseBuilder/ConsoleHelper.h`：控制台输出（颜色、擦行、进度条、读取输入）。
- `SharedIncludes/PersistantData.h`：`sfh::FontDatabase`/`FontFaceElement` 数据结构定义。
- `PersistantDataLib/PersistantData.cpp`：`sfh::FontDatabase::WriteToFile()` 的 XML 写出与格式化。

## 3. 高层处理流程

```mermaid
flowchart TD
  A[解析命令行参数] --> B[递归扫描目录\n收集文件路径+大小]
  B --> C{启用 -dedup?}
  C -- 否 --> D[并行解析字体\n(FontIndexCore::BuildFontDatabase)]
  C -- 是 --> E[按文件大小分组]
  E --> F[并行计算内容哈希\n(XXH3 + 8MiB 缓冲)]
  F --> G[按哈希分组并做字节级确认]
  G --> D
  D --> H[写出 FontDatabase XML\n(FontDatabase::WriteToFile)]
```

取消机制：`SetConsoleCtrlHandler()` 注册 `ControlHandler`，收到 Ctrl+C 等信号后将 `g_cancelToken=true`。扫描与计算循环会周期性检查 token，并通过 `ThrowIfCancelled()` 统一中断。

## 4. 参数解析与控制台编码

入口函数为 `wmain()`。启动时会把控制台输入/输出 code page 切到 UTF-8，并为 `wcout/wcerr` 设置 `.UTF8` locale，避免中文路径/字体名输出乱码。由于宽字符输入流在 Windows 控制台环境中存在兼容性问题，交互输入使用 `ConsoleReadLine()`（`ReadConsoleW`）读取。

支持的参数：

- `-output <path>`：输出 XML 路径。
- `-dedup`：启用去重（见第 6 节）。
- `-worker <n>`：设置工作线程数（默认：CPU 核心数的一半）。
- 其余参数：作为输入目录，转换为绝对路径后加入扫描列表。

当前实现使用 `ProgramOptions` 保存参数，`deduplicate` 与 `deleteDuplicates` 均已显式初始化为 `false`。

## 5. 目录扫描（FontIndexCore::EnumerateFontFiles）

扫描逻辑位于 `FontIndexCore/FontIndexCore.cpp` 的 `EnumerateFontFiles()`：

- 使用 `std::filesystem::recursive_directory_iterator` 递归枚举目录。
- 对文件条目应用扩展名过滤（大小写不敏感）：`.ttf/.otf/.ttc/.otc`。
- 输出为 `vector<FontSourceFile>`，其中包含：
  - `m_path`：完整路径
  - `m_fileSize`：文件大小（供 `-dedup` 路径复用）

扫描过程中会调用 `ThrowIfCancelled()`，确保 Ctrl+C 能及时停止递归。

## 6. 文件去重（-dedup，FontIndexCore::DeduplicateFiles）

去重目标是避免重复文件（相同内容）被多次解析。实现分两步：

1) **按文件大小分桶**
   - 文件大小唯一的组：直接保留该文件路径，无需哈希。
   - 同大小组：进入待哈希队列。

2) **并行计算 XXH3 128-bit 内容哈希，并做字节级确认**
   - 哈希实现：`xxhash` 的 `XXH3_128bits` 增量接口。
   - I/O：`CreateFileW` + `FILE_FLAG_SEQUENTIAL_SCAN`；8MiB 缓冲区通过 `VirtualAlloc` 预分配。
   - 哈希只用于快速聚类；同一哈希组内仍会通过 `AreFilesByteEqual()` 做字节级比较，避免哈希碰撞导致误判。

并发模型：

- 全局线程数 `g_WorkerCount`；工作线程并行填充缺失的内容哈希。
- `progress`（原子计数）用于 UI 进度条：大小唯一的文件会先增加进度，哈希完成后每个文件再自增一次。

失败与边界：

- 哈希/读文件异常会通过错误回调上报，当前文件会退化为单独分组或被跳过。
- 若取消 token 置位：去重流程会中断，上层随后 `ThrowIfCancelled()` 终止后续解析与写出。

## 7. 字体解析（FontIndexCore::BuildFontDatabase / FreeType）

解析核心位于 `FontIndexCore/FontDatabaseBuild.cpp`：

- **文件读取方式**：使用 `FileMapping`（`CreateFileMappingW` + `MapViewOfFile`）把字体文件映射到内存，避免多次磁盘读取。
- **多 face 处理**（TTC/OTC）：
  1) 先以 `face_index=-1` 打开，读取 `face->num_faces` 获取 face 数；
  2) 逐个 `faceIndex` 重新打开并解析，生成多个 `FontFaceElement`（同一路径，不同 `m_index`）。
- **字重/斜体/轮廓类型**：
  - `weight`：优先取 OS/2 表（`FT_Get_Sfnt_Table(..., FT_SFNT_OS2)`）的 `usWeightClass`；否则用 `style_flags` 推断（Bold→700，否则→300）。
  - `oblique`：由 `FT_STYLE_FLAG_ITALIC` 推断（1/0）。
  - `psOutline`：调用 `FT_Get_PS_Font_Info()` 判断是否为 PostScript outline 字体（返回非 `FT_Err_Invalid_Argument` 视为支持）。
- **名称提取与规范化**：
  - 遍历 `name` 表：只保留 `TT_PLATFORM_MICROSOFT` 且 `name_id` 属于：
    - `TT_NAME_ID_FONT_FAMILY` → `Win32FamilyName`
    - `TT_NAME_ID_FULL_NAME` → `FullName`
    - `TT_NAME_ID_PS_NAME` → `PostScriptName`
  - 编码转换策略：
    - Big5/GB2312/Wansung：按编码 ID 选择 code page（950/936/949），将字节流送入 `MultiByteToWideChar()`。
    - 其他：按 UTF-16BE 读取并进行字节序转换（`_byteswap_ushort`）。
  - 单条名称解析失败会被吞掉（忽略该 name，不影响该 face 的其他信息）。
  - `m_names` 最终会按（type,name）排序去重，避免重复条目。

资源管理：`FontAnalyzer` 使用 PImpl；构造时 `FT_Init_FreeType`，析构时 `FT_Done_FreeType`；每个 face 用 `wil::scope_exit` 确保 `FT_Done_Face()`。

## 8. 并行构建数据库（worker pool）

主流程在 `FontDatabaseBuilder.cpp` 的 “Build database...” 段：

- `fileSet` 为任务列表；共享迭代器 `nextFile` 由 `consumeLock` 保护，保证每个文件只被消费一次。
- 每个工作线程持有独立 `FontAnalyzer` 实例，避免共享解析状态（同时也减少锁竞争）。
- 解析结果为 `vector<FontFaceElement>`；合并进 `db.m_fonts` 时使用 `resultLock` 保护，并通过 move iterator 减少拷贝。
- 进度条由主线程定期读取 `done = nextFile - begin` 打印；输出日志通过 `logLock` 避免多线程输出互相覆盖。

重要特性：

- **输出顺序非确定**：`db.m_fonts` 的插入顺序取决于线程调度与单文件解析耗时；因此生成 XML 的 `FontFace` 顺序不保证稳定。若需要稳定 diff，应在写出前按路径/名称排序（当前未实现）。

## 9. 输出格式与写出实现（FontDatabase::WriteToFile）

`sfh::FontDatabase::WriteToFile()` 位于 `PersistantDataLib/PersistantData.cpp`：

- COM 初始化：`wil::CoInitializeEx()`
- 创建文件流：`SHCreateStreamOnFileEx(..., STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, ...)`
- DOM 构建：`IXMLDOMDocument` 创建根节点 `<FontDatabase>`，每个 `FontFace` 写成一个 `<FontFace>` 元素，属性包括：
  - `path`、`index`、`weight`、`oblique`、`psOutline`
- 名称元素以 `TYPEMAP` 为标签写成子元素（文本内容为名称）：
  - `<Win32FamilyName>…</Win32FamilyName>`
  - `<FullName>…</FullName>`
  - `<PostScriptName>…</PostScriptName>`
- 输出格式化：通过 `IMXWriter` 设置 `UTF-8`、缩进与 standalone，并用 SAX 管线写入流。

最小示例（字段为示意）：

```xml
<FontDatabase>
  <FontFace path="..\Fonts\MyFont.ttc" index="0" weight="400" oblique="0" psOutline="1">
    <Win32FamilyName>My Font</Win32FamilyName>
    <FullName>My Font Regular</FullName>
    <PostScriptName>MyFont-Regular</PostScriptName>
  </FontFace>
</FontDatabase>
```

索引文件的典型消费路径为：`SubtitleFontAutoLoaderDaemon` 在启动时读取 `SubtitleFontHelper.toml`，随后对每个索引文件项调用 `FontDatabase::ReadFromFile()` 加载索引。

索引 XML 的路径行为如下：

- 写出 `FontFace/@path` 时，会优先转换为“相对索引文件目录”的路径；如果跨盘符或无法相对化，则保留绝对路径。
- 读取索引时，无论 XML 中保存的是相对路径还是绝对路径，运行时都会解析为绝对路径后再进入查询与加载链路。
- 因此构建器输出的 XML 更适合随目录一起搬迁，而不会改变 daemon 与注入端看到的运行时路径语义。

## 10. 与 SubtitleFontAutoLoaderDaemon 的契约（简述）

`FontDatabaseBuilder` 输出字段并非“信息越多越好”，而是服务于守护进程的查询与匹配逻辑：

- `Win32FamilyName`：优先用于家庭名查询；守护进程会对长度为 31 的查询开启“截断匹配”（兼容 GDI `LOGFONT::lfFaceName` 限制）。
- `PostScriptName` 与 `FullName`：守护进程会用 `psOutline` 将它们分流：
  - `psOutline==1`：更倾向于 PostScript 名称通道
  - `psOutline!=1`：更倾向于 GDI FullName 通道
- `weight/oblique/psOutline`：会被放入 RPC 响应，用于后续加载决策与筛选。

因此，解析阶段的 OS/2/名称表处理与 `psOutline` 判定，直接影响守护进程命中率与结果质量。

## 11. 本地验证建议（不含自动化测试）

1) 运行构建器生成索引：

```powershell
.\FontDatabaseBuilder.exe -output D:/tmp/FontIndex.xml -dedup -worker 8 D:/Fonts
```

2) 用文本编辑器/浏览器打开 `FontIndex.xml`，确认：
   - 根节点为 `<FontDatabase>`，编码为 UTF-8 且有缩进；
   - 每个 `<FontFace>` 至少包含 `path/index/weight/oblique/psOutline` 属性；
   - 名称子元素存在且内容可读（尤其是中文字体名）。

## 12. 已知限制与注意事项

- `-worker` 线程数应保证 `>= 1`；若为 0，将不会启动工作线程，进度会停滞（当前参数校验未覆盖该场景）。
- 去重阶段在哈希失败时会将记录标记为无效并跳过；遇到大量无法读取的文件时，最终输出会少于发现的文件数。
- 仅解析 Microsoft 平台的名称记录；对非 Microsoft 平台名称（例如 Macintosh 平台）会忽略。
- 扫描使用 Win32 API 递归；极端深目录或超长路径可能触发系统限制，需要在部署环境中评估与规避。
