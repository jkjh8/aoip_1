/*
 * ptp-i2s-sync.c
 *
 * PTP freq 보정값을 I2S 하드웨어 클럭(clk_i2s)에 적용하는 커널 모듈.
 *
 * sysfs 인터페이스:
 *   /sys/kernel/ptp_i2s_sync/freq_ppb   - r/w: 보정값 (ppb, 정수)
 *   /sys/kernel/ptp_i2s_sync/rate_hz    - r:   현재 실제 클럭 주파수 (Hz)
 *
 * 동작:
 *   freq_ppb에 값을 쓰면 clk_set_rate()로 clk_i2s를 즉시 조정.
 *   모듈 언로드 시 원래 주파수로 복구.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/platform_device.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/atomic.h>

#define I2S_DEV     "1f000a0000.i2s"
#define I2S_CON     "i2sclk"
#define MAX_PPB     500000L     /* ±500 ppm 클램핑 */

/* clk_i2s는 부모(pll_audio)에서 정수 divider(87)로 만들어지므로 ppm 단위 미세 조정
 * 불가능. 대신 최상위 audio PLL(pll_audio_core)을 직접 조정하면 divider는 그대로
 * 유지된 채 BCK가 같은 비율로 변함.
 *
 * 단, clk_i2s는 mux clock으로 I2S 유휴 상태에서는 부모가 xosc(54MHz)이고
 * ALSA가 활성화한 후에야 pll_audio로 전환됨. 따라서 모듈 init 시점에는 PLL을
 * 잡을 수 없고, apply_ppb 호출 시 동적으로 walk-up 해야 함.
 * base_rate는 첫 성공적 walk-up에서 캡처. */
static struct clk      *i2s_clk;    /* clk_i2s (mux entry point) */
static struct clk      *pll_clk;    /* pll_audio_core (lazy, NULL until first apply) */
static unsigned long    base_rate;  /* pll_audio_core 의 원래 rate (lazy 캡처) */
static long             current_ppb;
static long             pending_ppb;    /* PLL 미도달 시 보류된 값 */
static bool             have_pending;
static struct kobject  *kobj;
static DEFINE_MUTEX(clk_mutex);
static struct notifier_block i2s_nb;
static struct delayed_work apply_work;

/* ── clk_set_rate 래퍼 ─────────────────────────────────────────────────── */
/* clk_i2s → 부모 → 부모(pll_audio_core) 동적 resolve. I2S가 active일 때만 성공. */
static struct clk *resolve_pll_clk(void)
{
    struct clk *parent, *gp;
    if (pll_clk) return pll_clk;
    parent = clk_get_parent(i2s_clk);
    if (!parent) return NULL;
    gp = clk_get_parent(parent);
    if (!gp) return NULL;
    pll_clk = gp;
    base_rate = clk_get_rate(pll_clk);
    pr_info("ptp-i2s-sync: PLL resolved, base_rate = %lu Hz, i2s = %lu Hz\n",
            base_rate, clk_get_rate(i2s_clk));
    return pll_clk;
}

static int apply_ppb(long ppb)
{
    unsigned long new_rate;
    int ret;
    struct clk *pll;

    if (ppb >  MAX_PPB) ppb =  MAX_PPB;
    if (ppb < -MAX_PPB) ppb = -MAX_PPB;

    pll = resolve_pll_clk();
    if (!pll) {
        pending_ppb  = ppb;
        have_pending = true;
        current_ppb  = ppb;
        pr_info("ptp-i2s-sync: PLL not yet reachable, ppb=%ld pending (will apply on I2S activation)\n", ppb);
        return 0;
    }

    /* new_rate = base_rate(pll) + base_rate * ppb / 1e9 */
    new_rate = (unsigned long)((s64)base_rate
               + div_s64((s64)base_rate * ppb, 1000000000LL));

    ret = clk_set_rate(pll, new_rate);
    if (ret) {
        pr_err("ptp-i2s-sync: clk_set_rate(pll=%lu) failed: %d\n", new_rate, ret);
        return ret;
    }

    current_ppb  = ppb;
    have_pending = false;
    pr_info("ptp-i2s-sync: ppb=%ld → pll=%lu Hz, i2s=%lu Hz\n",
            ppb, clk_get_rate(pll), clk_get_rate(i2s_clk));
    return 0;
}

/* 지연 적용 워커 — 노티파이어 컨텍스트(clk prepare_lock 보유 중) 밖에서 실행되어
 * 안전하게 mutex/clk_set_rate 호출 가능. */
static void apply_pending_work(struct work_struct *w)
{
    long ppb;
    unsigned long new_rate;

    mutex_lock(&clk_mutex);
    if (!have_pending || !resolve_pll_clk()) {
        mutex_unlock(&clk_mutex);
        return;
    }
    ppb = pending_ppb;
    new_rate = (unsigned long)((s64)base_rate
               + div_s64((s64)base_rate * ppb, 1000000000LL));
    if (clk_set_rate(pll_clk, new_rate) == 0) {
        current_ppb  = ppb;
        have_pending = false;
        pr_info("ptp-i2s-sync: deferred ppb=%ld applied → pll=%lu Hz, i2s=%lu Hz\n",
                ppb, clk_get_rate(pll_clk), clk_get_rate(i2s_clk));
    }
    mutex_unlock(&clk_mutex);
}

