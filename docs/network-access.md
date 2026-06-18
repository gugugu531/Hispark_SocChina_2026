# 板卡网络接入与远程连接

## 1. 两种访问模式

| 模式 | 板端接口 | 地址 | 用途 |
| --- | --- | --- | --- |
| 受管网络 | `eth1` | DHCPv4 + DHCPv6，均为动态地址 | 日常远程开发；电脑通过同一受管网络使用 IPv6 连接 |
| 电脑网线直连 | `eth0` | `192.168.1.168/24` | 受管网络不可用时的回退路径 |

不要把实测租约地址写成永久板卡地址。当前租约通常会因稳定的 DHCP Client ID / DUID
在短期重启后续租为同一地址，但 DHCP 服务器仍有权更改它。

## 2. 受管网络自动接入

认证与网络安装实现不属于本仓库，也不应从仓库文档链接到其存放位置。本仓库只记录
通用访问约定和已验证的网络行为。

启动流程：

```text
systemd-networkd
  -> eth1 DHCPv4 + DHCPv6 + IPv6 RA
  -> 清理厂商 search_tool 注入的无链路 eth0 默认路由
  -> Web Portal 认证
  -> sshd 通过板端全局 IPv6 提供远程连接
```

认证凭据和认证实现不得进入 Git。

## 3. 查询当前地址

板端串口：

```sh
ip -4 -brief address show eth1
ip -6 -brief address show eth1
networkctl status eth1
```

电脑已知道上一次 IPv6 地址时：

```sh
ssh -6 root@<BOARD_IPV6> 'ip -brief address show eth1'
```

若动态 IPv6 已变化且旧地址不可达，使用串口查询新地址。当前方案没有依赖公网 DDNS，
因此不能保证在完全不知道地址时仅靠网络自动发现板卡。

## 4. 电脑端连接

推荐先在 `~/.ssh/config` 建立别名；地址变化时只更新一处：

```sshconfig
Host hispark-remote
    HostName <BOARD_IPV6>
    User root
    AddressFamily inet6
    BindAddress <PC_WIFI_IPV6>

Host hispark-direct
    HostName 192.168.1.168
    User root
```

然后统一使用：

```sh
ssh hispark-remote
BOARD=hispark-remote scripts/deploy_board.sh
BOARD=hispark-remote scripts/run_board.sh
```

`scripts/deploy_board.sh` 和 `scripts/run_board.sh` 要求显式设置 `BOARD`，防止在受管网络与
电脑直连之间误选目标。

## 5. Clash Verge TUN

Clash TUN 会增加 IPv4/IPv6 策略路由。测试受管网络原生通路时，应显式绑定电脑 Wi-Fi
的全局 IPv6：

```sh
PC_WIFI_IPV6=$(ip -6 -o addr show dev wlp4s0 scope global \
  | awk '!/ temporary / {sub(/\\/.*/, "", $4); print $4; exit}')

ip -6 route get <BOARD_IPV6> from "$PC_WIFI_IPV6"
ping -6 -I "$PC_WIFI_IPV6" <BOARD_IPV6>
ssh -6 -o BindAddress="$PC_WIFI_IPV6" root@<BOARD_IPV6>
```

使用上面的 SSH 别名后，`BindAddress` 已固化在配置中，部署脚本无需知道 Clash 的细节。

## 6. 地址稳定性

- DHCPv4 租期实测为 1 小时，T1 约 30 分钟。持续联网时会自动续租；长时间断网、交换机
  端口或 DHCP 策略变化后可能更换。
- DHCPv6 实测有效期约 3 天、优选期约 45 小时。networkd DUID 稳定，完整重启曾续租到
  同一地址，但仍不是静态地址。
- 用于短期开发和演示足够稳定；长期无人值守连接应补充地址发现或受控 DDNS。

## 7. 故障恢复

```sh
systemctl status portal-network.service --no-pager
journalctl -u portal-network.service -b --no-pager
networkctl status eth1
ip route
ip -6 route
```

检查要点：

- `eth1` 应为 `routable (configured)`。
- IPv4 默认路由应走 DHCP 网关，而不是无链路的 `eth0 -> 192.168.1.1`。
- `eth1` 应同时具有全局 IPv4、全局 IPv6 和链路本地 IPv6。
- 公网 ICMP 可能被网络策略丢弃，认证状态应结合 Portal 日志、HTTP 和 SSH 判断。
