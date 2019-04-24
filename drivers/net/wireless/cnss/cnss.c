/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/export.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/pm_wakeup.h>
#include <linux/sched.h>
#include <linux/pm_qos.h>
#include <linux/esoc_client.h>
#include <linux/msm-bus.h>
#include <linux/msm-bus-board.h>
#include <mach/gpiomux.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/rwsem.h>
#include <mach/msm_pcie.h>
#include <linux/crypto.h>
#include <linux/scatterlist.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/log2.h>
#ifdef CONFIG_PCI_MSM
#include <linux/msm_pcie.h>
#else
#include <mach/msm_pcie.h>
#endif
#include <soc/qcom/subsystem_restart.h>
#include <soc/qcom/subsystem_notif.h>
#include <soc/qcom/ramdump.h>
#include <soc/qcom/memory_dump.h>
#include <net/cnss.h>

#define subsys_to_drv(d) container_of(d, struct cnss_data, subsys_desc)

#define VREG_ON			1
#define VREG_OFF		0
#define WLAN_EN_HIGH		1
#define WLAN_EN_LOW		0
#define PCIE_LINK_UP		1
#define PCIE_LINK_DOWN		0

#define QCA6174_VENDOR_ID	(0x168C)
#define QCA6174_DEVICE_ID	(0x003E)
#define QCA6174_REV_ID_OFFSET	(0x08)
#define QCA6174_FW_1_1	(0x11)
#define QCA6174_FW_1_3	(0x13)
#define QCA6174_FW_2_0	(0x20)
#define QCA6174_FW_3_0	(0x30)
#define AR6320_REV1_VERSION             0x5000000
#define AR6320_REV1_1_VERSION           0x5000001
#define AR6320_REV1_3_VERSION           0x5000003
#define AR6320_REV2_1_VERSION           0x5010000
#define AR6320_REV3_VERSION             0x5020000
static struct cnss_fw_files FW_FILES_QCA6174_FW_1_1 = {
"qwlan11.bin", "bdwlan11.bin", "otp11.bin", "utf11.bin",
"utfbd11.bin", "epping11.bin", "evicted11.bin"};
static struct cnss_fw_files FW_FILES_QCA6174_FW_2_0 = {
"qwlan20.bin", "bdwlan20.bin", "otp20.bin", "utf20.bin",
"utfbd20.bin", "epping20.bin", "evicted20.bin"};
static struct cnss_fw_files FW_FILES_QCA6174_FW_1_3 = {
"qwlan13.bin", "bdwlan13.bin", "otp13.bin", "utf13.bin",
"utfbd13.bin", "epping13.bin", "evicted13.bin"};
static struct cnss_fw_files FW_FILES_QCA6174_FW_3_0 = {
"qwlan30.bin", "bdwlan30.bin", "otp30.bin", "utf30.bin",
"utfbd30.bin", "epping30.bin", "evicted30.bin"};
static struct cnss_fw_files FW_FILES_DEFAULT = {
"qwlan.bin", "bdwlan.bin", "otp.bin", "utf.bin",
"utfbd.bin", "epping.bin", "evicted.bin"};

#define WLAN_VREG_NAME		"vdd-wlan"
#define WLAN_SWREG_NAME		"wlan-soc-swreg"
#define WLAN_EN_GPIO_NAME	"wlan-en-gpio"
#define PM_OPTIONS		0
#define PM_OPTIONS_SUSPEND_LINK_DOWN \
	(MSM_PCIE_CONFIG_NO_CFG_RESTORE | MSM_PCIE_CONFIG_LINKDOWN)
#define PM_OPTIONS_RESUME_LINK_DOWN \
	(MSM_PCIE_CONFIG_NO_CFG_RESTORE)

#define SOC_SWREG_VOLT_MAX	1187500
#define SOC_SWREG_VOLT_MIN	1187500

#define POWER_ON_DELAY		2000
#define WLAN_ENABLE_DELAY	10000
#define WLAN_RECOVERY_DELAY	1000
#define PCIE_ENABLE_DELAY	100000
#define EVICT_BIN_MAX_SIZE      (512*1024)
#define CNSS_PINCTRL_STATE_ACTIVE "default"

static DEFINE_SPINLOCK(pci_link_down_lock);

struct cnss_wlan_gpio_info {
	char *name;
	u32 num;
	bool state;
	bool init;
	bool prop;
};

struct cnss_wlan_vreg_info {
	struct regulator *wlan_reg;
	struct regulator *soc_swreg;
	bool state;
};

/* device_info is expected to be fully populated after cnss_config is invoked.
 * The function pointer callbacks are expected to be non null as well.
 */
static struct cnss_data {
	struct platform_device *pldev;
	struct subsys_device *subsys;
	struct subsys_desc    subsysdesc;
	bool ramdump_dynamic;
	struct ramdump_device *ramdump_dev;
	unsigned long ramdump_size;
	void *ramdump_addr;
	phys_addr_t ramdump_phys;
	u16 unsafe_ch_count;
	u16 unsafe_ch_list[CNSS_MAX_CH_NUM];
	struct cnss_wlan_driver *driver;
	struct pci_dev *pdev;
	const struct pci_device_id *id;
	struct cnss_wlan_vreg_info vreg_info;
	struct cnss_wlan_gpio_info gpio_info;
	bool pcie_link_state;
	bool pcie_link_down_ind;
	bool pci_register_again;
	struct pci_saved_state *saved_state;
	u16 revision_id;
	u16 dfs_nol_info_len;
	bool recovery_in_progress;
	bool fw_available;
	struct codeswap_codeseg_info *cnss_seg_info;
	/* Virtual Address of the DMA page */
	void *codeseg_cpuaddr[CODESWAP_MAX_CODESEGS];
	struct cnss_fw_files fw_files;
	struct pm_qos_request qos_request;
	void *modem_notify_handler;
	int modem_current_status;
	struct msm_bus_scale_pdata *bus_scale_table;
	uint32_t bus_client;
	void *subsys_handle;
	struct esoc_desc *esoc_desc;
	struct cnss_platform_cap cap;
	bool notify_modem_status;
	struct msm_pcie_register_event event_reg;
	struct wakeup_source ws;
	uint32_t recovery_count;
	enum cnss_driver_status driver_status;
#ifdef CONFIG_CNSS_SECURE_FW
	void *fw_mem;
#endif
	void *dfs_nol_info;
} *penv;

extern unsigned int system_rev;

static int cnss_wlan_vreg_on(struct cnss_wlan_vreg_info *vreg_info)
{
	int ret;

	ret = regulator_enable(vreg_info->wlan_reg);
	if (ret) {
		pr_err("%s: regulator enable failed for WLAN power\n",
				__func__);
		goto error_enable;
	}

	if (vreg_info->soc_swreg) {
		ret = regulator_enable(vreg_info->soc_swreg);
		if (ret) {
			pr_err("%s: regulator enable failed for external soc-swreg\n",
					__func__);
			goto error_enable2;
		}
	}
	return ret;

error_enable2:
	regulator_disable(vreg_info->wlan_reg);
error_enable:
	return ret;
}

static int cnss_wlan_vreg_off(struct cnss_wlan_vreg_info *vreg_info)
{
	int ret, ret2;

	if (vreg_info->soc_swreg) {
		ret = regulator_disable(vreg_info->soc_swreg);
		if (ret) {
			pr_err("%s: regulator disable failed for external soc-swreg\n",
					__func__);
			goto error_disable;
		}
	}
	ret = regulator_disable(vreg_info->wlan_reg);
	if (ret) {
		pr_err("%s: regulator disable failed for WLAN power\n",
				__func__);
		goto error_disable2;
	}
	return ret;

error_disable2:
	if (vreg_info->soc_swreg) {
		ret2 = regulator_enable(vreg_info->soc_swreg);
		if (ret2)
			ret = ret2;
	}
error_disable:
	return ret;
}

