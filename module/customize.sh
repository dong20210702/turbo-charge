SKIPUNZIP=1

check_file()
{
    ui_print " --- 检查所需文件是否存在 ---"
    temp_file=$(ls /sys/class/power_supply/*/temp 2>/dev/null)
    # 逐个检查电流节点，读出来是负数的直接跳过
    # MTK 主从充电架构里，未启用的从充电器节点（mtk-slave-charger、mtk-mst-div-chg 等）读出来是 -95 这类错误码，
    # 路径明明存在但写进去毫无效果，早期版本就是被这些节点骗过去的
    current_max_file=""
    for f in /sys/class/power_supply/*/constant_charge_current_max /sys/class/power_supply/*/constant_charge_current /sys/class/power_supply/*/fast_charge_current /sys/class/power_supply/*/input_current_limit /sys/class/power_supply/*/thermal_input_current; do
        [[ -e "${f}" ]] || continue
        node_value=$(cat "${f}" 2>/dev/null)
        case "${node_value}" in -*) continue ;; esac
        current_max_file="${current_max_file} ${f}"
    done
    for i in $(ls /sys/class/thermal | grep "thermal_zone"); do
        if [[ -f "/sys/class/thermal/${i}/type" ]]; then
            temp_sensor=$(cat /sys/class/thermal/${i}/type 2>/dev/null)
            for j in lcd_therm quiet_therm conn_therm modem_therm wifi_therm mtktsbtsnrpa mtktsbtsmdpa mtktsAP modem-0-usr modem1_wifi ddr-usr cwlan-usr; do
                [[ "${temp_sensor}" == "${j}" ]] && have_temp_sensor=1 && break
            done
        fi
        [[ -n "${have_temp_sensor}" ]] && break
    done
    if [[ ! -f "/sys/class/power_supply/battery/status" ]]; then
        no_battery_status=1
    fi
    if [[ ! -f "/sys/class/power_supply/battery/capacity" ]]; then
        no_battery_capacity=1
    fi
    if [[ ! -f "/sys/class/power_supply/battery/charging_enabled" && ! -f "/sys/class/power_supply/battery/battery_charging_enabled" && ! -f "/sys/class/power_supply/battery/input_suspend" && ! -f "/sys/class/qcom-battery/restricted_charging" ]]; then
        no_suspend_file=1
    fi
    if [[ -n "${no_battery_status}" ]]; then
        ui_print " ！由于找不到有关文件，“伪”旁路供电功能无法根据手机的充电状态而自动启停，详情请看程序运行时的log文件！"
    fi
    if [[ -n "${no_battery_status}" || -n "${no_battery_capacity}" || -n "${no_suspend_file}" ]]; then
        ui_print " ！由于找不到有关文件，电量控制功能失效，详情请看程序运行时的log文件！"
        no_power_control=1
    fi
    if [[ ! -f "/sys/class/power_supply/battery/step_charging_enabled" ]]; then
        ui_print " ！由于找不到有关文件，阶梯充电控制的所有功能失效，详情请看程序运行时的log文件！"
        no_step_charging=1
    elif [[ -n "${no_battery_capacity}" ]]; then
        ui_print " ！由于找不到部分有关文件，阶梯式充电控制的部分功能失效，详情请看程序运行时的log文件！"
    fi
    if [[ -z "${current_max_file}" ]]; then
        ui_print " ！！！警告：找不到任何可写的充电电流节点，模块将完全不起作用！！！"
        ui_print " ！！！开机后程序会在 /data/adb/turbo-charge/probe.txt 生成节点清单，请把它发给模块作者"
        no_current_change=1
    fi
    if [[ -z "${temp_file}" || -n "${no_battery_status}" ]]; then
        ui_print " ！由于找不到有关文件，充电时强制显示28℃功能失效，详情请看程序运行时的log文件！"
        no_force_temp=1
    fi
    if [[ -z "${no_force_temp}" || -z "${no_current_change}" ]]; then
        if [[ -z "${have_temp_sensor}" ]]; then
            if [[ -n "${no_force_temp}" ]]; then
                ui_print " ！由于找不到有关温度传感器，温度控制功能失效，详情请看程序运行时的log文件！"
            else
                ui_print " ！由于找不到有关温度传感器，温度控制及充电时强制显示28℃功能失效，详情请看程序运行时的log文件！"
                no_force_temp=1
            fi
        fi
        [[ -n "${no_step_charging}" && -n "${no_power_control}" && -n "${no_force_temp}" && -n "${no_current_change}" && -z "${have_temp_sensor}" ]] && force_exit=1
    else
        [[ -n "${no_step_charging}" && -n "${no_power_control}" && -n "${no_force_temp}" && -n "${no_current_change}" ]] && force_exit=1
    fi
    if [[ -n "${force_exit}" ]]; then
        ui_print " ！所有的所需文件均不存在，完全不适配此手机，安装失败！"
        ui_print " "
        rm -rf ${MODPATH}
        exit 1
    fi
    ui_print " - 检查完毕，开始安装"
}