/* clk_i2s rate-change notifier — I2S 활성/유휴 mux 전환 시 호출됨.
 * 주의: clk core 의 prepare_lock 을 들고 있는 컨텍스트라 여기서 직접 mutex 잡거나
 * clk_set_rate 호출 금지. 보류된 값이 있으면 workqueue 로 지연 적용. */
static int i2s_clk_notify(struct notifier_block *nb, unsigned long event, void *data)
{
    if (event != POST_RATE_CHANGE)
        return NOTIFY_OK;
    if (READ_ONCE(have_pending))
        schedule_delayed_work(&apply_work, msecs_to_jiffies(100));
    return NOTIFY_OK;
}

/* ── sysfs: freq_ppb ───────────────────────────────────────────────────── */
static ssize_t freq_ppb_show(struct kobject *kobj,
                              struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%ld\n", current_ppb);
}

static ssize_t freq_ppb_store(struct kobject *kobj,
                               struct kobj_attribute *attr,
                               const char *buf, size_t count)
{
    long ppb;
    int ret;

    ret = kstrtol(buf, 10, &ppb);
    if (ret) return ret;

    mutex_lock(&clk_mutex);
    ret = apply_ppb(ppb);
    mutex_unlock(&clk_mutex);

    return ret ? ret : count;
}

/* ── sysfs: rate_hz ────────────────────────────────────────────────────── */
static ssize_t rate_hz_show(struct kobject *kobj,
                             struct kobj_attribute *attr, char *buf)
{
    return sysfs_emit(buf, "%lu\n", clk_get_rate(i2s_clk));
}

/* 0664: 커널은 sysfs world-writable(0002) 금지 → 모듈 로드 후 외부에서
 * chgrp 또는 udev 로 aoip 실행 사용자에게 write 권한을 부여해야 함. */
static struct kobj_attribute attr_freq_ppb =
    __ATTR(freq_ppb, 0664, freq_ppb_show, freq_ppb_store);

static struct kobj_attribute attr_rate_hz =
    __ATTR(rate_hz, 0444, rate_hz_show, NULL);

static struct attribute *attrs[] = {
    &attr_freq_ppb.attr,
    &attr_rate_hz.attr,
    NULL,
};

static struct attribute_group attr_group = { .attrs = attrs };

/* ── init / exit ───────────────────────────────────────────────────────── */
static int __init ptp_i2s_sync_init(void)
{
    struct device *dev;
    int ret;

    dev = bus_find_device_by_name(&platform_bus_type, NULL, I2S_DEV);
    if (!dev) {
        pr_err("ptp-i2s-sync: device %s not found\n", I2S_DEV);
        return -ENODEV;
    }

    i2s_clk = clk_get(dev, I2S_CON);
    put_device(dev);

    if (IS_ERR(i2s_clk)) {
        pr_err("ptp-i2s-sync: clk_get failed: %ld\n", PTR_ERR(i2s_clk));
        return PTR_ERR(i2s_clk);
    }

    pll_clk      = NULL;    /* lazy resolve in apply_ppb */
    base_rate    = 0;
    current_ppb  = 0;
    pending_ppb  = 0;
    have_pending = false;

    INIT_DELAYED_WORK(&apply_work, apply_pending_work);

    i2s_nb.notifier_call = i2s_clk_notify;
    ret = clk_notifier_register(i2s_clk, &i2s_nb);
    if (ret) {
        pr_err("ptp-i2s-sync: clk_notifier_register failed: %d\n", ret);
        clk_put(i2s_clk);
        return ret;
    }
    /* I2S가 이미 활성 상태라면 즉시 PLL resolve 시도 */
    resolve_pll_clk();
    pr_info("ptp-i2s-sync: i2s_clk acquired, rate-change notifier registered\n");

    kobj = kobject_create_and_add("ptp_i2s_sync", kernel_kobj);
    if (!kobj) {
        clk_notifier_unregister(i2s_clk, &i2s_nb);
        clk_put(i2s_clk);
        return -ENOMEM;
    }

    ret = sysfs_create_group(kobj, &attr_group);
    if (ret) {
        kobject_put(kobj);
        clk_notifier_unregister(i2s_clk, &i2s_nb);
        clk_put(i2s_clk);
        return ret;
    }

    pr_info("ptp-i2s-sync: ready. sysfs: /sys/kernel/ptp_i2s_sync/\n");
    return 0;
}

static void __exit ptp_i2s_sync_exit(void)
{
    /* 노티파이어/워크 먼저 분리 — 더 이상 새 워크가 큐잉되지 않게 한 뒤 잔여 워크 완료 대기 */
    clk_notifier_unregister(i2s_clk, &i2s_nb);
    cancel_delayed_work_sync(&apply_work);

    mutex_lock(&clk_mutex);
    if (pll_clk && base_rate) {
        pr_info("ptp-i2s-sync: restoring pll_audio_core to %lu Hz\n", base_rate);
        clk_set_rate(pll_clk, base_rate);
    }
    mutex_unlock(&clk_mutex);

    sysfs_remove_group(kobj, &attr_group);
    kobject_put(kobj);
    clk_put(i2s_clk);
    pr_info("ptp-i2s-sync: unloaded\n");
}

module_init(ptp_i2s_sync_init);
module_exit(ptp_i2s_sync_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kjh");
MODULE_DESCRIPTION("PTP-synchronized I2S clock correction for RPi5 RP1");
