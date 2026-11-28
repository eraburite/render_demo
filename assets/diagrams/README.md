# assets/diagrams —— 设计书的插图

`DESIGN.md` 里的 8 张架构图/时序图在这里以 **预渲染 SVG + mermaid 源码** 的形式入库。

## 为什么不用 GitHub 的 mermaid 渲染

GitHub 并不自己画 mermaid，而是把每个 `mermaid` 代码块塞进 `viewscreen.githubusercontent.com`
的**跨域 iframe** 里渲染。这条链路一旦不通（网络受限、被扩展拦截、或 iframe 还没拿到尺寸
就开始量文字），`flowchart` / `classDiagram` 会直接失败，页面只剩「Unable to render rich
display + 源码」，`sequenceDiagram` 却可能仍然正常——同一份文档里一半图能看、一半报错。

实测（2026-11-28）：

| 环境 | 结果 |
| --- | --- |
| 本地 mermaid 10.9.1 解析 8 块 | 8/8 通过 |
| 本地 mermaid 11.17.2 解析 + 渲染 8 块 | 8/8 通过 |
| GitHub 页面（真实浏览器） | 4 张 sequence 成图；2 张 flowchart + 2 张 classDiagram 报错 |
| GitHub 图框内部报错 | `svg element not in render tree`（mermaid 量文字时 `getBBox()` 拿不到布局） |
| 对照组：mermaid 官方仓库 README | 10 张图里 7 张同样报错 |

结论：**图的语法没有问题，是 GitHub 的渲染链路不可靠**。因此把图预渲染成 SVG 提交进仓库，
正文用 `![](…)` 引用——图片走 GitHub 自己的图片管道，不依赖那个跨域 iframe。

## 目录

```
assets/diagrams/
├── mermaid.config.json          # 渲染配置
├── README.md                    # 本文件
├── src/*.mmd                    # mermaid 源码（改图改这里）
└── *.svg                        # 生成物（正文引用的是这些）
```

## 重新生成

需要 Node 18+。用本机已装的 Chrome/Edge 即可，不必让 puppeteer 再下一份 Chromium：

```bash
npm i -g @mermaid-js/mermaid-cli

cd assets/diagrams
export PUPPETEER_EXECUTABLE_PATH="/c/Program Files (x86)/Microsoft/Edge/Application/msedge.exe"   # 换成自己的浏览器路径
for f in src/*.mmd; do
  npx mmdc -i "$f" -o "$(basename "${f%.mmd}").svg" \
      -c mermaid.config.json -b white -w 1600
done
```

Windows PowerShell：

```powershell
$env:PUPPETEER_EXECUTABLE_PATH = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
Get-ChildItem src\*.mmd | ForEach-Object {
  npx mmdc -i $_.FullName -o "$($_.BaseName).svg" -c mermaid.config.json -b white -w 1600
}
```

**生成后必须做两件后处理**（`mmdc` 不会替你做）：

1. 根节点后插一个白底矩形：`<rect width="100%" height="100%" fill="#ffffff"/>`；
2. 把 mermaid 在 `foreignObject` 里输出的未闭合 `<br>` 改成 `<br/>`——否则 SVG 不是合法
   XML，浏览器会整张拒渲染。

用 `python -c "import xml.etree.ElementTree as ET; ET.parse('xxx.svg')"` 可以验证第 2 点。

## 约定

- **改图只改 `src/*.mmd`**，再重生成对应 SVG，两个文件一起提交。
- 白底是刻意的：GitHub 深色主题下不会出现深字压深底。
- `htmlLabels: true`（默认）是刻意的：标签用 `foreignObject` 里的 HTML 排版，mermaid 能按
  真实字体宽度算框大小；改成 `false` 后中文/多行标签会溢出框外。`foreignObject` 在
  Chrome / Firefox / Safari 的 `<img>` 里都能渲染。
- SVG 里不含脚本、不含外链，符合 GitHub 的图片策略。
- 图很宽（例如分层架构 3010 px），正文按容器宽度缩放后字会偏小——正文里用
  `[![…](x.svg)](x.svg)` 包一层链接，点开就是原尺寸。

## 文件对照

| 源码 | 生成物 | 用在 |
| --- | --- | --- |
| `src/architecture-layers.mmd` | `architecture-layers.svg` | 3.1 分层架构 |
| `src/architecture-classes.mmd` | `architecture-classes.svg` | 3.2 核心类关系 |
| `src/frame-loop-sequence.mmd` | `frame-loop-sequence.svg` | 3.3 每帧时序（LoopSequence） |
| `src/once-upload-sequence.mmd` | `once-upload-sequence.svg` | 3.3 一次性上传（OnceSequence） |
| `src/descriptor-layers.mmd` | `descriptor-layers.svg` | 3.4 描述符分层与多 Pass 组合 |
| `src/device-resource-classes.mmd` | `device-resource-classes.svg` | 4.6 资源层（IDeviceResource 体系） |
| `src/descriptor-binding-sequence.mmd` | `descriptor-binding-sequence.svg` | 4.10 描述符分层的落地实现对照 |
| `src/frame-sync-sequence.mmd` | `frame-sync-sequence.svg` | 6 同步模型总览 |
