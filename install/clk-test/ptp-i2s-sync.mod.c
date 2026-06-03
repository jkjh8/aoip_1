#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x556e4390, "clk_get_rate" },
	{ 0xe783e261, "sysfs_emit" },
	{ 0x3854774b, "kstrtoll" },
	{ 0xa39bc75a, "mutex_lock" },
	{ 0x76d9b876, "clk_set_rate" },
	{ 0x92997ed8, "_printk" },
	{ 0x2816d909, "mutex_unlock" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0xa25037d3, "device_match_name" },
	{ 0xa1290020, "platform_bus_type" },
	{ 0x11af9f52, "bus_find_device" },
	{ 0xf5a597e3, "clk_get" },
	{ 0x403c9df6, "put_device" },
	{ 0xa8f9227b, "kernel_kobj" },
	{ 0x9dda9f52, "kobject_create_and_add" },
	{ 0x2e1ca751, "clk_put" },
	{ 0x9c416fb2, "sysfs_create_group" },
	{ 0xce38ea7b, "kobject_put" },
	{ 0x46cde804, "sysfs_remove_group" },
	{ 0x81c7ec99, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "4C32A105443F81A04ABCF0D");