static int cnss_wlan_vreg_set(struct cnss_wlan_vreg_info *vreg_info, bool state)
{
	int ret = 0;

	if (vreg_info->state == state) {
		pr_debug("Already wlan vreg state is %s\n",
				state ? "enabled" : "disabled");
		goto out;
	}

	if (state)
		ret = cnss_wlan_vreg_on(vreg_info);
	else
		ret = cnss_wlan_vreg_off(vreg_info);

	if (ret)
		goto out;

	pr_debug("%s: wlan vreg is now %s\n", __func__,
			state ? "enabled" : "disabled");
	vreg_info->state = state;

out:
	return ret;
}

static int cnss_wlan_gpio_init(struct cnss_wlan_gpio_info *info)
{
	int ret = 0;

#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
	if(system_rev > 3)
		return ret;
#endif

	ret = gpio_request(info->num, info->name);

	if (ret) {
		pr_err("can't get gpio %s ret %d\n", info->name, ret);
		goto err_gpio_req;
	}

	ret = gpio_direction_output(info->num, info->init);

	if (ret) {
		pr_err("can't set gpio direction %s ret %d\n", info->name, ret);
		goto err_gpio_dir;
	}
	info->state = info->init;

	return ret;

err_gpio_dir:
	gpio_free(info->num);

err_gpio_req:

	return ret;
}
#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
extern void msm_pcie_wlan_en(bool onoff);
#endif
static void cnss_wlan_gpio_set(struct cnss_wlan_gpio_info *info, bool state)
{
	if (!info->prop)
		return;

#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
	if(system_rev > 3) {
		info->state = gpio_get_value(info->num);
	}
#endif

	if (info->state == state) {
		pr_debug("Already %s gpio is %s\n",
			 info->name, state ? "high" : "low");
		return;
	}

#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
	if(system_rev > 3) {
		msm_pcie_wlan_en(state);
	} else {
		gpio_set_value(info->num, state);
	}
#else
	gpio_set_value(info->num, state);
#endif
	info->state = state;

	pr_debug("%s: %s gpio is now %s\n", __func__,
		 info->name, info->state ? "enabled" : "disabled");
}

static int cnss_wlan_get_resources(struct platform_device *pdev)
{
	int ret = 0;
	struct cnss_wlan_gpio_info *gpio_info = &penv->gpio_info;
	struct cnss_wlan_vreg_info *vreg_info = &penv->vreg_info;

	vreg_info->wlan_reg = regulator_get(&pdev->dev, WLAN_VREG_NAME);

	if (IS_ERR(vreg_info->wlan_reg)) {
		if (PTR_ERR(vreg_info->wlan_reg) == -EPROBE_DEFER)
			pr_err("%s: vreg probe defer\n", __func__);
		else
			pr_err("%s: vreg regulator get failed\n", __func__);
		ret = PTR_ERR(vreg_info->wlan_reg);
		goto err_reg_get;
	}

	ret = regulator_enable(vreg_info->wlan_reg);

	if (ret) {
		pr_err("%s: vreg initial vote failed\n", __func__);
		goto err_reg_enable;
	}

	pr_err("%s: system_rev=%d\n", __func__, system_rev);
	if (of_get_property(pdev->dev.of_node, WLAN_SWREG_NAME"-supply", NULL)
#ifdef CONFIG_SEC_LENTIS_PROJECT
		&& (system_rev > 6)
#endif
#ifdef CONFIG_SEC_TRLTE_PROJECT
		&& (system_rev > 3)
#endif
	) {
		vreg_info->soc_swreg = regulator_get(&pdev->dev,
			WLAN_SWREG_NAME);
		if (IS_ERR(vreg_info->soc_swreg)) {
			pr_err("%s: soc-swreg node not found\n",
					__func__);
			goto err_reg_get2;
		}
#ifdef CONFIG_SEC_LENTIS_PROJECT
		if(system_rev == 7)
			ret = regulator_set_voltage(vreg_info->soc_swreg,
					1200000, 1200000);
		else if(system_rev < 7)
			ret = regulator_set_voltage(vreg_info->soc_swreg,
					1150000, 1150000);
		else
			ret = regulator_set_voltage(vreg_info->soc_swreg,
					SOC_SWREG_VOLT_MIN, SOC_SWREG_VOLT_MAX);
#else
		ret = regulator_set_voltage(vreg_info->soc_swreg,
					1150000, 1150000);
#endif
		if (ret) {
			pr_err("%s: vreg initial voltage set failed on soc-swreg\n",
					__func__);
			goto err_reg_set;
		}
		ret = regulator_enable(vreg_info->soc_swreg);
		if (ret) {
			pr_err("%s: vreg initial vote failed\n", __func__);
			goto err_reg_enable2;
		}
		penv->cap.cap_flag |= CNSS_HAS_EXTERNAL_SWREG;
	}

	vreg_info->state = VREG_ON;

	if (!of_find_property((&pdev->dev)->of_node, gpio_info->name, NULL)) {
		gpio_info->prop = false;
		goto end;
	}

	gpio_info->prop = true;

#ifdef USE_GPIO_EXPANDER_WLEN_EN
#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
	if(system_rev < 4)
		ret = of_property_read_u32((&pdev->dev)->of_node,
					gpio_info->name, &gpio_info->num);
	else
		ret = 0;
#endif
#else
	ret = of_get_named_gpio((&pdev->dev)->of_node,
				gpio_info->name, 0);
#endif

	if (ret >= 0) {
#ifdef USE_GPIO_EXPANDER_WLEN_EN
#if defined (CONFIG_SEC_TRLTE_PROJECT) || defined(CONFIG_SEC_TBLTE_PROJECT)
	if(system_rev < 4)
		pr_err("From now, We will use GPIO expander port num=%d\n", gpio_info->num);
#endif
#else
		gpio_info->num = ret;
#endif
		ret = 0;
	} else {
		if (ret == -EPROBE_DEFER)
			pr_debug("get WLAN_EN GPIO probe defer\n");
		else
			pr_err("can't get gpio %s ret %d",
			       gpio_info->name, ret);
		goto err_get_gpio;
	}

	ret = cnss_wlan_gpio_init(gpio_info);

	if (ret) {
		pr_err("gpio init failed\n");
		goto err_gpio_init;
	}

end:
	return ret;

err_gpio_init:
err_get_gpio:
	if (vreg_info->soc_swreg)
		regulator_disable(vreg_info->soc_swreg);
	vreg_info->state = VREG_OFF;

err_reg_enable2:
err_reg_set:
	if (vreg_info->soc_swreg)
		regulator_put(vreg_info->soc_swreg);

err_reg_get2:
	regulator_disable(vreg_info->wlan_reg);
err_reg_enable:
	regulator_put(vreg_info->wlan_reg);

err_reg_get:
	return ret;
}

static void cnss_wlan_release_resources(void)
{
	struct cnss_wlan_gpio_info *gpio_info = &penv->gpio_info;
	struct cnss_wlan_vreg_info *vreg_info = &penv->vreg_info;

	gpio_free(gpio_info->num);
	cnss_wlan_vreg_set(vreg_info, VREG_OFF);
	regulator_put(vreg_info->wlan_reg);
	if (vreg_info->soc_swreg)
		regulator_put(vreg_info->soc_swreg);
	gpio_info->state = WLAN_EN_LOW;
	gpio_info->prop = false;
	vreg_info->state = VREG_OFF;
}

static u8 cnss_get_pci_dev_bus_number(struct pci_dev *pdev)
{
	return pdev->bus->number;
}

