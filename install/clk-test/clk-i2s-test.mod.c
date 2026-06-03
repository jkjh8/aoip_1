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
	{ 0x92997ed8, "_printk" },
	{ 0x76d9b876, "clk_set_rate" },
	{ 0x556e4390, "clk_get_rate" },
	{ 0x2e1ca751, "clk_put" },
	{ 0xa25037d3, "device_match_name" },
	{ 0xa1290020, "platform_bus_type" },
	{ 0x11af9f52, "bus_find_device" },
	{ 0xf5a597e3, "clk_get" },
	{ 0x403c9df6, "put_device" },
	{ 0x81c7ec99, "module_layout" },
};

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "B633C58B05C437C8BBF2453");
