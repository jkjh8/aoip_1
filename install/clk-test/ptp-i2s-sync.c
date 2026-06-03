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
#include <linux/platform_device.h>
#include <linux/mutex.h>

#define I2S_DEV     "1f000a0000.i2s"
#define I2S_CON     "i2sclk"
#define MAX_PPB     500000L     /* ±500 ppm 클램핑 */

static struct clk      *i2s_clk;
static unsigned long    base_rate;
static long             current_ppb;
static struct kobject  *kobj;
static DEFINE_MUTEX(clk_mutex);

/* ── clk_set_rate 래퍼 ─────────────────────────────────────────────────── */
static int apply_ppb(long ppb)
{
    unsigned long new_rate;
    int ret;

    if (ppb >  MAX_PPB) ppb =  MAX_PPB;
    if (ppb < -MAX_PPB) ppb = -MAX_PPB;

    /* new_rate = base_rate + base_rate * ppb / 1e9 */
    new_rate = (unsigned long)((s64)base_rate
               + div_s64((s64)base_rate * ppb, 1000000000LL));

    ret = clk_set_rate(i2s_clk, new_rate);
    if (ret) {
        pr_err("ptp-i2s-sync: clk_set_rate(%lu) failed: %d\n", new_rate, ret);
        return ret;
    }

    current_ppb = ppb;
    pr_info("ptp-i2s-sync: ppb=%ld → rate=%lu Hz\n",
            ppb, clk_get_rate(i2s_clk));
    return 0;
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

    base_rate = clk_get_rate(i2s_clk);
    current_ppb = 0;
    pr_info("ptp-i2s-sync: base_rate = %lu Hz\n", base_rate);

    kobj = kobject_create_and_add("ptp_i2s_sync", kernel_kobj);
    if (!kobj) {
        clk_put(i2s_clk);
        return -ENOMEM;
    }

    ret = sysfs_create_group(kobj, &attr_group);
    if (ret) {
        kobject_put(kobj);
        clk_put(i2s_clk);
        return ret;
    }

    pr_info("ptp-i2s-sync: ready. sysfs: /sys/kernel/ptp_i2s_sync/\n");
    return 0;
}

static void __exit ptp_i2s_sync_exit(void)
{
    mutex_lock(&clk_mutex);
    pr_info("ptp-i2s-sync: restoring base_rate %lu Hz\n", base_rate);
    clk_set_rate(i2s_clk, base_rate);
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