void cnss_setup_fw_files(u16 revision)
{
	switch (revision) {

	case QCA6174_FW_1_1:
		strlcpy(penv->fw_files.image_file, "qwlan11.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.board_data, "bdwlan11.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.otp_data, "otp11.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_file, "utf11.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_board_data, "utfbd11.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.epping_file, "epping11.bin",
			CNSS_MAX_FILE_NAME);
		break;

	case QCA6174_FW_1_3:
		strlcpy(penv->fw_files.image_file, "qwlan13.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.board_data, "bdwlan13.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.otp_data, "otp13.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_file, "utf13.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_board_data, "utfbd13.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.epping_file, "epping13.bin",
			CNSS_MAX_FILE_NAME);
		break;

	case QCA6174_FW_2_0:
		strlcpy(penv->fw_files.image_file, "qwlan20.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.board_data, "bdwlan20.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.otp_data, "otp20.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_file, "utf20.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_board_data, "utfbd20.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.epping_file, "epping20.bin",
			CNSS_MAX_FILE_NAME);
		break;

	case QCA6174_FW_3_0:
		strlcpy(penv->fw_files.image_file, "qwlan30.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.board_data, "bdwlan30.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.otp_data, "otp30.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_file, "utf30.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_board_data, "utfbd30.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.epping_file, "epping30.bin",
			CNSS_MAX_FILE_NAME);
		break;

	default:
		strlcpy(penv->fw_files.image_file, "qwlan.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.board_data, "bdwlan.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.otp_data, "otp.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_file, "utf.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.utf_board_data, "utfbd.bin",
			CNSS_MAX_FILE_NAME);
		strlcpy(penv->fw_files.epping_file, "epping.bin",
			CNSS_MAX_FILE_NAME);
		break;
	}
}

int cnss_get_fw_files(struct cnss_fw_files *pfw_files)
{
	if (!penv || !pfw_files)
		return -ENODEV;

	*pfw_files = penv->fw_files;

	return 0;
}
EXPORT_SYMBOL(cnss_get_fw_files);

#ifdef CONFIG_CNSS_SECURE_FW
static void cnss_wlan_fw_mem_alloc(struct pci_dev *pdev)
{
	penv->fw_mem = devm_kzalloc(&pdev->dev, MAX_FIRMWARE_SIZE, GFP_KERNEL);

	if (!penv->fw_mem)
		pr_debug("Memory not available for Secure FW\n");
}
#else
static void cnss_wlan_fw_mem_alloc(struct pci_dev *pdev)
{
}
#endif
int cnss_get_fw_files_for_target(struct cnss_fw_files *pfw_files,
					u32 target_type, u32 target_version)
{
	if (!pfw_files)
		return -ENODEV;

	switch (target_version) {
	case AR6320_REV1_VERSION:
	case AR6320_REV1_1_VERSION:
		memcpy(pfw_files, &FW_FILES_QCA6174_FW_1_1, sizeof(*pfw_files));
		break;
	case AR6320_REV1_3_VERSION:
		memcpy(pfw_files, &FW_FILES_QCA6174_FW_1_3, sizeof(*pfw_files));
		break;
	case AR6320_REV2_1_VERSION:
		memcpy(pfw_files, &FW_FILES_QCA6174_FW_2_0, sizeof(*pfw_files));
		break;
	case AR6320_REV3_VERSION:
		memcpy(pfw_files, &FW_FILES_QCA6174_FW_3_0, sizeof(*pfw_files));
		break;
	default:
		memcpy(pfw_files, &FW_FILES_DEFAULT, sizeof(*pfw_files));
		pr_err("%s version mismatch 0x%X 0x%X",
				__func__, target_type, target_version);
		break;
	}
	return 0;
}
EXPORT_SYMBOL(cnss_get_fw_files_for_target);

static int cnss_wlan_pci_probe(struct pci_dev *pdev,
			       const struct pci_device_id *id)
{
	int ret = 0;
	struct cnss_wlan_vreg_info *vreg_info = &penv->vreg_info;
	struct cnss_wlan_gpio_info *gpio_info = &penv->gpio_info;
	void *cpu_addr;
	dma_addr_t dma_handle;
	struct codeswap_codeseg_info *cnss_seg_info = NULL;
	struct device *dev = &pdev->dev;

	penv->pdev = pdev;
	penv->id = id;
	penv->fw_available = false;

	if (penv->pci_register_again) {
		pr_debug("%s: PCI re-registration complete\n", __func__);
		penv->pci_register_again = false;
		return 0;
	}

	pci_read_config_word(pdev, QCA6174_REV_ID_OFFSET, &penv->revision_id);
	cnss_setup_fw_files(penv->revision_id);

	if (penv->pcie_link_state) {
		pci_save_state(pdev);
		penv->saved_state = pci_store_saved_state(pdev);

		ret = msm_pcie_pm_control(MSM_PCIE_SUSPEND,
					  cnss_get_pci_dev_bus_number(pdev),
					  NULL, NULL, PM_OPTIONS);
		if (ret) {
			pr_err("Failed to shutdown PCIe link\n");
			goto err_pcie_suspend;
		}
		penv->pcie_link_state = PCIE_LINK_DOWN;
	}

	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);
	ret = cnss_wlan_vreg_set(vreg_info, VREG_OFF);

	if (ret) {
		pr_err("can't turn off wlan vreg\n");
		goto err_pcie_suspend;
	}

	cnss_wlan_fw_mem_alloc(pdev);

	if (penv->revision_id != QCA6174_FW_3_0) {
		pr_debug("Supported Target Revision:%d\n", penv->revision_id);
		goto err_pcie_suspend;
	}

	cpu_addr = dma_alloc_coherent(dev, EVICT_BIN_MAX_SIZE,
					&dma_handle, GFP_KERNEL);
	if (!cpu_addr || !dma_handle) {
		pr_err("cnss: Memory Alloc failed for codeswap feature\n");
		goto err_pcie_suspend;
	}

	cnss_seg_info = devm_kzalloc(dev, sizeof(*cnss_seg_info),
							GFP_KERNEL);
	if (!cnss_seg_info) {
		pr_err("Fail to allocate memory for cnss_seg_info\n");
		goto end_dma_alloc;
	}

	memset(cnss_seg_info, 0, sizeof(*cnss_seg_info));
	cnss_seg_info->codeseg_busaddr[0]   = (void *)dma_handle;
	penv->codeseg_cpuaddr[0]            = cpu_addr;
	cnss_seg_info->codeseg_size         = EVICT_BIN_MAX_SIZE;
	cnss_seg_info->codeseg_total_bytes  = EVICT_BIN_MAX_SIZE;
	cnss_seg_info->num_codesegs         = 1;
	cnss_seg_info->codeseg_size_log2    = ilog2(EVICT_BIN_MAX_SIZE);

	penv->cnss_seg_info = cnss_seg_info;
	pr_debug("%s: Successfully allocated memory for CODESWAP\n", __func__);

	return ret;

end_dma_alloc:
	dma_free_coherent(dev, EVICT_BIN_MAX_SIZE, cpu_addr, dma_handle);
err_pcie_suspend:
	return ret;
}

static void cnss_wlan_pci_remove(struct pci_dev *pdev)
{

}

static int cnss_wlan_pci_suspend(struct pci_dev *pdev, pm_message_t state)
{
	int ret = 0;
	struct cnss_wlan_driver *wdriver;

	if (!penv)
		goto out;

	wdriver = penv->driver;
	if (!wdriver)
		goto out;

	if (wdriver->suspend) {
		ret = wdriver->suspend(pdev, state);

		if (penv->pcie_link_state) {
			pci_save_state(pdev);
			penv->saved_state = pci_store_saved_state(pdev);
		}
	}

out:
	return ret;
}

static int cnss_wlan_pci_resume(struct pci_dev *pdev)
{
	int ret = 0;
	struct cnss_wlan_driver *wdriver;

	if (!penv)
		goto out;

	wdriver = penv->driver;
	if (!wdriver)
		goto out;

	if (wdriver->resume && !penv->pcie_link_down_ind) {
		if (penv->saved_state)
			pci_load_and_free_saved_state(pdev,
				&penv->saved_state);
		pci_restore_state(pdev);

		ret = wdriver->resume(pdev);
	}

out:
	return ret;
}

static DECLARE_RWSEM(cnss_pm_sem);

