# Firefox native messaging host（kwin-api-server/native/）

把 Firefox 插件的 native messaging 与 kwin-api-server 的 unix socket 桥接起来。

## 文件

| 文件 | 说明 |
|---|---|
| `kwin-api-host.py` | host 本体（Python 3，仅标准库）。纯字节级转发：Firefox ↔ socket |
| `host-manifest.json` | 清单模板；`xmake install` 把 `@HOST_PATH@` 替换为绝对路径后安装 |
| `test_native_host.py` | 端到端测试：模拟浏览器 + 模拟 daemon，无需 Firefox/KWin |

## 原理

两种帧格式在 Linux 上完全一致：

```
Firefox native messaging（stdin/stdout）：
    4 字节长度（uint32，本机字节序）+ UTF-8 JSON 消息
kwin-api-server socket（doc/PROTOCOL.md §2.2）：
    4 字节长度（uint32，本机字节序）+ UTF-8 JSON 帧
```

所以 host 不做任何 JSON 解析：stdin 收到的帧原样写 socket，
socket 收到的帧原样写 stdout。退出条件：任一侧 EOF 即退出（退出码 0）。

socket 路径：`$KWIN_API_SOCKET`（覆盖，测试用）或
`$XDG_RUNTIME_DIR/kwin-api-server/service.socket`。host 是自包含的单个
脚本（只依赖 Python 标准库），安装到哪个前缀都能用。

## 安装（通过 xmake）

`xmake.lua` 里的 `kwin-api-native` target 负责安装 host 与清单：

| 安装项 | 路径 |
|---|---|
| host 脚本 | `<prefix>/libexec/kwin-api-server/kwin-api-host.py`（0755） |
| 清单（渲染后） | `<prefix>/lib/firefox/native-messaging-hosts/org.plasma_tweak.kwin_api.json` |

```sh
sudo xmake install                    # 装到 /usr
```

当安装前缀是 `/usr` 时，清单还会额外复制到 Firefox 实际扫描的系统目录
`/usr/lib/mozilla/native-messaging-hosts/` 与
`/usr/lib64/mozilla/native-messaging-hosts/`（哪个存在装哪个），
所以 `sudo xmake install` 一次搞定，Firefox 直接能找到 host。

卸载：

```sh
sudo xmake uninstall                  # 或 xmake uninstall --installdir=<dir>
```

### 无 root 的临时安装（staged / dev）

```sh
xmake install -o ./stage              # 装到 stage/ 前缀（不改 /usr）
# 让 Firefox 用上 staged 的 host（路径在 stage 内，stage 目录要留着）：
cp stage/lib/firefox/native-messaging-hosts/org.plasma_tweak.kwin_api.json \
   ~/.mozilla/native-messaging-hosts/
```

清单的 `allowed_extensions` 是 `kwin-window-manager@plasma-tweak.local`，
必须与 `extension/manifest.json` 的 `browser_specific_settings.gecko.id`
一致。改 id 时要两处同步改（`host-manifest.json` 与插件 manifest）。

## 测试

```sh
python3 test_native_host.py
```

覆盖：请求→响应往返、daemon 主动通知转发、daemon 断开时 host 退出、
id-null 请求转发（无响应）。host 侧日志在 stderr，测试失败时会打印出来辅助排查。

## 手动冒烟

```sh
# 起一个假 daemon（任意能回 JSON-RPC 的 unix socket 服务），然后：
printf '\x00\x00\x00\x00...' | KWIN_API_SOCKET=/tmp/x.sock python3 kwin-api-host.py
# 或检查安装后的 host 能否连上 socket：
python3 <prefix>/libexec/kwin-api-server/kwin-api-host.py --check
```
