/*
 * clk-i2s-test.c
 *
 * clk_i2s 동적 주파수 변경 가능 여부 테스트.
 * 로드 시 -13 ppm 적용, 언로드 시 원래 주파수 복구.
 *
 * 사용:
 *   sudo insmod clk-i2s-test.ko
 *   dmesg | grep clk-i2s
 *   sudo rmmod clk-i2s-test
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/platform_device.h>

#define I2S_DEV   "1f000a0000.i2s"
#define I2S_CON   "i2sclk"
#define PPM_TEST  (-13)           /* 테스트용 보정값 (ppm) */

static unsigned long original_rate;
static struct clk *i2s_clk;

static int __init clk_i2s_test_init(void)
{
    struct device *dev;
    unsigned long rate, new_rate;
    int ret;

    /* 플랫폼 버스에서 I2S 디바이스 찾기 */
    dev = bus_find_device_by_name(&platform_bus_type, NULL, I2S_DEV);
    if (!dev) {
        pr_err("clk-i2s: device %s not found\n", I2S_DEV);
        return -ENODEV;
    }

    i2s_clk = clk_get(dev, I2S_CON);
    put_device(dev);

    if (IS_ERR(i2s_clk)) {
        pr_err("clk-i2s: clk_get(%s, %s) failed: %ld\n",
               I2S_DEV, I2S_CON, PTR_ERR(i2s_clk));
        i2s_clk = NULL;
        return PTR_ERR(i2s_clk);
    }

    rate = clk_get_rate(i2s_clk);
    original_rate = rate;
    pr_info("clk-i2s: current rate = %lu Hz\n", rate);

    /* ppm 보정: new_rate = rate * (1 + ppm/1e6) */
    new_rate = rate + (long)(rate / 1000000L * PPM_TEST);
    pr_info("clk-i2s: attempting set_rate = %lu Hz (%d ppm)\n",
            new_rate, PPM_TEST);

    ret = clk_set_rate(i2s_clk, new_rate);
    if (ret) {
        pr_err("clk-i2s: clk_set_rate failed: %d\n", ret);
        clk_put(i2s_clk);
        i2s_clk = NULL;
        return ret;
    }

    rate = clk_get_rate(i2s_clk);
    pr_info("clk-i2s: *** SUCCESS *** new rate = %lu Hz (diff = %ld Hz)\n",
            rate, (long)rate - (long)original_rate);
    pr_info("clk-i2s: actual correction = %+ld ppb\n",
            (long)((long)rate - (long)original_rate) * 1000000000L
            / (long)original_rate);

    return 0;
}

static void __exit clk_i2s_test_exit(void)
{
    if (!i2s_clk)
        return;

    pr_info("clk-i2s: restoring rate to %lu Hz\n", original_rate);
    clk_set_rate(i2s_clk, original_rate);
    pr_info("clk-i2s: rate after restore = %lu Hz\n",
            clk_get_rate(i2s_clk));
    clk_put(i2s_clk);
}

module_init(clk_i2s_test_init);
module_exit(clk_i2s_test_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kjh");
MODULE_DESCRIPTION("I2S clock dynamic rate test for RPi5 RP1");