static int cnss_pm_notify(struct notifier_block *b,
			unsigned long event, void *p)
{
	switch (event) {
	case PM_SUSPEND_PREPARE:
		down_write(&cnss_pm_sem);
		break;

	case PM_POST_SUSPEND:
		up_write(&cnss_pm_sem);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block cnss_pm_notifier = {
	.notifier_call = cnss_pm_notify,
};

static DEFINE_PCI_DEVICE_TABLE(cnss_wlan_pci_id_table) = {
	{ QCA6174_VENDOR_ID, QCA6174_DEVICE_ID, PCI_ANY_ID, PCI_ANY_ID },
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, cnss_wlan_pci_id_table);

struct pci_driver cnss_wlan_pci_driver = {
	.name     = "cnss_wlan_pci",
	.id_table = cnss_wlan_pci_id_table,
	.probe    = cnss_wlan_pci_probe,
	.remove   = cnss_wlan_pci_remove,
	.suspend  = cnss_wlan_pci_suspend,
	.resume   = cnss_wlan_pci_resume,
};

void recovery_work_handler(struct work_struct *recovery)
{
	cnss_device_self_recovery();
}

DECLARE_WORK(recovery_work, recovery_work_handler);

void cnss_schedule_recovery_work(void)
{
	schedule_work(&recovery_work);
}
EXPORT_SYMBOL(cnss_schedule_recovery_work);

void cnss_pci_link_down_cb(struct msm_pcie_notify *notify)
{
	unsigned long flags;

	spin_lock_irqsave(&pci_link_down_lock, flags);
	if (penv->pcie_link_down_ind) {
		pr_debug("PCI link down recovery is in progress, ignore!\n");
		spin_unlock_irqrestore(&pci_link_down_lock, flags);
		return;
	}
	penv->pcie_link_down_ind = true;
	spin_unlock_irqrestore(&pci_link_down_lock, flags);

	pr_err("PCI link down, schedule recovery\n");
	schedule_work(&recovery_work);
}

void cnss_wlan_pci_link_down(void)
{
	unsigned long flags;

	spin_lock_irqsave(&pci_link_down_lock, flags);
	if (penv->pcie_link_down_ind) {
		pr_debug("PCI link down recovery is in progress, ignore!\n");
		spin_unlock_irqrestore(&pci_link_down_lock, flags);
		return;
	}
	penv->pcie_link_down_ind = true;
	spin_unlock_irqrestore(&pci_link_down_lock, flags);

	pr_err("PCI link down detected by host driver, schedule recovery!\n");
	schedule_work(&recovery_work);
}
EXPORT_SYMBOL(cnss_wlan_pci_link_down);

int cnss_pcie_shadow_control(struct pci_dev *dev, bool enable)
{
	return msm_pcie_shadow_control(dev, enable);
}
EXPORT_SYMBOL(cnss_pcie_shadow_control);

int cnss_get_codeswap_struct(struct codeswap_codeseg_info *swap_seg)
{
	struct codeswap_codeseg_info *cnss_seg_info = penv->cnss_seg_info;

	if (!cnss_seg_info) {
		swap_seg = NULL;
		return -ENOENT;
	}
	if (!penv->fw_available) {
		pr_err("%s: fw is not availabe\n", __func__);
		return -ENOENT;
	}

	*swap_seg = *cnss_seg_info;

	return 0;
}
EXPORT_SYMBOL(cnss_get_codeswap_struct);

static void cnss_wlan_memory_expansion(void)
{
	struct device *dev;
	const struct firmware *fw_entry;
	const char *filename = FW_FILES_QCA6174_FW_3_0.evicted_data;
	u_int32_t fw_entry_size, size_left, dma_size_left, length;
	char *fw_temp;
	char *fw_data;
	char *dma_virt_addr;
	struct codeswap_codeseg_info *cnss_seg_info;
	u_int32_t total_length = 0;
	struct pci_dev *pdev;

	pdev = penv->pdev;
	dev = &pdev->dev;
	cnss_seg_info = penv->cnss_seg_info;

	if (!cnss_seg_info) {
		pr_err("cnss: cnss_seg_info is NULL\n");
		goto end;
	}

	if (penv->fw_available) {
		pr_debug("cnss: fw code already copied to host memory\n");
		goto end;
	}

	if (request_firmware(&fw_entry, filename, dev) != 0) {
		pr_err("cnss:failed to get fw: %s\n", filename);
		goto end;
	}

	if (!fw_entry || !fw_entry->data) {
		pr_err("%s: INVALID FW entries\n", __func__);
		goto release_fw;
	}

	dma_virt_addr = (char *)penv->codeseg_cpuaddr[0];
	fw_data = (u8 *) fw_entry->data;
	fw_temp = fw_data;
	fw_entry_size = fw_entry->size;
	if (fw_entry_size > EVICT_BIN_MAX_SIZE)
		fw_entry_size = EVICT_BIN_MAX_SIZE;
	size_left = fw_entry_size;
	dma_size_left = EVICT_BIN_MAX_SIZE;
	while ((size_left && fw_temp) && (dma_size_left > 0)) {
		fw_temp = fw_temp + 4;
		size_left = size_left - 4;
		length = *(int *)fw_temp;
		if ((length > size_left || length <= 0) ||
			(dma_size_left <= 0 || length > dma_size_left)) {
			pr_err("cnss: wrong length read:%d\n",
							length);
			break;
		}
		fw_temp = fw_temp + 4;
		size_left = size_left - 4;
		memcpy(dma_virt_addr, fw_temp, length);
		dma_size_left = dma_size_left - length;
		size_left = size_left - length;
		fw_temp = fw_temp + length;
		dma_virt_addr = dma_virt_addr + length;
		total_length += length;
		pr_debug("cnss: bytes_left to copy: fw:%d; dma_page:%d\n",
						size_left, dma_size_left);
	}
	pr_debug("cnss: total_bytes copied: %d\n", total_length);
	cnss_seg_info->codeseg_total_bytes = total_length;
	penv->fw_available = 1;

release_fw:
	release_firmware(fw_entry);
end:
	return;
}

int cnss_wlan_register_driver(struct cnss_wlan_driver *driver)
{
	int ret = 0;
	int probe_again = 0;
	struct cnss_wlan_driver *wdrv;
	struct cnss_wlan_vreg_info *vreg_info;
	struct cnss_wlan_gpio_info *gpio_info;
	struct pci_dev *pdev;

	if (!penv)
		return -ENODEV;

	wdrv = penv->driver;
	vreg_info = &penv->vreg_info;
	gpio_info = &penv->gpio_info;
	pdev = penv->pdev;

	if (!wdrv) {
		penv->driver = wdrv = driver;
	} else {
		pr_err("driver already registered\n");
		return -EEXIST;
	}

again:
	ret = cnss_wlan_vreg_set(vreg_info, VREG_ON);
	if (ret) {
		pr_err("wlan vreg ON failed\n");
		goto err_wlan_vreg_on;
	}

	usleep(POWER_ON_DELAY);

	cnss_wlan_gpio_set(gpio_info, WLAN_EN_HIGH);
	usleep(WLAN_ENABLE_DELAY);

	if (!pdev) {
		pr_debug("%s: invalid pdev. register pci device\n", __func__);
		ret = pci_register_driver(&cnss_wlan_pci_driver);

		if (ret) {
			pr_err("%s: pci registration failed\n", __func__);
			goto err_pcie_link_up;
		}
		pdev = penv->pdev;
		if (!pdev) {
			pr_err("%s: pdev is still invalid\n", __func__);
			goto err_pcie_link_up;
		}
	}

	if (!penv->pcie_link_state && !penv->pcie_link_down_ind) {
		ret = msm_pcie_pm_control(MSM_PCIE_RESUME,
					  cnss_get_pci_dev_bus_number(pdev),
					  NULL, NULL, PM_OPTIONS);
		if (ret) {
			pr_err("PCIe link bring-up failed\n");
			goto err_pcie_link_up;
		}
		penv->pcie_link_state = PCIE_LINK_UP;
	} else if (!penv->pcie_link_state && penv->pcie_link_down_ind) {

		ret = msm_pcie_pm_control(MSM_PCIE_RESUME,
				cnss_get_pci_dev_bus_number(pdev),
				NULL, NULL, PM_OPTIONS_RESUME_LINK_DOWN);

		if (ret) {
			pr_err("PCIe link bring-up failed (link down option)\n");
			goto err_pcie_link_up;
		}
		ret = msm_pcie_recover_config(pdev);
		if (ret) {
			pr_err("cnss: PCI link failed to recover\n");
			goto err_pcie_recover;
		}
		penv->pcie_link_down_ind = false;
	}
	penv->pcie_link_state = PCIE_LINK_UP;

	if (penv->revision_id == QCA6174_FW_3_0)
		cnss_wlan_memory_expansion();

	if (wdrv->probe) {
		if (penv->saved_state)
			pci_load_and_free_saved_state(pdev, &penv->saved_state);

		pci_restore_state(pdev);

		ret = wdrv->probe(pdev, penv->id);
		if (ret) {
			if (probe_again > 3) {
				pr_err("Failed to probe WLAN\n");
				goto err_wlan_probe;
			}
			pci_save_state(pdev);
			penv->saved_state = pci_store_saved_state(pdev);
			msm_pcie_pm_control(MSM_PCIE_SUSPEND,
					    cnss_get_pci_dev_bus_number(pdev),
					    NULL, NULL, PM_OPTIONS);
			penv->pcie_link_state = PCIE_LINK_DOWN;
			cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);
			usleep(WLAN_ENABLE_DELAY);
			cnss_wlan_vreg_set(vreg_info, VREG_OFF);
			usleep(POWER_ON_DELAY);
			probe_again++;
			goto again;
		}
	}

	penv->event_reg.events = MSM_PCIE_EVENT_LINKDOWN;
	penv->event_reg.user = pdev;
	penv->event_reg.mode = MSM_PCIE_TRIGGER_CALLBACK;
	penv->event_reg.callback = cnss_pci_link_down_cb;
	penv->event_reg.options = MSM_PCIE_CONFIG_NO_RECOVERY;
	ret = msm_pcie_register_event(&penv->event_reg);
	if (ret) {
		pr_err("%s: PCI link down detect register failed %d\n",
				__func__, ret);
		ret = 0;
	}

	if (penv->notify_modem_status && wdrv->modem_status)
		wdrv->modem_status(pdev, penv->modem_current_status);

	return ret;

err_wlan_probe:
	pci_save_state(pdev);
	penv->saved_state = pci_store_saved_state(pdev);
err_pcie_recover:
	msm_pcie_pm_control(MSM_PCIE_SUSPEND,
			    cnss_get_pci_dev_bus_number(pdev),
			    NULL, NULL, PM_OPTIONS);
	penv->pcie_link_state = PCIE_LINK_DOWN;

err_pcie_link_up:
	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);
	cnss_wlan_vreg_set(vreg_info, VREG_OFF);
	if (pdev) {
		pr_err("%d: Unregistering PCI device\n", __LINE__);
		pci_unregister_driver(&cnss_wlan_pci_driver);
		penv->pdev = NULL;
		penv->pci_register_again = true;
	}

err_wlan_vreg_on:
	penv->driver = NULL;

	return ret;
}
EXPORT_SYMBOL(cnss_wlan_register_driver);