print_modname()
{
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
    ui_print " - 模块: $(grep '^name=' ${TMPDIR}/module.prop | sed 's/^name=//g')"
    ui_print " - 模块版本: $(grep '^version=' ${TMPDIR}/module.prop | sed 's/^version=//g')"
    ui_print " - 作者: $(grep '^author=' ${TMPDIR}/module.prop | sed 's/^author=//g')"
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
}

print_info()
{
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
    ui_print " - 可修改/data/adb/turbo-charge/option.txt来更改一些参数，即时生效"
    ui_print " - log文件为同目录的log.txt，包含文件检测、配置更改等信息"
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
    ui_print " ！！！如果安装模块后卡开机，请删除模块目录下的system文件夹！！！"
    ui_print " "
    ui_print " ！！！卸载模块请务必在Magisk中卸载！！！"
    ui_print " ！！！或手动执行模块目录下的uninstall.sh文件后再删除模块目录！！！"
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
    ui_print " -      ↓模块介绍↓"
    ui_print " - 删除温控，充电时持续修改电池温度，让系统认为电池温度一直是28℃"
    ui_print " - 持续修改充电电流，以达到最快充电速度"
    ui_print " - 可选添加温控，若添加，默认当手机温度高于58℃时最高充电电流限制为6A"
    ui_print " - 可选添加电量控制，若添加，默认电量到达95%时断电，小于等于80%时恢复充电"
    ui_print " - 可选是否关闭阶梯式充电，若选是，默认电量大于15%时关闭阶梯式充电，电量小于等于15%时开启阶梯式充电"
    ui_print " - 可选是否启用“伪”旁路充电，但请注意是“伪”，因为并不是真的旁路供电，而是直接将充电电流降低至500mA"
    ui_print " "
    ui_print " ！！！若手机体感温度过高，请立即拔下充电器并将手机静置在阴凉处！！！"
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
}

