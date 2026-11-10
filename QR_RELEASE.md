# FBI 二维码固定发布流程

这份流程是唯一支持的二维码方案。以后每个版本只替换版本号，不再临时选择
CDN、GitHub Release 跳转链接或本地路径。

## 固定地址

- 原始 CIA 必须提交为：`downloads/bilibili-v<版本>.cia`
- FBI/CDN 传输副本必须提交为：`downloads/bilibili-v<版本>.cia.woff2`
- 二维码编码内容必须是：
  `https://quantil.jsdelivr.net/gh/baldicoot123/bilibili-3ds@main/downloads/bilibili-v<版本>.cia.woff2`
- 二维码 PNG 必须提交为：`downloads/bilibili-v<版本>-fbi-qr.png`
- 给用户展示 PNG 时必须使用：
  `https://raw.githubusercontent.com/baldicoot123/bilibili-3ds/main/downloads/bilibili-v<版本>-fbi-qr.png`

## 生成命令

CIA 合并到 `main` 后运行：

```sh
python tools/make_fbi_qr.py v1.8.0 --cia release/v1.8.0/bilibili.cia
```

脚本只有在以下检查全部通过后才会写出 PNG：

所有请求必须使用 FBI 2.6.1 的真实请求头：
`User-Agent: FBI/2.6.1` 和 `Accept-Encoding: gzip, deflate`。

1. HEAD 返回 HTTP 200；
2. 全程零重定向；
3. `Content-Length` 与本地 CIA 完全一致；
4. 响应没有 `Content-Encoding`（禁止 gzip/deflate）；
5. 服务端声明 `Accept-Ranges: bytes`；
6. 64KiB Range 请求返回 HTTP 206；
7. `Content-Range` 中的完整大小正确；
8. 远端前 64KiB 与本地 CIA 逐字节一致；
9. 使用相同 FBI 请求头下载完整文件，字节数与 SHA-256 均与本地 CIA 一致；
10. 生成的二维码必须反解回预期的 `.cia.woff2` URL。

生成后提交二维码 PNG，再用脚本打印的 Raw URL 展示给用户。

## 禁止使用

- `cdn.jsdelivr.net`：此前响应缺少 FBI 依赖的完整长度信息；
- Quantil/Fastly 上的普通 `.cia` URL：在 FBI 请求头下会被 gzip，长度和文件头
  都会变化；必须使用内容不变的 `.cia.woff2` 传输副本；
- GitHub Release 的 CIA 地址：会重定向到临时签名 URL；
- GitHub Release 的二维码附件地址：响应为下载附件，浏览器可能不显示；
- `release-assets.githubusercontent.com`：临时签名会过期；
- `C:\...` 本地路径：只能在生成二维码的电脑上访问。

任何一项验证失败，都不得发布二维码或声称可以由 FBI 安装。