void cnss_wlan_unregister_driver(struct cnss_wlan_driver *driver)
{
	struct cnss_wlan_driver *wdrv;
	struct cnss_wlan_vreg_info *vreg_info;
	struct cnss_wlan_gpio_info *gpio_info;
	struct pci_dev *pdev;

	if (!penv)
		return;

	wdrv = penv->driver;
	vreg_info = &penv->vreg_info;
	gpio_info = &penv->gpio_info;
	pdev = penv->pdev;

	if (!wdrv) {
		pr_err("driver not registered\n");
		return;
	}

	msm_bus_scale_client_update_request(penv->bus_client, 0);

	if (!pdev) {
		pr_err("%d: invalid pdev\n", __LINE__);
		goto cut_power;
	}

	if (wdrv->remove)
		wdrv->remove(pdev);

	if (penv->pcie_link_state && !penv->pcie_link_down_ind) {
		pci_save_state(pdev);
		penv->saved_state = pci_store_saved_state(pdev);

		if (msm_pcie_pm_control(MSM_PCIE_SUSPEND,
					cnss_get_pci_dev_bus_number(pdev),
					NULL, NULL, PM_OPTIONS)) {
			pr_err("Failed to shutdown PCIe link\n");
			return;
		}
	} else if (penv->pcie_link_state && penv->pcie_link_down_ind) {
		penv->saved_state = NULL;

		if (msm_pcie_pm_control(MSM_PCIE_SUSPEND,
				cnss_get_pci_dev_bus_number(pdev),
				NULL, NULL, PM_OPTIONS_SUSPEND_LINK_DOWN)) {
			pr_err("Failed to shutdown PCIe link (with linkdown option)\n");
			return;
		}
	}
	penv->pcie_link_state = PCIE_LINK_DOWN;
	penv->driver_status = CNSS_UNINITIALIZED;

	msm_pcie_deregister_event(&penv->event_reg);

cut_power:
	penv->driver = NULL;

	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);

	if (cnss_wlan_vreg_set(vreg_info, VREG_OFF))
		pr_err("wlan vreg OFF failed\n");
}
EXPORT_SYMBOL(cnss_wlan_unregister_driver);

int cnss_set_wlan_unsafe_channel(u16 *unsafe_ch_list, u16 ch_count)
{
	if (!penv)
		return -ENODEV;

	if ((!unsafe_ch_list) || (ch_count > CNSS_MAX_CH_NUM))
		return -EINVAL;

	penv->unsafe_ch_count = ch_count;

	if (ch_count != 0)
		memcpy((char *)penv->unsafe_ch_list, (char *)unsafe_ch_list,
			ch_count * sizeof(u16));

	return 0;
}
EXPORT_SYMBOL(cnss_set_wlan_unsafe_channel);

int cnss_get_wlan_unsafe_channel(u16 *unsafe_ch_list,
					u16 *ch_count, u16 buf_len)
{
	if (!penv)
		return -ENODEV;

	if (!unsafe_ch_list || !ch_count)
		return -EINVAL;

	if (buf_len < (penv->unsafe_ch_count * sizeof(u16)))
		return -ENOMEM;

	*ch_count = penv->unsafe_ch_count;
	memcpy((char *)unsafe_ch_list, (char *)penv->unsafe_ch_list,
			penv->unsafe_ch_count * sizeof(u16));

	return 0;
}
EXPORT_SYMBOL(cnss_get_wlan_unsafe_channel);

int cnss_wlan_set_dfs_nol(void *info, u16 info_len)
{
	void *temp;

	if (!penv)
		return -ENODEV;

	if (!info || !info_len)
		return -EINVAL;

	temp = kmalloc(info_len, GFP_KERNEL);
	if (!temp)
		return -ENOMEM;

	memcpy(temp, info, info_len);

	kfree(penv->dfs_nol_info);

	penv->dfs_nol_info = temp;
	penv->dfs_nol_info_len = info_len;

	return 0;
}
EXPORT_SYMBOL(cnss_wlan_set_dfs_nol);

int cnss_wlan_get_dfs_nol(void *info, u16 info_len)
{
	int len;

	if (!penv)
		return -ENODEV;

	if (!info || !info_len)
		return -EINVAL;

	if (penv->dfs_nol_info == NULL || penv->dfs_nol_info_len == 0)
		return -ENOENT;

	len = min(info_len, penv->dfs_nol_info_len);

	memcpy(info, penv->dfs_nol_info, len);

	return len;
}
EXPORT_SYMBOL(cnss_wlan_get_dfs_nol);

void cnss_pm_wake_lock_init(struct wakeup_source *ws, const char *name)
{
	wakeup_source_init(ws, name);
}
EXPORT_SYMBOL(cnss_pm_wake_lock_init);

