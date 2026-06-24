# 前端依赖

## hls.js

LL-HLS 浏览器播放库。固定版本，本地部署，不使用 CDN。

- 建议版本：hls.js v1.5.x
- 下载：https://github.com/video-dev/hls.js/releases
- 文件：`hls.min.js` → 放置于 `/usr/share/socchina-web/vendor/hls.min.js`

### 部署时自动下载

```sh
# 在 install_board_web.sh 执行前运行：
curl -L -o web/ui/vendor/hls.min.js \
  https://cdn.jsdelivr.net/npm/hls.js@1.5/dist/hls.min.js
```

### 验收记录

| 日期 | 版本 | 校验值 (sha256) | 验收人 | 备注 |
| --- | --- | --- | --- | --- |
| — | — | — | — | 待首次部署验收后填写 |
