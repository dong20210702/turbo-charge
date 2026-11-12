# turbo-charge

**开始上班，随缘更新**

酷安：[诺鸡鸭](https://www.coolapk.com/u/1145497)

## 模块功能

删除温控，关闭阶梯式充电，持续修改电池温度及充电电流，尽可能的让手机满血快充。配置文件在/data/adb/turbo-charge/option.txt，可以更改一些参数，日志文件为同目录的log.txt

## 使用到的工具

1.使用自己构建的[aarch64-linux-musl-gcc](https://github.com/chase535/aarch64-linux-musl-gcc)交叉编译工具链进行编译链接，静态链接更高效，程序大小相较于使用基于glibc函数库的gnu-gcc生成的程序来说大量减少

2.使用[mimalloc](https://github.com/microsoft/mimalloc)代替原版malloc家族，以略微增加程序大小为代价，解决musl函数库malloc家族执行效率低的短板，提高程序运行效率

3.使用[CMake](https://cmake.org)自动生成Makefile文件（别问，问就是懒，不想学Makefile）

## 适配

**我并不会适配不支持的机型，但您可通过提交PR来主动适配，亦可通过提交PR来优化程序**

适配需要文件，但问题就在这，我也不知道需要哪些文件，之前收集过很多不适配的手机的各种文件，但是发现并没有需要的文件，而且没有的很彻底，所以我无从下手，无法进行适配

## 运行机制与排错

1.程序由模块的service.sh以守护方式启动，意外退出会自动重启；只有日志里明确写着“判定不适配”或“内部错误”时才会停止重启

2.同一时刻只允许一个实例运行（文件锁 /data/adb/turbo-charge/lock），重装模块不重启手机也不会出现两个实例互相抢写节点

3.运行期间某个内核节点短暂不可用（拔插充电器、驱动重新注册节点）不会再结束进程，只会跳过本次操作并在下一次循环重试，日志里会出现“暂时无法读取…”的提示

4.只在开机时写一次的那些内核参数，程序每30秒复查一次，发现被系统改回就重新写入，日志里会出现“检测到N项内核参数被系统改回，已重新写入”

5.排查“快充不生效”时请先看log.txt：进程是否还在（`ps -A | grep turbo-charge`）、有没有“判定不适配”、有没有限制电流的记录

## CI构建与自动发版

本仓库（fork）已接入完整自动化，全部在 [Actions](https://github.com/dong20210702/turbo-charge/actions) 完成：

1.**自动编译**：push 到 main / 提 PR 都会用官方交叉编译工具链编译一次，产物在对应运行的 Artifacts 里。编译带 `-W -Wall -Werror`，代码有告警即失败，所以能编过就代表没引入新的坏味道

2.**自动发版**：push 时 CI 会读 `module/module.prop` 的 versionCode，只有比线上 `update_channel/turbo-charge.json` 里的大才会发布。发布动作包括：打 tag、建 GitHub Release、把 zip 写回 `update_channel/`、刷新 OTA 通道、清 jsdelivr 缓存。**所以升级流程只有三步：改代码 → 调高 version 和 versionCode → push**

3.**自动升级**：模块的 updateJson 指向 jsdelivr 上的 OTA 通道（`https://cdn.jsdelivr.net/gh/dong20210702/turbo-charge@main/update_channel/turbo-charge.json`），Magisk 打开模块页时会比对 versionCode 并提示更新，点一下即在 Magisk 内完成升级。注意它不会静默自动安装，是"提示 + 一键升级"

4.想强制重发同一个 versionCode，去 Actions 手动跑 Build 并勾选 `force_publish`

> 说明：Magisk 不会定期静默升级，需要打开 Magisk App 的模块页才会检查；updateJson 是模块安装时写进设备的，所以从旧版（v68 及之前，指向作者的 gitee 通道）升到本仓库的版本，需要手动刷一次 v69，之后才会走本仓库的 OTA 通道。

## 开源协议

程序遵循**AGPLv3**开源协议