void cnss_pm_wake_lock(struct wakeup_source *ws)
{
	__pm_stay_awake(ws);
}
EXPORT_SYMBOL(cnss_pm_wake_lock);

void cnss_pm_wake_lock_timeout(struct wakeup_source *ws, ulong msec)
{
	__pm_wakeup_event(ws, msec);
}
EXPORT_SYMBOL(cnss_pm_wake_lock_timeout);

void cnss_pm_wake_lock_release(struct wakeup_source *ws)
{
	__pm_relax(ws);
}
EXPORT_SYMBOL(cnss_pm_wake_lock_release);

void cnss_pm_wake_lock_destroy(struct wakeup_source *ws)
{
	wakeup_source_trash(ws);
}
EXPORT_SYMBOL(cnss_pm_wake_lock_destroy);

void cnss_lock_pm_sem(void)
{
	down_read(&cnss_pm_sem);
}
EXPORT_SYMBOL(cnss_lock_pm_sem);

void cnss_release_pm_sem(void)
{
	up_read(&cnss_pm_sem);
}
EXPORT_SYMBOL(cnss_release_pm_sem);

void cnss_flush_work(void *work)
{
	struct work_struct *cnss_work = work;
	cancel_work_sync(cnss_work);
}
EXPORT_SYMBOL(cnss_flush_work);

void cnss_flush_delayed_work(void *dwork)
{
	struct delayed_work *cnss_dwork = dwork;
	cancel_delayed_work_sync(cnss_dwork);
}
EXPORT_SYMBOL(cnss_flush_delayed_work);

void cnss_get_monotonic_boottime(struct timespec *ts)
{
	get_monotonic_boottime(ts);
}
EXPORT_SYMBOL(cnss_get_monotonic_boottime);

void cnss_get_boottime(struct timespec *ts)
{
	ktime_get_ts(ts);
}
EXPORT_SYMBOL(cnss_get_boottime);

void cnss_init_work(struct work_struct *work, work_func_t func)
{
	INIT_WORK(work, func);
}
EXPORT_SYMBOL(cnss_init_work);

void cnss_init_delayed_work(struct delayed_work *work, work_func_t func)
{
	INIT_DELAYED_WORK(work, func);
}
EXPORT_SYMBOL(cnss_init_delayed_work);

int cnss_get_ramdump_mem(unsigned long *address, unsigned long *size)
{
	if (!penv || !penv->pldev)
		return -ENODEV;

	*address = penv->ramdump_phys;
	*size = penv->ramdump_size;

	return 0;
}
EXPORT_SYMBOL(cnss_get_ramdump_mem);

void *cnss_get_virt_ramdump_mem(unsigned long *size)
{
	if (!penv || !penv->pldev)
		return NULL;

	*size = penv->ramdump_size;

	return penv->ramdump_addr;
}
EXPORT_SYMBOL(cnss_get_virt_ramdump_mem);

void cnss_device_crashed(void)
{
	if (penv && penv->subsys) {
		subsys_set_crash_status(penv->subsys, true);
		subsystem_restart_dev(penv->subsys);
	}
}
EXPORT_SYMBOL(cnss_device_crashed);

int cnss_set_cpus_allowed_ptr(struct task_struct *task, ulong cpu)
{
	return set_cpus_allowed_ptr(task, cpumask_of(cpu));
}
EXPORT_SYMBOL(cnss_set_cpus_allowed_ptr);

static int cnss_shutdown(const struct subsys_desc *subsys, bool force_stop)
{
	struct cnss_wlan_driver *wdrv;
	struct pci_dev *pdev;
	struct cnss_wlan_vreg_info *vreg_info;
	struct cnss_wlan_gpio_info *gpio_info;
	int ret = 0;

	if (!penv)
		return -ENODEV;

	penv->recovery_in_progress = true;
	wdrv = penv->driver;
	pdev = penv->pdev;
	vreg_info = &penv->vreg_info;
	gpio_info = &penv->gpio_info;

	if (!pdev) {
		ret = -EINVAL;
		goto cut_power;
	}

	if (wdrv && wdrv->shutdown)
		wdrv->shutdown(pdev);

	if (penv->pcie_link_state && !penv->pcie_link_down_ind) {
		pci_save_state(pdev);
		penv->saved_state = pci_store_saved_state(pdev);
		if (msm_pcie_pm_control(MSM_PCIE_SUSPEND,
					cnss_get_pci_dev_bus_number(pdev),
					NULL, NULL, PM_OPTIONS)) {
			pr_debug("cnss: Failed to shutdown PCIe link!\n");
			ret = -EFAULT;
		}
		penv->pcie_link_state = PCIE_LINK_DOWN;
	} else if (penv->pcie_link_state && penv->pcie_link_down_ind) {
		if (msm_pcie_pm_control(MSM_PCIE_SUSPEND,
				cnss_get_pci_dev_bus_number(pdev),
				NULL, NULL, PM_OPTIONS_SUSPEND_LINK_DOWN)) {
			pr_debug("cnss: Failed to shutdown PCIe link!\n");
			ret = -EFAULT;
		}
		penv->saved_state = NULL;
		penv->pcie_link_state = PCIE_LINK_DOWN;
	}

cut_power:
	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);

	if (cnss_wlan_vreg_set(vreg_info, VREG_OFF))
		pr_err("cnss: Failed to set WLAN VREG_OFF!\n");

	return ret;
}

static int cnss_powerup(const struct subsys_desc *subsys)
{
	struct cnss_wlan_driver *wdrv;
	struct pci_dev *pdev;
	struct cnss_wlan_vreg_info *vreg_info;
	struct cnss_wlan_gpio_info *gpio_info;
	int ret = 0;

	if (!penv)
		return -ENODEV;

	if (!penv->driver)
		goto out;

	wdrv = penv->driver;
	pdev = penv->pdev;
	vreg_info = &penv->vreg_info;
	gpio_info = &penv->gpio_info;

	ret = cnss_wlan_vreg_set(vreg_info, VREG_ON);
	if (ret) {
		pr_err("cnss: Failed to set WLAN VREG_ON!\n");
		goto err_wlan_vreg_on;
	}

	usleep(POWER_ON_DELAY);
	cnss_wlan_gpio_set(gpio_info, WLAN_EN_HIGH);
	usleep(WLAN_ENABLE_DELAY);

	if (!pdev) {
		pr_err("%d: invalid pdev\n", __LINE__);
		goto err_pcie_link_up;
	}

	if (!penv->pcie_link_state && !penv->pcie_link_down_ind) {
		ret = msm_pcie_pm_control(MSM_PCIE_RESUME,
				  cnss_get_pci_dev_bus_number(pdev),
				  NULL, NULL, PM_OPTIONS);

		if (ret) {
			pr_err("cnss: Failed to bring-up PCIe link!\n");
			goto err_pcie_link_up;
		}
		penv->pcie_link_state = PCIE_LINK_UP;

	} else if (!penv->pcie_link_state && penv->pcie_link_down_ind) {
		ret = msm_pcie_pm_control(MSM_PCIE_RESUME,
			cnss_get_pci_dev_bus_number(pdev),
			NULL, NULL, PM_OPTIONS_RESUME_LINK_DOWN);

		if (ret) {
			pr_err("cnss: Failed to bring-up PCIe link!\n");
			goto err_pcie_link_up;
		}
		penv->pcie_link_state = PCIE_LINK_UP;
		ret = msm_pcie_recover_config(penv->pdev);
		if (ret) {
			pr_err("cnss: PCI link failed to recover\n");
			goto err_pcie_link_up;
		}
		penv->pcie_link_down_ind = false;
	}

	if (wdrv && wdrv->reinit) {
		if (penv->saved_state)
			pci_load_and_free_saved_state(pdev,
				&penv->saved_state);

		pci_restore_state(pdev);

		ret = wdrv->reinit(pdev, penv->id);
		if (ret) {
			pr_err("%d: Failed to do reinit\n", __LINE__);
			goto err_wlan_reinit;
		}
	} else {
		pr_err("%d: wdrv->reinit is invalid\n", __LINE__);
			goto err_pcie_link_up;
	}

	if (penv->notify_modem_status && wdrv->modem_status)
		wdrv->modem_status(pdev, penv->modem_current_status);

out:
	penv->recovery_in_progress = false;
	return ret;

err_wlan_reinit:
	pci_save_state(pdev);
	penv->saved_state = pci_store_saved_state(pdev);
	msm_pcie_pm_control(MSM_PCIE_SUSPEND,
			cnss_get_pci_dev_bus_number(pdev),
			NULL, NULL, PM_OPTIONS);
	penv->pcie_link_state = PCIE_LINK_DOWN;

err_pcie_link_up:
	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);
	cnss_wlan_vreg_set(vreg_info, VREG_OFF);
	if (penv->pdev) {
		pr_err("%d: Unregistering pci device\n", __LINE__);
		pci_unregister_driver(&cnss_wlan_pci_driver);
		penv->pdev = NULL;
		penv->pci_register_again = true;
	}

