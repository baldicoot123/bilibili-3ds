# FBI 二维码固定发布流程

这份流程是唯一支持的二维码方案。以后每个版本只替换版本号，不再临时选择
CDN、GitHub Release 跳转链接或本地路径。

## 固定地址

- CIA 必须提交为：`downloads/bilibili-v<版本>.cia`
- 二维码编码内容必须是：
  `https://quantil.jsdelivr.net/gh/baldicoot123/bilibili-3ds@main/downloads/bilibili-v<版本>.cia`
- 二维码 PNG 必须提交为：`downloads/bilibili-v<版本>-fbi-qr.png`
- 给用户展示 PNG 时必须使用：
  `https://raw.githubusercontent.com/baldicoot123/bilibili-3ds/main/downloads/bilibili-v<版本>-fbi-qr.png`

## 生成命令

CIA 合并到 `main` 后运行：

```sh
python tools/make_fbi_qr.py v1.8.0 --cia release/v1.8.0/bilibili.cia
```

脚本只有在以下检查全部通过后才会写出 PNG：

1. HEAD 返回 HTTP 200；
2. 全程零重定向；
3. `Content-Length` 与本地 CIA 完全一致；
4. 服务端声明 `Accept-Ranges: bytes`；
5. 64KiB Range 请求返回 HTTP 206；
6. `Content-Range` 中的完整大小正确；
7. 远端前 64KiB 与本地 CIA 逐字节一致。

生成后提交二维码 PNG，再用脚本打印的 Raw URL 展示给用户。

## 禁止使用

- `cdn.jsdelivr.net`：此前响应缺少 FBI 依赖的完整长度信息；
- GitHub Release 的 CIA 地址：会重定向到临时签名 URL；
- GitHub Release 的二维码附件地址：响应为下载附件，浏览器可能不显示；
- `release-assets.githubusercontent.com`：临时签名会过期；
- `C:\...` 本地路径：只能在生成二维码的电脑上访问。

任何一项验证失败，都不得发布二维码或声称可以由 FBI 安装。