check_old_option()
{
    for i in $(seq $#); do
        VARIABLE=$(eval echo '${'"${i}"'}')
        VALUE=$(grep "^${VARIABLE}=" /data/adb/turbo-charge/option.txt | sed "s/^${VARIABLE}=//g")
        VALUE_DEFAULT=$(grep "^${VARIABLE}=" ${TMPDIR}/option.txt | sed "s/^${VARIABLE}=//g")
        if [[ -z "${VALUE}" ]]; then
            ui_print "  - ${VARIABLE}不存在，使用默认值${VALUE_DEFAULT}"
        elif [[ -z "$(echo "${VALUE}" | grep '^[[:digit:]]*$')" ]]; then
            ui_print "  - ${VARIABLE}的值为空或非纯数字（正整数），使用默认值${VALUE_DEFAULT}"
        elif [[ "${VALUE}" -gt 2147483647 ]]; then
            ui_print "  - ${VARIABLE}的值大于2147483647，这是不被允许的，使用默认值${VALUE_DEFAULT}"
        elif [[ "${VALUE}" -eq 0 && "${VARIABLE}" == "CYCLE_TIME" ]]; then
            ui_print "  - ${VARIABLE}的值等于0，这是不被允许的，使用默认值${VALUE_DEFAULT}"
        else
            ui_print "  - ${VARIABLE}的值为${VALUE}，将此值写入新配置文件"
            sed -i "s/^${VARIABLE}=.*/${VARIABLE}=${VALUE}/g" ${TMPDIR}/option.txt
        fi
    done
}

on_install()
{
    if [[ -f /data/adb/turbo-charge/option.txt ]]; then
        ui_print " 旧配置文件存在，开始读取旧配置文件的配置"
        check_old_option CYCLE_TIME FORCE_TEMP CURRENT_MAX STEP_CHARGING_DISABLED
        check_old_option TEMP_CTRL POWER_CTRL STEP_CHARGING_DISABLED_THRESHOLD
        check_old_option CHARGE_STOP CHARGE_START TEMP_MAX HIGHEST_TEMP_CURRENT
        check_old_option RECHARGE_TEMP BYPASS_CHARGE
        # 熔断限流值低于3A时按新默认4A处理
        # 2A在4.2V电池上只有约8.4W，一旦进入温度熔断，充电慢到几乎停滞。
        # v73起熔断改成带滞回的常态策略（不再是"偶尔触发一次的极端保护"），
        # 限流值继续留在2A会让保护区间变成"基本不充电"，所以低值统一升到4A
        _old_htc=$(grep "^HIGHEST_TEMP_CURRENT=" /data/adb/turbo-charge/option.txt | sed "s/^HIGHEST_TEMP_CURRENT=//g")
        if [[ -n "${_old_htc}" ]] && echo "${_old_htc}" | grep -q '^[0-9][0-9]*$' && [[ "${_old_htc}" -lt 3000000 ]]; then
            sed -i "s/^HIGHEST_TEMP_CURRENT=.*/HIGHEST_TEMP_CURRENT=4000000/g" ${TMPDIR}/option.txt
            ui_print "  - 旧的HIGHEST_TEMP_CURRENT=${_old_htc}偏低，熔断时功率不足8W，已更新为4000000（约16.8W）"
        fi
        # 旧的FORCE_TEMP_MAX=45正好压在多数机型充电常态温度区间（45~47℃），
        # 会让充电大部分时间处于熔断限流，不再沿用旧值，直接使用新默认48℃
        _old_ftm=$(grep "^FORCE_TEMP_MAX=" /data/adb/turbo-charge/option.txt | sed "s/^FORCE_TEMP_MAX=//g")
        if [[ -n "${_old_ftm}" ]] && echo "${_old_ftm}" | grep -q '^[0-9][0-9]*$'; then
            if [[ "${_old_ftm}" -lt 47 ]]; then
                ui_print "  - 旧的FORCE_TEMP_MAX=${_old_ftm}℃偏低（充电常态温度就在45~47℃），已改用新的49℃"
            else
                ui_print "  - 旧的FORCE_TEMP_MAX=${_old_ftm}℃保留"
                sed -i "s/^FORCE_TEMP_MAX=.*/FORCE_TEMP_MAX=${_old_ftm}/g" ${TMPDIR}/option.txt
            fi
        fi
        # 上一版(v74)的默认组合是 熔断48/解除45/限流4A。
        # 实测该组合仍会在47.9℃触发熔断，把34W打到16.6W；v75把三值一起改为 49/46/6A。
        # 只在三项"完全等于上一版默认"时才整体升级，避免覆盖用户自己调过的值
        _old_gr=$(grep "^GUARD_RECOVER=" /data/adb/turbo-charge/option.txt | sed "s/^GUARD_RECOVER=//g")
        if [[ "${_old_ftm}" == "48" && "${_old_gr}" == "45" && "${_old_htc}" == "4000000" ]]; then
            sed -i "s/^FORCE_TEMP_MAX=.*/FORCE_TEMP_MAX=49/g; s/^GUARD_RECOVER=.*/GUARD_RECOVER=46/g; s/^HIGHEST_TEMP_CURRENT=.*/HIGHEST_TEMP_CURRENT=6000000/g" ${TMPDIR}/option.txt
            ui_print "  - 检测到上一版的默认温度策略（48℃/45℃/4A），已整体升级为49℃/46℃/6A"
        fi
        # v76：旧默认 TEMP_MAX=52 会被满速充电时的主板温度（实测峰值54.4℃）长期触发，
        # 把功率压在26W并且几乎出不来；RECHARGE_TEMP=45 永远达不到，等于进了出不去。
        # 只对等于旧默认的值做升级，用户自己调过的保持不动
        _old_tmx=$(grep "^TEMP_MAX=" /data/adb/turbo-charge/option.txt | sed "s/^TEMP_MAX=//g")
        _old_rt=$(grep "^RECHARGE_TEMP=" /data/adb/turbo-charge/option.txt | sed "s/^RECHARGE_TEMP=//g")
        if [[ "${_old_tmx}" == "52" ]]; then
            sed -i "s/^TEMP_MAX=.*/TEMP_MAX=58/g" ${TMPDIR}/option.txt
            ui_print "  - 旧的TEMP_MAX=52℃会被满速充电的主板温度长期触发，已更新为58℃"
        fi
        if [[ "${_old_rt}" == "45" ]]; then
            sed -i "s/^RECHARGE_TEMP=.*/RECHARGE_TEMP=50/g" ${TMPDIR}/option.txt
            ui_print "  - 旧的RECHARGE_TEMP=45℃低于限流后的稳定温度，等于进得去出不来，已更新为50℃"
        fi
    else
        ui_print " 旧配置文件不存在，使用默认配置"
        ui_print "  - 添加温控，当温度高于58℃时限制最高充电电流为6A，低于50℃时恢复"
        ui_print "  - 不添加电量控制"
        ui_print "  - 不关闭阶梯式充电"
        ui_print "  - 启用强制显示28℃功能"
        ui_print "  - 禁用“伪”旁路供电功能"
    fi
    ui_print " "
    ui_print " ********************************************************"
    ui_print " "
    [[ -f /data/adb/turbo-charge/bypass_charge.txt ]] && BYPASS_APP=$(grep -v -E "#|^$" /data/adb/turbo-charge/bypass_charge.txt)
    if [[ -n "${BYPASS_APP}" ]]; then
        echo " 以下APP包名位于旧的“伪”旁路供电列表中，将会写入到新的“伪”旁路供电列表"
        for s in ${BYPASS_APP}; do
            ui_print "  - ${s}"
            echo "${s}" >> ${TMPDIR}/bypass_charge.txt
        done
        ui_print " "
        ui_print " ********************************************************"
        ui_print " "
    fi
    cp -af ${TMPDIR}/turbo-charge ${TMPDIR}/module.prop ${TMPDIR}/service.sh ${TMPDIR}/uninstall.sh ${MODPATH}
    [[ ! -d /data/adb/turbo-charge ]] && mkdir -p /data/adb/turbo-charge
    cp -af ${TMPDIR}/option.txt ${TMPDIR}/bypass_charge.txt /data/adb/turbo-charge
    for k in /system/bin /system/etc/init /system/etc/perf /system/vendor/bin /system/vendor/etc /system/vendor/etc/init /system/vendor/etc/perf; do
        all_thermal="${all_thermal} $(find ${k} -maxdepth 1 -type f -name '*thermal*' 2>/dev/null)"
    done
    all_thermal="${all_thermal} $(find /system/vendor/etc -maxdepth 1 -type f -name 'powerhint*' 2>/dev/null)"
    all_thermal="${all_thermal} $(find /system/vendor/lib/hw -maxdepth 1 -type f -name 'thermal*' 2>/dev/null)"
    all_thermal="${all_thermal} $(find /system/vendor/lib64/hw -maxdepth 1 -type f -name 'thermal*' 2>/dev/null)"
    for thermal in ${all_thermal}; do
        mkdir -p ${MODPATH}${thermal%/*}
        touch ${MODPATH}${thermal}
    done
    [[ -e /data/vendor/thermal ]] && chattr -i $(find /data/vendor/thermal)
    rm -rf /data/vendor/thermal
    mkdir -p /data/vendor/thermal/config
    chattr +i /data/vendor/thermal/config /data/vendor/thermal
}

set_permissions()
{
    set_perm_recursive  ${MODPATH}  0  0  0755  0777
    set_perm_recursive  /data/adb/turbo-charge  0  0  0755  0777
}

unzip -oj "${ZIPFILE}" module.prop 'common/*' -d ${TMPDIR} >&2
print_modname
check_file
print_info
on_install
set_permissions