err_wlan_vreg_on:
	return ret;
}

static int cnss_ramdump(int enable, const struct subsys_desc *subsys)
{
	struct ramdump_segment segment;

	if (!penv)
		return -ENODEV;

	if (!penv->ramdump_size)
		return -ENOENT;

	if (!enable)
		return 0;

	memset(&segment, 0, sizeof(segment));
	segment.address = penv->ramdump_phys;
	segment.size = penv->ramdump_size;

	return do_ramdump(penv->ramdump_dev, &segment, 1);
}

static void cnss_crash_shutdown(const struct subsys_desc *subsys)
{
	struct cnss_wlan_driver *wdrv;
	struct pci_dev *pdev;

	if (!penv)
		return;

	wdrv = penv->driver;
	pdev = penv->pdev;

	if (pdev && wdrv && wdrv->crash_shutdown)
		wdrv->crash_shutdown(pdev);
}

void cnss_device_self_recovery(void)
{
	if (!penv)
		return;

	if (penv->recovery_in_progress) {
		pr_err("cnss: Recovery already in progress\n");
		return;
	}
	if (penv->driver_status == CNSS_LOAD_UNLOAD) {
		pr_err("cnss: load unload in progress\n");
		return;
	}
	penv->recovery_count++;
	penv->recovery_in_progress = true;
	cnss_pm_wake_lock(&penv->ws);
	cnss_shutdown(NULL, false);
	usleep(WLAN_RECOVERY_DELAY);
	cnss_powerup(NULL);
	cnss_pm_wake_lock_release(&penv->ws);
	penv->recovery_in_progress = false;
}
EXPORT_SYMBOL(cnss_device_self_recovery);

static int cnss_modem_notifier_nb(struct notifier_block *this,
				  unsigned long code,
				  void *ss_handle)
{
	struct cnss_wlan_driver *wdrv;
	struct pci_dev *pdev;

	pr_debug("%s: Modem-Notify: event %lu\n", __func__, code);

	if (!penv)
		return NOTIFY_DONE;

	if (SUBSYS_AFTER_POWERUP == code)
		penv->modem_current_status = 1;
	else if (SUBSYS_BEFORE_SHUTDOWN == code)
		penv->modem_current_status = 0;
	else
		return NOTIFY_DONE;

	wdrv = penv->driver;
	pdev = penv->pdev;

	if (!wdrv || !pdev || !wdrv->modem_status)
		return NOTIFY_DONE;

	wdrv->modem_status(pdev, penv->modem_current_status);

	return NOTIFY_OK;
}

static struct notifier_block mnb = {
	.notifier_call = cnss_modem_notifier_nb,
};

static int cnss_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct esoc_desc *desc;
	const char *client_desc;
	struct device *dev = &pdev->dev;
	struct msm_client_dump dump_entry;
	struct resource *res;
	u32 ramdump_size = 0;

	if (penv)
		return -ENODEV;

	penv = devm_kzalloc(&pdev->dev, sizeof(*penv), GFP_KERNEL);
	if (!penv)
		return -ENOMEM;

	penv->pldev = pdev;
	penv->esoc_desc = NULL;

	penv->gpio_info.name = WLAN_EN_GPIO_NAME;
	penv->gpio_info.num = 0;
	penv->gpio_info.state = WLAN_EN_LOW;
	penv->gpio_info.init = WLAN_EN_HIGH;
	penv->gpio_info.prop = false;
	penv->vreg_info.wlan_reg = NULL;
	penv->vreg_info.state = VREG_OFF;
	penv->pci_register_again = false;

	ret = cnss_wlan_get_resources(pdev);
	if (ret)
		goto err_get_wlan_res;

	penv->pcie_link_state = PCIE_LINK_UP;

	penv->notify_modem_status =
		of_property_read_bool(dev->of_node,
				      "qcom,notify-modem-status");

	if (penv->notify_modem_status) {
		ret = of_property_read_string_index(dev->of_node, "esoc-names",
						    0, &client_desc);
		if (ret) {
			pr_debug("%s: esoc-names is not defined in DT, SKIP\n",
				__func__);
		} else {
			desc = devm_register_esoc_client(dev, client_desc);
			if (IS_ERR_OR_NULL(desc)) {
				ret = PTR_RET(desc);
				pr_err("%s: can't find esoc desc\n", __func__);
				goto err_esoc_reg;
			}
			penv->esoc_desc = desc;
		}
	}

	penv->subsysdesc.name = "AR6320";
	penv->subsysdesc.owner = THIS_MODULE;
	penv->subsysdesc.shutdown = cnss_shutdown;
	penv->subsysdesc.powerup = cnss_powerup;
	penv->subsysdesc.ramdump = cnss_ramdump;
	penv->subsysdesc.crash_shutdown = cnss_crash_shutdown;
	penv->subsysdesc.dev = &pdev->dev;
	penv->subsys = subsys_register(&penv->subsysdesc);
	if (IS_ERR(penv->subsys)) {
		ret = PTR_ERR(penv->subsys);
		goto err_subsys_reg;
	}

	if (of_property_read_u32(dev->of_node, "qcom,wlan-ramdump-dynamic",
				&ramdump_size) == 0) {
		penv->ramdump_addr = dma_alloc_coherent(&pdev->dev,
				ramdump_size, &penv->ramdump_phys, GFP_KERNEL);

		if (penv->ramdump_addr)
			penv->ramdump_size = ramdump_size;
		penv->ramdump_dynamic = true;
	} else {
		res = platform_get_resource_byname(penv->pldev,
				IORESOURCE_MEM, "ramdump");
		if (res) {
			penv->ramdump_phys = res->start;
			ramdump_size = resource_size(res);
			penv->ramdump_addr = ioremap(penv->ramdump_phys,
					ramdump_size);

			if (penv->ramdump_addr)
				penv->ramdump_size = ramdump_size;

			penv->ramdump_dynamic = false;
		}
	}

	pr_debug("%s: ramdump addr: %p, phys: %pa\n", __func__,
			penv->ramdump_addr, &penv->ramdump_phys);

	if (penv->ramdump_size == 0) {
		pr_info("%s: CNSS ramdump will not be collected", __func__);
		goto skip_ramdump;
	}

	dump_entry.id = MSM_CNSS_WLAN;
	dump_entry.start_addr = penv->ramdump_phys;
	dump_entry.end_addr = dump_entry.start_addr + penv->ramdump_size;

	ret = msm_dump_table_register(&dump_entry);
	if (ret) {
		pr_err("%s: Dump table setup failed: %d\n", __func__, ret);
		goto err_ramdump_create;
	}

	penv->subsys_handle = subsystem_get(penv->subsysdesc.name);

	penv->ramdump_dev = create_ramdump_device(penv->subsysdesc.name,
				penv->subsysdesc.dev);
	if (!penv->ramdump_dev) {
		ret = -ENOMEM;
		goto err_ramdump_create;
	}

