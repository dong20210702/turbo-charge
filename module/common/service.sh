#!/system/bin/sh
MODDIR=${0%/*}
DATA=/data/adb/turbo-charge
LOG=$DATA/log.txt
[[ -f $LOG ]] && mv -f $LOG $LOG.old
echo "等待手机启动完毕，以确保时间准确。若只看到这一行内容，请立即联系模块作者！" > $LOG
until [[ "$(getprop service.bootanim.exit)" == "1" ]]; do
    sleep 1
done
echo "手机启动完毕" >> $LOG
echo "" >> $LOG
#等待电池节点就绪（部分机型开机后节点比开机动画更晚出现），最多等20秒
for i in $(seq 20); do
    [[ -f /sys/class/power_supply/battery/status ]] && break
    sleep 1
done
#清空云控温控目录
[[ -e /data/vendor/thermal ]] && chattr -i $(find /data/vendor/thermal)
rm -rf /data/vendor/thermal
mkdir -p /data/vendor/thermal/config
chattr +i /data/vendor/thermal/config /data/vendor/thermal
#守护式启动：
#旧版本在开机时启动了两个实例，60秒后按进程名杀掉其中一个，一旦进程名解析出错或者被杀的是唯一的那个实例，
#快充就会彻底失效且没有任何提示，只能重启手机。现在只保留一个实例（程序内部用文件锁互斥），
#并且程序意外退出时自动重启，只有明确判定“不适配”或“内部错误”时才停止
(
    retry=0
    unsupported=0
    while :; do
        ${MODDIR}/turbo-charge >> $LOG 2>&1
        rc=$?
        if [[ "$rc" == "0" || "$rc" == "100" ]]; then
            echo "程序退出（退出码$rc），不再重启" >> $LOG
            break
        fi
        if [[ "$rc" == "98765" || "$rc" == "9988" ]]; then
            echo "程序内部错误（退出码$rc），不再重启，请携带日志反馈" >> $LOG
            break
        fi
        if [[ "$rc" == "800" || "$rc" == "1000" ]]; then
            unsupported=$((unsupported+1))
            if [[ $unsupported -ge 3 ]]; then
                echo "本机缺少模块所需的全部文件，程序判定不适配（退出码$rc），不再重启" >> $LOG
                break
            fi
        fi
        retry=$((retry+1))
        [[ $retry -gt 12 ]] && retry=12
        echo "程序意外退出（退出码$rc），$((retry*5))秒后自动重启" >> $LOG
        sleep $((retry*5))
    done
) >/dev/null 2>&1 &