skip_ramdump:
	penv->modem_current_status = 0;

	if (penv->notify_modem_status) {
		penv->modem_notify_handler =
			subsys_notif_register_notifier(penv->esoc_desc ?
						       penv->esoc_desc->name :
						       "modem", &mnb);
		if (IS_ERR(penv->modem_notify_handler)) {
			ret = PTR_ERR(penv->modem_notify_handler);
			pr_err("%s: Register notifier Failed\n", __func__);
			goto err_notif_modem;
		}
	}

	ret = pci_register_driver(&cnss_wlan_pci_driver);
	if (ret)
		goto err_pci_reg;

	penv->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!penv->bus_scale_table)
		goto err_bus_scale;

	penv->bus_client = msm_bus_scale_register_client
	    (penv->bus_scale_table);
	if (!penv->bus_client)
		goto err_bus_reg;

	cnss_pm_wake_lock_init(&penv->ws, "cnss_wlock");

	register_pm_notifier(&cnss_pm_notifier);

#ifdef CONFIG_CNSS_MAC_BUG
	/* 0-4K memory is reserved for QCA6174 to address a MAC HW bug.
	 * MAC would do an invalid pointer fetch based on the data
	 * that was read from 0 to 4K. So fill it with zero's (to an
	 * address for which PCIe RC honored the read without any errors).
	 */
	memset(phys_to_virt(0), 0, SZ_4K);
#endif

	pr_info("cnss: Platform driver probed successfully.\n");
	return ret;

err_bus_reg:
	msm_bus_cl_clear_pdata(penv->bus_scale_table);

err_bus_scale:
	pci_unregister_driver(&cnss_wlan_pci_driver);

err_pci_reg:
	destroy_ramdump_device(penv->ramdump_dev);

err_ramdump_create:
	if (penv->subsys_handle)
		subsystem_put(penv->subsys_handle);
	if (penv->notify_modem_status)
		subsys_notif_unregister_notifier
			(penv->modem_notify_handler, &mnb);

err_notif_modem:
	if (penv->ramdump_addr) {
		if (penv->ramdump_dynamic) {
			dma_free_coherent(&pdev->dev, penv->ramdump_size,
					penv->ramdump_addr, penv->ramdump_phys);
		} else {
			iounmap(penv->ramdump_addr);
		}
	}

	subsys_unregister(penv->subsys);

err_subsys_reg:
	if (penv->esoc_desc)
		devm_unregister_esoc_client(&pdev->dev, penv->esoc_desc);

err_esoc_reg:
	cnss_wlan_release_resources();

err_get_wlan_res:
	penv = NULL;

	return ret;
}

static int cnss_remove(struct platform_device *pdev)
{
	struct cnss_wlan_vreg_info *vreg_info = &penv->vreg_info;
	struct cnss_wlan_gpio_info *gpio_info = &penv->gpio_info;

	unregister_pm_notifier(&cnss_pm_notifier);
	cnss_pm_wake_lock_destroy(&penv->ws);

	kfree(penv->dfs_nol_info);

	if (penv->bus_client)
		msm_bus_scale_unregister_client(penv->bus_client);

	cnss_pm_wake_lock_destroy(&penv->ws);

	if (penv->ramdump_addr) {
		if (penv->ramdump_dynamic) {
			dma_free_coherent(&pdev->dev, penv->ramdump_size,
					penv->ramdump_addr, penv->ramdump_phys);
		} else {
			iounmap(penv->ramdump_addr);
		}
	}

	cnss_wlan_gpio_set(gpio_info, WLAN_EN_LOW);
	if (cnss_wlan_vreg_set(vreg_info, VREG_OFF))
		pr_err("Failed to turn OFF wlan vreg\n");
	cnss_wlan_release_resources();

	return 0;
}

static const struct of_device_id cnss_dt_match[] = {
	{.compatible = "qcom,cnss"},
	{}
};

MODULE_DEVICE_TABLE(of, cnss_dt_match);

static struct platform_driver cnss_driver = {
	.probe  = cnss_probe,
	.remove = cnss_remove,
	.driver = {
		.name = "cnss",
		.owner = THIS_MODULE,
		.of_match_table = cnss_dt_match,
	},
};

static int __init cnss_initialize(void)
{
	return platform_driver_register(&cnss_driver);
}

static void __exit cnss_exit(void)
{
	struct platform_device *pdev = penv->pldev;
	if (penv->ramdump_dev)
		destroy_ramdump_device(penv->ramdump_dev);
	if (penv->notify_modem_status)
		subsys_notif_unregister_notifier(penv->modem_notify_handler,
						 &mnb);
	subsys_unregister(penv->subsys);
	if (penv->esoc_desc)
		devm_unregister_esoc_client(&pdev->dev, penv->esoc_desc);
	platform_driver_unregister(&cnss_driver);
}

void cnss_request_pm_qos(u32 qos_val)
{
	pm_qos_add_request(&penv->qos_request, PM_QOS_CPU_DMA_LATENCY, qos_val);
}
EXPORT_SYMBOL(cnss_request_pm_qos);

void cnss_remove_pm_qos(void)
{
	pm_qos_remove_request(&penv->qos_request);
}
EXPORT_SYMBOL(cnss_remove_pm_qos);

int cnss_request_bus_bandwidth(int bandwidth)
{
	int ret = 0;

	if (!penv || !penv->bus_client)
		return -ENODEV;

	switch (bandwidth) {
	case CNSS_BUS_WIDTH_NONE:
	case CNSS_BUS_WIDTH_LOW:
	case CNSS_BUS_WIDTH_MEDIUM:
	case CNSS_BUS_WIDTH_HIGH:
		ret = msm_bus_scale_client_update_request(penv->bus_client,
							  bandwidth);
		if (ret)
			pr_err("%s: could not set bus bandwidth %d, ret = %d\n",
			       __func__, bandwidth, ret);
		break;

	default:
		pr_err("%s: Invalid request %d", __func__, bandwidth);
		ret = -EINVAL;

	}
	return ret;
}
EXPORT_SYMBOL(cnss_request_bus_bandwidth);

int cnss_get_platform_cap(struct cnss_platform_cap *cap)
{
	if (!penv)
		return -ENODEV;

	if (cap)
		*cap = penv->cap;

	return 0;

}
EXPORT_SYMBOL(cnss_get_platform_cap);

void cnss_set_driver_status(enum cnss_driver_status driver_status)
{
	penv->driver_status = driver_status;
}
EXPORT_SYMBOL(cnss_set_driver_status);

#ifdef CONFIG_CNSS_SECURE_FW
int cnss_get_sha_hash(const u8 *data, u32 data_len, u8 *hash_idx, u8 *out)
{
	struct scatterlist sg;
	struct hash_desc desc;
	int ret = 0;

	if (!out) {
		pr_err("memory for output buffer is not allocated\n");
		ret = -EINVAL;
		goto end;
	}

	desc.flags = CRYPTO_TFM_REQ_MAY_SLEEP;
	desc.tfm   = crypto_alloc_hash(hash_idx, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(desc.tfm)) {
		pr_err("crypto_alloc_hash failed:%ld\n", PTR_ERR(desc.tfm));
		ret = PTR_ERR(desc.tfm);
		goto end;
	}

	sg_init_one(&sg, data, data_len);
	ret = crypto_hash_digest(&desc, &sg, sg.length, out);
	crypto_free_hash(desc.tfm);
end:
	return ret;
}
EXPORT_SYMBOL(cnss_get_sha_hash);

void *cnss_get_fw_ptr(void)
{
	if (!penv)
		return NULL;

	return penv->fw_mem;
}
EXPORT_SYMBOL(cnss_get_fw_ptr);
#endif

/* wlan prop driver cannot invoke show_stack
 * function directly, so to invoke this function it
 * call wcnss_dump_stack function
 */
void cnss_dump_stack(struct task_struct *task)
{
	show_stack(task, NULL);
}
EXPORT_SYMBOL(cnss_dump_stack);

module_init(cnss_initialize);
module_exit(cnss_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(DEVICE "CNSS Driver");
