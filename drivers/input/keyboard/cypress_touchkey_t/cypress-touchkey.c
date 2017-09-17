/*
 * cypress_touchkey.c - Platform data for cypress touchkey driver
 *
 * Copyright (C) 2011 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include "cypress-touchkey.h"
#include "cy8cmbr_swd.h"

struct qpnp_pin_cfg cypress_int_set[] = {
	{
		.mode = 0,
		.pull = 5,
		.output_type = 0,
		.invert = 0,
		.vin_sel = 2,
		.out_strength = 2,
		.src_sel = 3,
		.master_en = 1,

	},
	{
		.mode = 0,
		.pull = 4,
		.output_type = 0,
		.invert = 0,
		.vin_sel = 2,
		.out_strength = 2,
		.src_sel = 3,
		.master_en = 1,
	},
};

#ifdef USE_OPEN_CLOSE
static int cypress_input_open(struct input_dev *dev);
static void cypress_input_close(struct input_dev *dev);
#endif

#ifdef TKEY_REQUEST_FW_UPDATE
static int tkey_load_fw(struct cypress_touchkey_info *info, u8 fw_path);
static int tkey_unload_fw(struct cypress_touchkey_info *info, u8 fw_path);
#endif

static void cypress_touchkey_reset(struct cypress_touchkey_info *info);

static int touchkey_led_status;
static int touchled_cmd_reversed;

extern int boot_mode_recovery;
/*extern int get_lcd_attached(void);*/

static int cypress_touchkey_i2c_read(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(&client->dev);
	int err = 0;
	int retry = 3;

	while (retry--) {
		if (!info->enabled) {
			dev_info(&client->dev, "%s: touchkey is not enabled\n", __func__);
			return -1;
		}

		err = i2c_smbus_read_i2c_block_data(client, reg, len, val);
		if (err >= 0)
			return err;

		dev_info(&client->dev, "%s: i2c transfer error.\n", __func__);
		msleep(20);
	}
	return err;

}

static int cypress_touchkey_i2c_write(struct i2c_client *client,
		u8 reg, u8 *val, unsigned int len)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(&client->dev);
	int err = 0;
	int retry = 3;

	while (retry--) {
		if (!info->enabled) {
			dev_info(&client->dev, "%s: touchkey is not enabled\n", __func__);
			return -1;
		}

		err = i2c_smbus_write_i2c_block_data(client, reg, len, val);
		if (err >= 0)
			return err;

		dev_info(&client->dev, "%s: i2c transfer error. (%d)\n", __func__,err);
		msleep(20);
	}
	return err;
}

static void cypress_touchkey_interrupt_set_dual(struct i2c_client *client)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(&client->dev);
	int ret = 0;
	int retry = 5;
	u8 data[3] = {0, };

	if (info->device_ver != 0x40) { /* support CYPRESS MBR3155 */
		dev_err(&client->dev, "%s: not support this module\n", __func__);
		return;
	}

	if (info->ic_fw_ver <= CYPRESS_RECENT_BACK_REPORT_FW_VER) {
		dev_err(&client->dev, "%s: not support this version\n", __func__);
		return;
	}

	while (retry--) {
		data[0] = TK_CMD_DUAL_DETECTION;
		data[1] = 0x00;
		data[2] = TK_BIT_DETECTION_CONFIRM;

		ret = i2c_smbus_write_i2c_block_data(client, CYPRESS_GEN, 3, &data[0]);
		if (ret < 0) {
			dev_err(&client->dev, "%s: i2c write error. (%d)\n", __func__, ret);
			msleep(30);
			continue;
		}
		msleep(30);

		data[0] = CYPRESS_DETECTION_FLAG;

		ret = i2c_smbus_read_i2c_block_data(client, data[0], 1, &data[1]);
		if (ret < 0) {
			dev_err(&client->dev, "%s: i2c read error. (%d)\n", __func__, ret);
			msleep(30);
			continue;
		}

		if (data[1] != 1) {
			dev_err(&client->dev,
				"%s: interrupt set: 0x%X, failed.\n", __func__, data[1]);
			continue;
		}

		dev_info(&client->dev, "%s: interrupt set: 0x%X\n", __func__, data[1]);
		break;
	}

}

static int tkey_i2c_check(struct cypress_touchkey_info *info)
{
	struct i2c_client *client = info->client;

	int ret = 0;
	u8 data[3] = { 0, };

	ret = cypress_touchkey_i2c_read(info->client, CYPRESS_FW_VER, data, 3);

	if (ret < 0) {
		dev_err(&client->dev, "Failed to read Module version\n");
		info->ic_fw_ver = 0;
		info->module_ver = 0;
		info->device_ver = 0;
		return ret;
	}

	info->ic_fw_ver = data[0];
	info->module_ver = data[1];
	info->device_ver = data[2];
	dev_info(&client->dev, "%s: ic_fw_ver = 0x%02x, module_ver = 0x%02x\n",
		__func__, info->ic_fw_ver, info->module_ver);

	/* CY device bit 6 (0x40) = MBR3155 */
	if(info->device_ver == 0x40)
		dev_info(&client->dev, "%s: CY device(0x%02x) = CY8CMBR3155\n"
					,__func__,info->device_ver);
	else
		dev_info(&client->dev, "%s: CY device(0x%02x) is not supported ver.\n"
					,__func__,info->device_ver);

	/* CYPRESS Firmware setting interrupt type : dual or single interrupt */
	cypress_touchkey_interrupt_set_dual(info->client);

	/* Read internal checksum*/
#ifdef CRC_CHECK_INTERNAL
	ret = cypress_touchkey_i2c_read(info->client, CYPRESS_CRC, data, 2);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to read crc\n");
		info->crc = 0;
		return ret;
	}

	info->crc = ((0xFF & data[1]) << 8) | data[0];
#endif
	return ret;
}

static int cypress_power_onoff(struct device *dev, bool onoff)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int rc;

	dev_info(&info->client->dev, "%s: power %s\n",
			__func__, onoff ? "on" : "off");

	if (!info->touch_3p3) {
		info->touch_3p3 = regulator_get(&info->client->dev,
			"touch_3p3");
		if (IS_ERR(info->touch_3p3) || !info->touch_3p3) {
			dev_err(&info->client->dev,
				"Regulator(touch_3p3) get failed rc = %ld\n",
					PTR_ERR(info->touch_3p3));
			return -1;
		}

		rc = regulator_set_voltage(info->touch_3p3,
			3300000, 3300000);
		if (rc) {
			dev_err(&info->client->dev,
				"regulator(touch_3p3) set_vtg failed rc=%d\n", rc);
			return rc;
		}
	}

	if (!info->vcc_en) {
		info->vcc_en = regulator_get(&info->client->dev,
			"vcc_en");
		if (IS_ERR(info->vcc_en) || !info->vcc_en) {
			dev_err(&info->client->dev,
				"Regulator(vcc_en) get failed rc = %ld\n",
					PTR_ERR(info->vcc_en));
			return -1;
		}
	}

	if (onoff) {
		if (!info->init_done || !regulator_is_enabled(info->touch_3p3)) {
			rc = regulator_enable(info->touch_3p3);
			if (rc) {
				dev_err(&info->client->dev,
					"Regulator touch_3p3 enable failed rc=%d\n", rc);
				return rc;
			}
		} else {
			dev_info(&info->client->dev, "%s: 3p3 is enabled\n", __func__);
		}

		if (!info->init_done || !regulator_is_enabled(info->vcc_en)) {
			rc = regulator_enable(info->vcc_en);
			if (rc) {
				dev_err(&info->client->dev,
					"Regulator vcc_en enable failed rc=%d\n", rc);
				return rc;
			}
		} else {
			dev_info(&info->client->dev, "%s: vcc is enabled\n", __func__);
		}

		rc = qpnp_pin_config(info->pdata->gpio_int,
				&cypress_int_set[CYPRESS_GPIO_STATE_WAKE]);
		if (rc < 0)
			dev_info(dev, "%s: wakeup int config return: %d\n", __func__, rc);

	} else {
		rc = qpnp_pin_config(info->pdata->gpio_int,
				&cypress_int_set[CYPRESS_GPIO_STATE_SLEEP]);
		if (rc < 0)
			dev_info(dev, "%s: sleep int config return: %d\n", __func__, rc);

		if (regulator_is_enabled(info->touch_3p3)) {
			rc = regulator_disable(info->touch_3p3);
			if (rc) {
				dev_err(&info->client->dev,
					"Regulator touch_3p3 disable failed rc=%d\n", rc);
				return rc;
			}
		} else {
			dev_info(&info->client->dev, "%s: 3p3 is disabled\n", __func__);

		}

		if (regulator_is_enabled(info->vcc_en)) {
			rc = regulator_disable(info->vcc_en);
			if (rc) {
				dev_err(&info->client->dev,
					"Regulator vcc_en disable failed rc=%d\n", rc);
				return rc;
			}
		} else {
			dev_info(&info->client->dev, "%s: vcc is disabled\n", __func__);

		}
	}
	return 1;
}

static void cypress_touchkey_reset(struct cypress_touchkey_info *info)
{
	int ret;

	dev_info(&info->client->dev, "%s\n", __func__);

	input_report_key(info->input_dev, info->keycode[0], 0);
	input_report_key(info->input_dev, info->keycode[1], 0);
	input_sync(info->input_dev);

#ifdef TKEY_BOOSTER
	if (info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, 2);
	dev_info(&info->client->dev,
			"%s: dvfs_lock free.\n", __func__);
#endif
	info->is_powering_on = true;
	if (info->irq_enable) {
		info->irq_enable = false;
		disable_irq(info->client->irq);
	}
	info->enabled = false;
	cypress_power_onoff(&info->client->dev, 0);

	msleep(30);

	cypress_power_onoff(&info->client->dev, 1);
	msleep(200);

	/* CYPRESS Firmware setting interrupt type : dual or single interrupt */
	cypress_touchkey_interrupt_set_dual(info->client);

	info->enabled = true;

	if (touchled_cmd_reversed) {
		touchled_cmd_reversed = 0;
		ret = i2c_smbus_write_byte_data(info->client,
				CYPRESS_GEN, touchkey_led_status);
		if (ret < 0) {
			dev_err(&info->client->dev,
					"%s: i2c write error [%d]\n", __func__, ret);
		}
		dev_info(&info->client->dev,
				"%s: LED returned on\n", __func__);
		msleep(30);
	}

	if (!info->irq_enable) {
		enable_irq(info->client->irq);
		info->irq_enable = true;
	}

	info->is_powering_on = false;
}

#ifdef CONFIG_GLOVE_TOUCH
static void cypress_touchkey_glove_work(struct work_struct *work)
{
	struct cypress_touchkey_info *info =
		container_of(work, struct cypress_touchkey_info, glove_work.work);

	u8 data[2] = { 0, };
	int ret = 0;
	u8 retry = 0;
	u8 glove_bit;

	if (info->glove_value == 1) {
		/* Send glove Command */
		data[0] = TK_BIT_CMD_GLOVE;
		data[1] = TK_BIT_WRITE_CONFIRM;
	} else {
		data[0] = TK_BIT_CMD_REGULAR;
		data[1] = TK_BIT_WRITE_CONFIRM;
	}

	cypress_touchkey_i2c_write(info->client, CYPRESS_OP_MODE, data, 2);
	pr_err("[HASH] data[0] =  [[[  %x  ]]]\n",data[0]);

	while (retry < 3) {
		msleep(30);

		/* Check status flag mode */
		ret = cypress_touchkey_i2c_read(info->client, CYPRESS_STATUS_FLAG, data, 1);
		if (ret < 0) {
			dev_info(&info->client->dev, "%s: Failed to check status flag.\n",
					__func__);
			return;
		}

		glove_bit = !!(data[0] & TK_BIT_GLOVE);
		dev_err(&info->client->dev,
			"data[0]=%x, 1mm: %x, flip: %x, glove: %x, ta: %x\n\n",
			data[0], data[0] & 0x20, data[0] & 0x10, data[0] & 0x08, data[0] & 0x04);

		if (info->glove_value == glove_bit) {
			dev_info(&info->client->dev, "%s: Glove mode is %s\n",
					__func__, info->glove_value ? "enabled" : "disabled");
				break;
			} else {
			dev_err(&info->client->dev,
					"%s: Error to set glove_mode val %d, bit %d, retry %d\n",
					__func__, info->glove_value, glove_bit, retry);
		}

		retry = retry + 1;
	}

	if (retry == 3)
		dev_err(&info->client->dev, "%s: Failed to set the glove mode\n", __func__);

	return;
}

int cypress_touchkey_glovemode(struct cypress_touchkey_info *info, int value)
{

	if (wake_lock_active(&info->fw_wakelock)) {
		dev_info(&info->client->dev, "wackelock active\n");
		return 0;
	}
/*
	if (info->glove_value == value) {
		dev_info(&info->client->dev,
			"glove mode is already %s\n", value ? "enabled" : "disabled");
		return 0;
	}
*/
	if (!info->irq_enable) {
		dev_info(&info->client->dev, "%s: irq is already diabled\n", __func__);

		return 0;
	}

	mutex_lock(&info->tkey_glove_lock);
	cancel_delayed_work(&info->glove_work);

	info->glove_value = value;

	schedule_delayed_work(&info->glove_work,
		msecs_to_jiffies(TKEY_GLOVE_DWORK_TIME));
	mutex_unlock(&info->tkey_glove_lock);
	return 0;
}
#endif

#ifdef TKEY_FLIP_MODE
void cypress_touchkey_flip_cover(struct cypress_touchkey_info *info, int value)
{
	u8 data[2] = { 0, };
	int ret = 0;
	u8 retry = 0;

	if (!(info->enabled)) {
		dev_info(&info->client->dev, "%s : Touchkey is not enabled.\n",
				__func__);
		return ;
	}

	if (wake_lock_active(&info->fw_wakelock)) {
		dev_info(&info->client->dev, "wackelock active\n");
		return ;
	}

	if (value == 1) {
		if (info->irq_enable) {
			info->irq_enable = false;
			disable_irq(info->irq);
		} else {
			dev_info(&info->client->dev, "%s : already diabled irq.\n",
					__func__);
		}
	} else {
		if (!info->irq_enable) {
			enable_irq(info->irq);
			info->irq_enable = true;
		} else {
			dev_info(&info->client->dev, "%s : already enabled irq.\n",
					__func__);
		}
	}

	return;
//TEST
	if (value == 1) {
		/* Send filp mode Command */
		data[0] = TK_BIT_CMD_FLIP;
		data[1] = TK_BIT_WRITE_CONFIRM;
	} else {
		data[0] = TK_BIT_CMD_REGULAR;
		data[1] = TK_BIT_WRITE_CONFIRM;
	}

	ret = cypress_touchkey_i2c_write(info->client, CYPRESS_OP_MODE, data, 2);
	if (ret < 0) {
		dev_info(&info->client->dev, "%s: Failed to write flip mode command.\n",
				__func__);
		return;
	}

	while (retry < 3) {
		msleep(30);
		/* Check status flag mode */
		ret = cypress_touchkey_i2c_read(info->client, CYPRESS_STATUS_FLAG, data, 1);
		if (ret < 0) {
			dev_info(&info->client->dev, "%s: Failed to check status flag.\n",
					__func__);
			return;
		}

		dev_err(&info->client->dev,
			"data[0]=%x, 1mm: %x, flip: %x, glove: %x, ta: %x\n\n",
			data[0], data[0] & 0x20, data[0] & 0x10, data[0] & 0x08, data[0] & 0x04);

		if (value == 1) {
			if (data[0] & TK_BIT_FLIP) {
				dev_err(&info->client->dev, "%s: Flip mode is enabled\n", __func__);
				info->enabled_flip = true;
				break;
			} else {
				dev_err(&info->client->dev, "%s: flip_mode Enable failed. retry=%d\n",
					__func__, retry);
			}
		} else {
			if (!(data[0] & TK_BIT_FLIP)) {
				dev_err(&info->client->dev,
					"%s: Normal mode from Flip mode\n", __func__);
				info->enabled_flip = false;
				break;
			} else {
				dev_info(&info->client->dev,
					"%s: normal_mode Enable failed. retry=%d \n",
					__func__, retry);
			}
		}
		retry = retry + 1;
	}

	if (retry == 3)
		dev_err(&info->client->dev, "[Touchkey] flip cover failed\n");

	return;
}
#endif

#ifdef TKEY_1MM_MODE
void cypress_touchkey_1mm_mode(struct cypress_touchkey_info *info, int value)
{
	u8 data[2] = { 0, };
	int ret = 0;
	u8 retry = 0;
	u8 stylus_status;

	if (!(info->enabled)) {
		dev_info(&info->client->dev, "%s : Touchkey is not enabled.\n",
				__func__);
		return ;
	}

	if (wake_lock_active(&info->fw_wakelock)) {
		dev_info(&info->client->dev, "wackelock active\n");
		return ;
	}

	if (value == 1) {
	/* Send 1mm mode Command */
		data[0] = TK_BIT_CMD_1MM;
		data[1] = TK_BIT_WRITE_CONFIRM;
	} else {
		data[0] = TK_BIT_CMD_REGULAR;
		data[1] = TK_BIT_WRITE_CONFIRM;
	}

	ret = cypress_touchkey_i2c_write(info->client, CYPRESS_OP_MODE, data, 2);
	if (ret < 0) {
		dev_info(&info->client->dev, "%s: Failed to write 1mm mode command.\n",
				__func__);
		return;
	}

	while (retry < 3) {
		msleep(30);
		/* Check status flag mode */
		ret = cypress_touchkey_i2c_read(info->client, CYPRESS_STATUS_FLAG, data, 1);
		if (ret < 0) {
			dev_info(&info->client->dev, "%s: Failed to check status flag.\n",
					__func__);
			return;
		}
		stylus_status = !!(data[0] & TK_BIT_1MM);

		dev_err(&info->client->dev,
			"data[0]=%x, 1mm: %x, flip: %x, glove: %x, ta: %x\n\n",
			data[0], data[0] & 0x20, data[0] & 0x10, data[0] & 0x08, data[0] & 0x04);

		if (value == stylus_status) {
			dev_info(&info->client->dev,
				"%s: 1MM mode is %s\n",
				__func__,stylus_status ? "enabled" : "disabled");
				break;
			} else {
			dev_err(&info->client->dev,
				"%s: Error to set 1MM mode, val %d, 1mm bit %d, retry %d\n",
				__func__, value, stylus_status, retry);
		}
		retry = retry + 1;
	}

	if (retry == 3)
		dev_err(&info->client->dev, "[Touchkey] 1mm mode failed\n");

	return;
}
#endif

static irqreturn_t cypress_touchkey_interrupt(int irq, void *dev_id)
{
	struct cypress_touchkey_info *info = dev_id;
	u8 buf[10] = {0,};
	int code;
	int press;
	int ret;
	int i;

	if (!atomic_read(&info->keypad_enable)) {
			goto out;
	}

	ret = gpio_get_value(info->pdata->gpio_int);
	if (ret) {
		dev_info(&info->client->dev,
				"%s: not real interrupt (%d).\n", __func__, ret);
		goto out;
	}

	if (info->is_powering_on) {
		dev_info(&info->client->dev,
				"%s: ignoring spurious boot interrupt\n", __func__);
		goto out;
	}


	/* Lentisq bringup */
	ret = i2c_smbus_read_byte_data(info->client, CYPRESS_BUTTON_STATUS);
	if (ret < 0) {
		dev_info(&info->client->dev, "%s: interrupt failed with %d.\n",
				__func__, ret);
		goto out;
	}

	buf[0] = ret;

	/* L OS support Screen Pinning feature.
	 * "recent + back key" event is CombinationKey for exit Screen Pinning mode.
	 * If touchkey firmware version is higher than CYPRESS_RECENT_BACK_REPORT_FW_VER,
	 * touchkey can make interrupt "back key" and "recent key" in same time.
	 * lower version firmware can make interrupt only one key event.
	 */
	if (info->ic_fw_ver >= CYPRESS_RECENT_BACK_REPORT_FW_VER) {
		int menu_data = buf[0] & 0x3;
		int back_data = (buf[0] >> 2) & 0x3;
		u8 menu_press = menu_data % 2;
		u8 back_press = back_data % 2;

		if (menu_data)
			input_report_key(info->input_dev, info->keycode[0], menu_press);
		if (back_data)
			input_report_key(info->input_dev, info->keycode[1], back_press);

		press = menu_press | back_press;

#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		dev_info(&info->client->dev,
				"%s: %s%s%X, fw_ver: 0x%x, modue_ver: 0x%x\n", __func__,
				menu_data ? (menu_press ? "menu P " : "menu R ") : "",
				back_data ? (back_press ? "back P " : "back R ") : "",
				buf[0], info->ic_fw_ver, info->module_ver);
#else
		dev_info(&info->client->dev, "%s: key %s%s fw_ver: 0x%x, modue_ver: 0x%x\n", __func__,
				menu_data ? (menu_press ? "P" : "R") : "",
				back_data ? (back_press ? "P" : "R") : "",
				info->ic_fw_ver, info->module_ver);
#endif

	} else {
		press = !(buf[0] & PRESS_BIT_MASK);
		code = (int)(buf[0] & KEYCODE_BIT_MASK) - 1;
#ifndef CONFIG_SAMSUNG_PRODUCT_SHIP
		dev_info(&info->client->dev,
				"%s: %s key %s. fw=0x%x, md=0x%x \n", __func__,
				code ? "back" : "recent", press ? "pressed" : "released",
				info->ic_fw_ver, info->module_ver);
#else
		dev_info(&info->client->dev,
				"%s: %s. fw=0x%x, md=0x%x \n", __func__,
				press ? "pressed" : "released",
				info->ic_fw_ver, info->module_ver);
#endif
		if (code < 0) {
			dev_info(&info->client->dev,
					"%s, not profer interrupt 0x%2X.(release all finger)\n",
					__func__, buf[0]);
			/* need release all finger function. */
			for (i = 0; i < info->pdata->keycodes_size; i++) {
				input_report_key(info->input_dev, info->keycode[i], 0);
				input_sync(info->input_dev);
			}
			goto out;
		}

		input_report_key(info->input_dev, info->keycode[code], press);
	}
	input_sync(info->input_dev);

#ifdef TKEY_BOOSTER
	if (info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, !!press);
#endif

out:
	return IRQ_HANDLED;
}

static int cypress_touchkey_auto_cal(struct cypress_touchkey_info *info, bool booting)
{
	u8 data[6] = { 0, };
	int count = 0;
	int ret = 0;
	u8 retry = 0;

	while (retry < 3) {
		ret = cypress_touchkey_i2c_read(info->client, CYPRESS_GEN, data, 4);
		if (ret < 0) {
			dev_info(&info->client->dev, "%s: autocal read fail.\n", __func__);
			return ret;
		}

		data[0] = 0x50;
		data[3] = 0x01;

		count = cypress_touchkey_i2c_write(info->client,CYPRESS_GEN, data, 4);
		dev_info(&info->client->dev,
				"%s: data[0]=%x data[1]=%x data[2]=%x data[3]=%x\n",
				__func__, data[0], data[1], data[2], data[3]);

		if (booting)
			msleep(100);
		else
			msleep(130);

		ret = cypress_touchkey_i2c_read(info->client, CYPRESS_GEN, data, 6);

		if (data[5] & 0x80) {	/* TK_BIT_AUTOCAL */
			dev_dbg(&info->client->dev, "%s: Run Autocal\n", __func__);
			break;
		} else {
			dev_info(&info->client->dev, "%s: autocal failed[%x][%d]\n",
					__func__, data[5], ret);
		}
		retry = retry + 1;
	}

	if (retry == 3)
		dev_err(&info->client->dev, "%s: Failed to Set the Autocalibration\n", __func__);

	return count;
}

#ifdef TKEY_REQUEST_FW_UPDATE
#ifdef USE_TKEY_UPDATE_WORK
static void tkey_fw_update_work(struct work_struct *work)
{
	struct cypress_touchkey_info *info =
		container_of(work, struct cypress_touchkey_info, update_work);

	int retry;
	int ret = 0;

	dev_info(&info->client->dev, "%s : touchkey_update Start!!\n", __func__);
	retry = NUM_OF_RETRY_UPDATE;


	if (info->irq_enable) {
		info->irq_enable = false;
		disable_irq(info->client->irq);
	}

	wake_lock(&info->fw_wakelock);

	while (retry--) {
		if(cy8cmbr_swd_program(info) == 0) {
			msleep(50);
			ret = tkey_i2c_check(info);
			if (ret < 0) {
				dev_info(&info->client->dev,
					"%s : touchkey_update pass but fw crashed.\n", __func__);
				continue;
			} else {
				dev_info(&info->client->dev,
					"%s : touchkey_update pass!!\n", __func__);
				break;
			}
		}
		msleep(50);
	}

	if (retry <= 0) {
		dev_err(&info->client->dev, "%s : touchkey_update fail\n", __func__);
		goto end_fw_update;
	}

	msleep(500);
	tkey_i2c_check(info);

 end_fw_update:

	if (!info->irq_enable) {
		enable_irq(info->client->irq);
		info->irq_enable = true;
	}

	wake_unlock(&info->fw_wakelock);


}
#endif

static int tkey_fw_update(struct cypress_touchkey_info *info, bool force)
{
	struct i2c_client *client = info->client;
	int retry;
	int ret = 0;

	dev_info(&client->dev, "%s : touchkey_update Start!!\n", __func__);
	if (info->irq_enable) {
		info->irq_enable = false;
		disable_irq(info->client->irq);
	}

	wake_lock(&info->fw_wakelock);

#ifdef CHECH_TKEY_IC_STATUS
	cancel_delayed_work(&info->status_work);
#endif

	if (force == true)
		retry = 2;
	else
		retry = NUM_OF_RETRY_UPDATE;

	while (retry--) {
		if(cy8cmbr_swd_program(info) == 0) {
			msleep(50);
			ret = tkey_i2c_check(info);
			if (ret < 0) {
				dev_info(&client->dev,
					"%s : touchkey_update pass but fw crashed.\n", __func__);
				continue;
			} else {
				dev_info(&client->dev,
					"%s : touchkey_update pass!!\n", __func__);
				break;
			}
		}
		msleep(50);
		dev_err(&client->dev,
			"%s : touchkey_update failed... retry...\n", __func__);
	}

	if (retry <= 0) {
		dev_err(&client->dev, "%s : touchkey_update fail\n", __func__);
		goto end_fw_update;
	}

	msleep(500);

	tkey_i2c_check(info);

 end_fw_update:
	wake_unlock(&info->fw_wakelock);
	if (!info->irq_enable) {
		enable_irq(info->client->irq);
		info->irq_enable = true;
	}

	info->irq_enable = true;

#ifdef CHECH_TKEY_IC_STATUS
	schedule_delayed_work(&info->status_work,
		msecs_to_jiffies(TKEY_CHECK_STATUS_TIME));
#endif

	return ret;
}
#endif

static ssize_t cypress_touchkey_ic_version_read(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int count;

	info->ic_fw_ver = i2c_smbus_read_byte_data(info->client,
			CYPRESS_FW_VER);
	dev_info(&info->client->dev,
			"%s : FW IC Ver 0x%02x\n", __func__, info->ic_fw_ver);

	count = snprintf(buf, 20, "0x%02x\n", info->ic_fw_ver);
	return count;
}

static ssize_t cypress_touchkey_src_version_read(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int count;
	dev_info(&info->client->dev,
			"%s : FW src ver 0x%02x\n", __func__, info->src_fw_ver);

	count = snprintf(buf, 20, "0x%02x\n", info->src_fw_ver);
	return count;
}

static ssize_t cypress_touchkey_firm_status_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	dev_info(&info->client->dev, "%s touchkey_update_status: %d\n",
			__func__, info->touchkey_update_status);
	if (info->touchkey_update_status == DOWNLOADING)
		return snprintf(buf, 13, "Downloading\n");
	else if (info->touchkey_update_status == UPDATE_FAIL)
		return snprintf(buf, 6, "FAIL\n");
	else if (info->touchkey_update_status == UPDATE_PASS)
		return snprintf(buf, 6, "PASS\n");
	else
		return snprintf(buf, 5, "N/A\n");
}

static ssize_t cypress_touchkey_update_write(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
#ifdef TKEY_REQUEST_FW_UPDATE
	struct i2c_client *client = info->client;	
	int count = 0;
	u8 fw_path;
	dev_info(&info->client->dev, "%s \n", __func__);

	switch (*buf) {
		case 's':
		case 'S':
			fw_path = FW_BUILT_IN;
			if(info->support_fw_update == false) {
				dev_err(&client->dev,
					"%s: IC(dev:%x, md:%x) does not support fw update\n.",
					__func__, info->device_ver, info->module_ver);
				return size;
			}
			break;

		case 'i':
		case 'I':
			fw_path = FW_IN_SDCARD;
			break;

		default:
			dev_err(&client->dev, "%s: invalid parameter %c\n.", __func__,
				*buf);
			return -EINVAL;
	}
	count = tkey_load_fw(info, fw_path);
	if (count < 0) {
		dev_err(&client->dev, "fail to load fw in %d (%d)\n",
			fw_path, count);
		return count;
	}

	info->touchkey_update_status = DOWNLOADING;

	count = tkey_fw_update(info, false);
	if (count < 0) {
		cypress_power_onoff(&info->client->dev, 0);
		info->touchkey_update_status = UPDATE_FAIL;
		dev_err(&client->dev,
			"%s: fail to flash fw (%d)\n.", __func__, count);
		return count;
	}

	info->touchkey_update_status = UPDATE_PASS;
	tkey_unload_fw(info, fw_path);
#else
	dev_err(&info->client->dev,
		"%s: Firmware update is not supported.\n",__func__);
#endif

	return size;
}

static ssize_t cypress_touchkey_led_control(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int data, ret;
	static const int ledCmd[] = {TK_CMD_LED_OFF, TK_CMD_LED_ON};

	dev_dbg(&info->client->dev, "called %s\n", __func__);

	if (wake_lock_active(&info->fw_wakelock)) {
		dev_err(&info->client->dev, "%s : wackelock active\n",
					__func__);
		return size;
	}
	
	ret = sscanf(buf, "%d", &data);

	if (ret != 1) {
		dev_err(&info->client->dev, "%s, %d err\n",
			__func__, __LINE__);
		return size;
	}

	if (data != 0 && data != 1) {
		dev_err(&info->client->dev, "%s wrong cmd %x\n",
			__func__, data);
		return size;
	}

	if (!info->enabled) {
		touchled_cmd_reversed = 1;
		goto out;
	}

	ret = i2c_smbus_write_byte_data(info->client, CYPRESS_GEN, ledCmd[data]);
	if (ret < 0) {
		dev_info(&info->client->dev,
				"%s: i2c write error [%d]\n", __func__, ret);
		touchled_cmd_reversed = 1;
		goto out;
	}


	dev_info(&info->client->dev,
		"touch_led_control : %s\n", data ? "on" : "off");

	msleep(30);

out:
	touchkey_led_status =  ledCmd[data];

	return size;
}

static ssize_t cypress_touchkey_sensitivity_control(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t size)
{
	int value, ret;
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	u8 data[2];

	sscanf(buf, "%d\n", &value);
	dev_info(&info->client->dev, "%s %d\n", __func__, value);

	if (value == 1) {
		/* Send inspection mode Command */
		data[0] = TK_BIT_CMD_INSPECTION;
		data[1] = TK_BIT_WRITE_CONFIRM;
	} else {
		/* Exit inspection mode */
		data[0] = TK_BIT_CMD_INSPECTION;
		data[1] = TK_BIT_EXIT_CONFIRM;
		ret = cypress_touchkey_i2c_write(info->client, CYPRESS_OP_MODE, data, 2);
		if (ret < 0) {
			dev_info(&info->client->dev,
				"%s: Failed to exit inspection mode command.\n",
				__func__);
		}

		/* Send Normal mode Command */
		data[0] = TK_BIT_CMD_REGULAR;
		data[1] = TK_BIT_WRITE_CONFIRM;
	}

	ret = cypress_touchkey_i2c_write(info->client, CYPRESS_OP_MODE, data, 2);
	if (ret < 0) {
		dev_info(&info->client->dev,
			"%s: Failed to write inspection mode command.\n",
			__func__);
		return 0;
	}

	ret = i2c_smbus_read_i2c_block_data(info->client,
				CYPRESS_GEN, 1, data);
	if (ret < 0) {
		dev_info(&info->client->dev,
			"[Touchkey] fail to CYPRESS_DATA_UPDATE.\n");
		return ret;
	}
	if ((data[0] & 0x20))
		dev_info(&info->client->dev,
			"[Touchkey] Control Enabled!!\n");
	else
		dev_info(&info->client->dev,
			"[Touchkey] Control Disabled!!\n");

	return size;
}

static ssize_t cypress_touchkey_menu_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 menu_sensitivity;
	u8 data[2] = { 0, };
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
			CYPRESS_DIFF_MENU, ARRAY_SIZE(data), data);
	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"[TouchKey] fail to read menu sensitivity.\n");
		return ret;
	}

	menu_sensitivity = ((0x00FF & data[0])<<8) | data[1];
	dev_info(&info->client->dev, "called %s , data : %d %d\n",
				__func__, data[0], data[1]);

	return snprintf(buf, 20, "%d\n", menu_sensitivity);
}

static ssize_t cypress_touchkey_back_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 back_sensitivity;
	u8 data[14] = { 0, };
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
			CYPRESS_DIFF_BACK, ARRAY_SIZE(data), data);
	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"[TouchKey] fail to read back sensitivity.\n");
		return ret;
	}

	back_sensitivity = ((0x00FF & data[0])<<8) | data[1];
	dev_info(&info->client->dev, "called %s , data : %d %d\n",
				__func__, data[0], data[1]);

	return snprintf(buf, 20, "%d\n", back_sensitivity);
}

static ssize_t cypress_touchkey_raw_data0_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 raw_data0;
	u8 data[2] = {0,};
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
		CYPRESS_RAW_DATA_MENU, ARRAY_SIZE(data), data);

	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"[TouchKey] fail to read MENU raw data.\n");
		return ret;
	}

	raw_data0 = ((0x00FF & data[0])<<8) | data[1];
	dev_info(&info->client->dev, "called %s , data : %d %d\n",
			__func__, data[0], data[1]);
	return snprintf(buf, 20, "%d\n", raw_data0);
}

static ssize_t cypress_touchkey_raw_data1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 raw_data1;
	u8 data[2] = {0,};
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
		CYPRESS_RAW_DATA_BACK, ARRAY_SIZE(data), data);

	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"[TouchKey] fail to read MENU_INNER raw data.\n");
		return ret;
	}

	raw_data1 = ((0x00FF & data[0])<<8) | data[1];

	dev_info(&info->client->dev, "called %s , data : %d %d\n",
			__func__, data[0], data[1]);
	return snprintf(buf, 20, "%d\n", raw_data1);
}

static ssize_t cypress_touchkey_base_data0_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 base_data0;
	u8 data[2] = {0,};
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
		CYPRESS_BASE_DATA_MENU, ARRAY_SIZE(data), data);

	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"fail to read MENU baseline data.\n");
		return ret;
	}

	base_data0 = ((0x00FF & data[0])<<8) | data[1];
	dev_info(&info->client->dev, "called %s , data : %d %d\n",
			__func__, data[0], data[1]);
	return snprintf(buf, 20, "%d\n", base_data0);
}

static ssize_t cypress_touchkey_base_data1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u16 base_data1;
	u8 data[2] = {0,};
	int ret;

	ret = i2c_smbus_read_i2c_block_data(info->client,
		CYPRESS_BASE_DATA_BACK, ARRAY_SIZE(data), data);

	if (ret != ARRAY_SIZE(data)) {
		dev_info(&info->client->dev,
			"fail to read MENU_INNER baseline data.\n");
		return ret;
	}

	base_data1 = ((0x00FF & data[0])<<8) | data[1];

	dev_info(&info->client->dev, "called %s , data : %d %d\n",
			__func__, data[0], data[1]);
	return snprintf(buf, 20, "%d\n", base_data1);
}

static ssize_t cypress_touchkey_idac0_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u8 idac0;
	u8 data = 0;

	data = i2c_smbus_read_byte_data(info->client, CYPRESS_IDAC_MENU);

	dev_info(&info->client->dev, "called %s , data : %d\n", __func__, data);
	idac0 = data;
	return snprintf(buf, 20, "%d\n", idac0);
}

static ssize_t cypress_touchkey_idac1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u8 idac1;
	u8 data = 0;

	data = i2c_smbus_read_byte_data(info->client, CYPRESS_IDAC_BACK);

	dev_info(&info->client->dev, "called %s , data : %d\n", __func__, data);
	idac1 = data;
	return snprintf(buf, 20, "%d\n", idac1);
}

static ssize_t cypress_touchkey_comp_idac0_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u8 compidac0;
	u8 data = 0;

	data = i2c_smbus_read_byte_data(info->client, CYPRESS_COMPIDAC_MENU);

	dev_info(&info->client->dev, "called %s , data : %d\n", __func__, data);
	compidac0 = data;
	return snprintf(buf, 20, "%d\n", compidac0);
}

static ssize_t cypress_touchkey_comp_idac1_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u8 compidac1;
	u8 data = 0;

	data = i2c_smbus_read_byte_data(info->client, CYPRESS_COMPIDAC_BACK);

	dev_info(&info->client->dev, "called %s , data : %d\n", __func__, data);
	compidac1 = data;
	return snprintf(buf, 20, "%d\n", compidac1);
}

static ssize_t cypress_touchkey_threshold_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	static u8 touchkey_threshold;
	u8 data = 0;

	data = i2c_smbus_read_byte_data(info->client, CYPRESS_THRESHOLD);

	dev_info(&info->client->dev, "called %s , data : %d\n", __func__, data);
	touchkey_threshold = data;
	return snprintf(buf, 20, "%d\n", touchkey_threshold);
}

static ssize_t cypress_touchkey_autocal_testmode(struct device *dev,
		struct device_attribute *attr, const char *buf,
		size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int count = 0;
	int on_off;
	if (sscanf(buf, "%d\n", &on_off) == 1) {
		dev_info(&info->client->dev, "[TouchKey] Test Mode : %d\n", on_off);
		if (on_off == 1) {
		count = i2c_smbus_write_byte_data(info->client,
			CYPRESS_GEN, CYPRESS_DATA_UPDATE);
		}
	} else {
		cypress_touchkey_auto_cal(info, false);
	}

	return count;
}

static ssize_t cypress_touchkey_autocal_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int data;

	sscanf(buf, "%d\n", &data);
	dev_dbg(&info->client->dev, "%s %d\n", __func__, data);

	if (data == 1)
		cypress_touchkey_auto_cal(info, false);
	return 0;
}

static ssize_t cypress_touchkey_autocal_status(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	u8 data[6];
	int ret;
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);

	dev_dbg(&info->client->dev, "%s\n", __func__);
	ret = i2c_smbus_read_i2c_block_data(info->client,
			CYPRESS_GEN, 6, data);
	if ((data[5] & 0x80))	/* TK_BIT_AUTOCAL */
		return snprintf(buf, 20, "Enabled\n");
	else
		return snprintf(buf, 20, "Disabled\n");

}

#ifdef CONFIG_GLOVE_TOUCH
static ssize_t cypress_touchkey_glove_mode_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int data;

	sscanf(buf, "%d\n", &data);
	dev_info(&info->client->dev, "%s %d\n", __func__, data);

	cypress_touchkey_glovemode(info, data);

	return size;
}
#endif

#ifdef TKEY_FLIP_MODE
static ssize_t cypress_touchkey_flip_cover_mode_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int data;

	sscanf(buf, "%d\n", &data);
	dev_info(&info->client->dev, "%s %d\n", __func__, data);

	cypress_touchkey_flip_cover(info, data);

	return size;
}
#endif

#ifdef TKEY_1MM_MODE
static ssize_t cypress_touchkey_1mm_mode_enable(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int data;

	sscanf(buf, "%d\n", &data);
	dev_info(&info->client->dev, "%s %d\n", __func__, data);

	cypress_touchkey_1mm_mode(info, data);

	return size;
}
#endif

#ifdef TKEY_BOOSTER
static ssize_t boost_level_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int val, retval;
	int stage;

	dev_info(&info->client->dev, "%s\n", __func__);
	sscanf(buf, "%d", &val);

	stage = 1 << val;

	if (!(info->tkey_booster->dvfs_stage & stage)) {
		dev_info(&info->client->dev,
			"%s: wrong cmd %d\n", __func__, val);
		return count;
	}

	info->tkey_booster->dvfs_boost_mode = stage;
	dev_info(&info->client->dev,
			"%s: dvfs_boost_mode = 0x%X\n",
			__func__, info->tkey_booster->dvfs_boost_mode);

	if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_DUAL) {
		info->tkey_booster->dvfs_freq = MIN_TOUCH_LIMIT_SECOND;
		dev_info(&info->client->dev,
			"%s: boost_mode DUAL, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_SINGLE) {
		info->tkey_booster->dvfs_freq = MIN_TOUCH_LIMIT;
		dev_info(&info->client->dev,
			"%s: boost_mode SINGLE, dvfs_freq = %d\n",
			__func__, info->tkey_booster->dvfs_freq);
	} else if (info->tkey_booster->dvfs_boost_mode == DVFS_STAGE_NONE) {
		retval = info->tkey_booster->dvfs_off(info->tkey_booster);
		if (retval < 0) {
			dev_err(&info->client->dev,
					"%s: booster stop failed(%d).\n",
					__func__, retval);
		}
	}
	return count;
}
#endif

static ssize_t cypress_touchkey_read_register(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int val, data = 0;

	sscanf(buf, "%x", &val);

	data = i2c_smbus_read_byte_data(info->client, val);
	if(data < 0)
		dev_err(&info->client->dev,
			"%s : failed to read 0x%02x (%d)\n",
			__func__, val ,data);

	dev_info(&info->client->dev,
		"%s: read 0x%02x reg data = 0x%02x\n",
		__func__, val, data);

	return size;
}

static ssize_t sec_keypad_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", atomic_read(&info->keypad_enable));
}

static ssize_t sec_keypad_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct cypress_touchkey_info *info = dev_get_drvdata(dev);
	int i;

	unsigned int val = 0;
	sscanf(buf, "%d", &val);
	val = (val == 0 ? 0 : 1);
	atomic_set(&info->keypad_enable, val);
	if (val) {
		for (i = 0; i < ARRAY_SIZE(info->keycode); i++)
			set_bit(info->keycode[i], info->input_dev->keybit);
	} else {
		for (i = 0; i < ARRAY_SIZE(info->keycode); i++)
			clear_bit(info->keycode[i], info->input_dev->keybit);
	}
	input_sync(info->input_dev);

	return count;
}

static DEVICE_ATTR(keypad_enable, S_IRUGO|S_IWUSR, sec_keypad_enable_show,
	      sec_keypad_enable_store);
static DEVICE_ATTR(touchkey_firm_update_status, S_IRUGO | S_IWUSR | S_IWGRP,
		cypress_touchkey_firm_status_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, S_IRUGO,
		cypress_touchkey_ic_version_read, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, S_IRUGO,
		cypress_touchkey_src_version_read, NULL);
static DEVICE_ATTR(touchkey_firm_update, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, cypress_touchkey_update_write);
static DEVICE_ATTR(brightness, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, cypress_touchkey_led_control);
static DEVICE_ATTR(touch_sensitivity, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, cypress_touchkey_sensitivity_control);

static DEVICE_ATTR(touchkey_recent, S_IRUGO,		/* RECENT diff */
		cypress_touchkey_menu_show, NULL);
static DEVICE_ATTR(touchkey_back, S_IRUGO,			/* BACK diff */
		cypress_touchkey_back_show, NULL);

static DEVICE_ATTR(touchkey_recent_raw, S_IRUGO,	/* RECENT outer raw */
		cypress_touchkey_raw_data0_show, NULL);
static DEVICE_ATTR(touchkey_back_raw, S_IRUGO,	/* BACK outer raw */
		cypress_touchkey_raw_data1_show, NULL);

static DEVICE_ATTR(touchkey_baseline_data0, S_IRUGO,	/* RECENT outer baseline */
		cypress_touchkey_base_data0_show, NULL);
static DEVICE_ATTR(touchkey_baseline_data1, S_IRUGO,	/* BACK outer baseline */
		cypress_touchkey_base_data1_show, NULL);

static DEVICE_ATTR(touchkey_recent_idac, S_IRUGO,	/* RECENT  idac */
		cypress_touchkey_idac0_show, NULL);
static DEVICE_ATTR(touchkey_back_idac, S_IRUGO,	/* BACK  idac */
		cypress_touchkey_idac1_show, NULL);

static DEVICE_ATTR(touchkey_comp_idac0, S_IRUGO,
		cypress_touchkey_comp_idac0_show, NULL);
static DEVICE_ATTR(touchkey_comp_idac1, S_IRUGO,
		cypress_touchkey_comp_idac1_show, NULL);

static DEVICE_ATTR(touchkey_threshold, S_IRUGO,	/* threshold */
		cypress_touchkey_threshold_show, NULL);

static DEVICE_ATTR(touchkey_autocal_start, S_IRUGO | S_IWUSR | S_IWGRP,
		NULL, cypress_touchkey_autocal_testmode);
static DEVICE_ATTR(autocal_enable, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		cypress_touchkey_autocal_enable);
static DEVICE_ATTR(autocal_stat, S_IRUGO | S_IWUSR | S_IWGRP,
		cypress_touchkey_autocal_status, NULL);
#ifdef CONFIG_GLOVE_TOUCH
static DEVICE_ATTR(glove_mode, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		cypress_touchkey_glove_mode_enable);
#endif

#ifdef TKEY_FLIP_MODE
static DEVICE_ATTR(flip_mode, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		cypress_touchkey_flip_cover_mode_enable);
#endif
#ifdef TKEY_1MM_MODE
static DEVICE_ATTR(1mm_mode, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		cypress_touchkey_1mm_mode_enable);
#endif
#ifdef TKEY_BOOSTER
static DEVICE_ATTR(boost_level,
		   S_IWUSR | S_IWGRP, NULL, boost_level_store);
#endif
static DEVICE_ATTR(read_reg, S_IRUGO | S_IWUSR | S_IWGRP, NULL,
		cypress_touchkey_read_register);

static struct attribute *touchkey_attributes[] = {
	&dev_attr_keypad_enable.attr,
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_update.attr,
	&dev_attr_brightness.attr,
	&dev_attr_touch_sensitivity.attr,
	&dev_attr_touchkey_recent.attr,
	&dev_attr_touchkey_back.attr,
	&dev_attr_touchkey_recent_raw.attr,
	&dev_attr_touchkey_back_raw.attr,
	&dev_attr_touchkey_baseline_data0.attr,
	&dev_attr_touchkey_baseline_data1.attr,
	&dev_attr_touchkey_recent_idac.attr,
	&dev_attr_touchkey_back_idac.attr,
	&dev_attr_touchkey_comp_idac0.attr,
	&dev_attr_touchkey_comp_idac1.attr,
	&dev_attr_touchkey_threshold.attr,
	&dev_attr_touchkey_autocal_start.attr,
	&dev_attr_autocal_enable.attr,
	&dev_attr_autocal_stat.attr,
#ifdef CONFIG_GLOVE_TOUCH
	&dev_attr_glove_mode.attr,
#endif
#ifdef TKEY_FLIP_MODE
	&dev_attr_flip_mode.attr,
#endif
#ifdef TKEY_1MM_MODE
	&dev_attr_1mm_mode.attr,
#endif
#ifdef TKEY_BOOSTER
	&dev_attr_boost_level.attr,
#endif
	&dev_attr_read_reg.attr,
	NULL,
};

static struct attribute_group touchkey_attr_group = {
	.attrs = touchkey_attributes,
};

#ifdef TKEY_REQUEST_FW_UPDATE
static int load_fw_built_in(struct cypress_touchkey_info *info)
{
	struct i2c_client *client = info->client;
	int ret;
	char *fw_name;

	fw_name = kasprintf(GFP_KERNEL, "%s/%s.fw",
			TKEY_FW_BUILTIN_PATH, TKEY_TR_CYPRESS_FW_NAME);

	dev_info(&info->client->dev, "tkey firmware name = %s\n",fw_name);
	ret = request_firmware(&info->fw, fw_name, &client->dev);
	if (ret) {
		dev_err(&info->client->dev,
			"error requesting built-in firmware (%d)\n", ret);
		goto out;
	}

	info->fw_img = (struct fw_image *)info->fw->data;
	info->src_fw_ver = info->fw_img->first_fw_ver;
	info->src_md_ver = info->fw_img->second_fw_ver;

	pr_info("%s: firmware version = 0x%02x for module 0x%02x\n",
		__func__, info->src_fw_ver, info->src_md_ver);
out:
	kfree(fw_name);
	return ret;
}

static int load_fw_in_sdcard(struct cypress_touchkey_info *info)
{
	struct i2c_client *client = info->client;
	int ret;
	mm_segment_t old_fs;
	struct file *fp;
	long nread;
	int len;
	char *fw_name;

	fw_name = kasprintf(GFP_KERNEL, "%s/%s.bin",
			TKEY_FW_IN_SDCARD_PATH, TKEY_TR_CYPRESS_FW_NAME);

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	fp = filp_open(fw_name, O_RDONLY, S_IRUSR);
	if (IS_ERR(fp)) {
		dev_err(&client->dev, "%s: fail to open fw in %s\n",
			__func__, fw_name);
		ret = -ENOENT;
		goto err_open_fw;
	}
	len = fp->f_path.dentry->d_inode->i_size;

	info->fw_img = kzalloc(len, GFP_KERNEL);
	if (!info->fw_img) {
		dev_err(&client->dev, "%s: fail to alloc mem for fw\n",
			__func__);
		ret = -EFAULT;
		goto err_alloc;
	}

	nread = vfs_read(fp, (char __user *)info->fw_img, len, &fp->f_pos);
	if (nread != len) {
		dev_err(&client->dev, "%s: fail to read fw file, nread = %lx\n",
			__func__, nread);
		ret = -EIO;
		kfree(info->fw_img);
		goto read_err;
	}
	dev_info(&client->dev, "%s: load fw in internal sd (%ld)\n",
		 __func__, nread);

	ret = 0;

read_err:
err_alloc:
	filp_close(fp, NULL);
err_open_fw:
	set_fs(old_fs);
	kfree(fw_name);
	return ret;
}

static int tkey_load_fw(struct cypress_touchkey_info *info, u8 fw_path)
{
	struct i2c_client *client = info->client;
	int ret;

	switch (fw_path) {
	case FW_BUILT_IN:
		ret = load_fw_built_in(info);
		break;

	case FW_IN_SDCARD:
		ret = load_fw_in_sdcard(info);
		break;

	default:
		dev_err(&client->dev, "%s: invalid fw path (%d)\n",
			__func__, fw_path);
		return -ENOENT;
	}
	if (ret < 0) {
		dev_err(&client->dev, "fail to load fw in %d (%d)\n",
			fw_path, ret);
		return ret;
	}
	return 0;
}

static int tkey_unload_fw(struct cypress_touchkey_info *info, u8 fw_path)
{
	struct i2c_client *client = info->client;

	switch (fw_path) {
	case FW_BUILT_IN:
		release_firmware(info->fw);
		info->fw = NULL;
		break;

	case FW_IN_SDCARD:
		kfree(info->fw_img);
		info->fw_img = NULL;
		break;

	default:
		dev_err(&client->dev, "%s: invalid fw path (%d)\n",
			__func__, fw_path);
		return -ENOENT;
	}

	return 0;
}

static int tkey_flash_fw(struct cypress_touchkey_info *info, u8 fw_path, bool force)
{
	struct i2c_client *client = info->client;
	int ret;

	/* firmware load */
	ret = tkey_load_fw(info, fw_path);
	if (ret < 0) {
		dev_err(&client->dev, "%s : fail to load fw (%d)\n", __func__, ret);
		return ret;
	}
	info->cur_fw_path = fw_path;

	/* CY device bit 6 (0x40) = MBR3155 */
	/* Do FW update after 0x06 module */
	if (force == true || \
		(info->device_ver == 0x40 && info->module_ver >= 0x06))
		info->support_fw_update = true;
	else
		info->support_fw_update = false;

	dev_info(&client->dev, "%s : [IC] fw ver:%#x, module ver:%#x\n",
			 __func__ , info->ic_fw_ver, info->module_ver);
	dev_info(&client->dev, "%s : [Binary] fw ver:%#x, supported module ver:%#x\n",
			 __func__ , info->src_fw_ver, info->src_md_ver);
#ifdef CRC_CHECK_INTERNAL
	dev_info(&client->dev, "%s : [CRC] IC:%#x, Bin:%#x\n",
			 __func__ , info->crc, info->fw_img->checksum);
#endif

	/* firmware version compare */
	if (info->ic_fw_ver == info->src_fw_ver && !force) {
#ifdef CRC_CHECK_INTERNAL
		if(info->crc != info->fw_img->checksum){
			dev_info(&client->dev,
				"%s : CRC is not matched. need to re-update\n",
				__func__);
		} else
#endif
		{
			dev_info(&client->dev, 
				"%s : IC already have latest firmware\n",
				__func__);
			goto out;
		}
	} else if (info->support_fw_update == false) {
		dev_info(&client->dev,
			"%s : IC doesn't support firmware update. dev(0x%02x), mod(0x%02x)\n",
			__func__,info->device_ver, info->module_ver);
		goto out;
	} else if (info->ic_fw_ver > info->src_fw_ver && \
			info->ic_fw_ver < info->src_fw_ver + 0x10) {
		dev_info(&client->dev,
			"%s : IC has test firmware above phone fw\n",__func__);
		goto out;
	}

#ifdef USE_TKEY_UPDATE_WORK
	queue_work(info->fw_wq, &info->update_work);
#else
	/* firmware update */
	ret = tkey_fw_update(info, force);
	if (ret < 0) {
		goto err_fw_update;
	}
err_fw_update:
#endif
out:
	tkey_unload_fw(info, fw_path);
	return ret;
}
#endif

#ifdef CHECH_TKEY_IC_STATUS
static void cypress_touchkey_check_status(struct work_struct *work)
{
	struct cypress_touchkey_info *info =
		container_of(work, struct cypress_touchkey_info, status_work.work);
	int ret;

	if (!info->enabled) {
		dev_err(&info->client->dev, "%s: disabled\n", __func__);
		info->status_count = 0;
		info->status_value = 0;
		info->status_value_old = 0;
		return;
	}

	if (wake_lock_active(&info->fw_wakelock)) {
		dev_info(&info->client->dev, "%s: wackelock active\n", __func__);
		return ;
	}

	if (info->ic_fw_ver < TKEY_STATUS_CHECK_FW_VER)
		return;

	cancel_delayed_work(&info->status_work);

	info->status_value_old = info->status_value;

	ret = cypress_touchkey_i2c_read(info->client, TK_CHECK_STATUS_REGISTER, &info->status_value, 1);
	if (ret < 0) {
		dev_err(&info->client->dev, "%s: read error, ret: %d\n", __func__, ret);
		info->status_count++;
		goto check_reset;
	}

	if ((info->status_value == info->status_value_old)) {
		dev_err(&info->client->dev, "%s: status same, %d %d\n", __func__, info->status_value, info->status_value_old);

		info->status_count++;
	} else {
		info->status_count = 0;
	}

check_reset:
	if (info->status_count >= 2) {
		dev_err(&info->client->dev, "%s: reset count: %d\n", __func__, info->status_count);

		cypress_touchkey_reset(info);

		info->status_count = 0;
		info->status_value = 0;
		info->status_value_old = 0;
	}

	schedule_delayed_work(&info->status_work,
		msecs_to_jiffies(TKEY_CHECK_STATUS_TIME));

	return;
}
#endif

#ifdef CONFIG_OF
static int cypress_get_keycodes(struct device *dev, char *name,
				struct cypress_touchkey_platform_data *pdata)
{
	struct property *prop;
	struct device_node *np = dev->of_node;
	int rc;

	prop = of_find_property(np, name, NULL);
	if (!prop)
		return -EINVAL;
	if (!prop->value)
		return -ENODATA;

	pdata->keycodes_size = prop->length / sizeof(u32);
	rc = of_property_read_u32_array(np, name, pdata->touchkey_keycode,
				pdata->keycodes_size);
	if (rc && (rc != -EINVAL)) {
		dev_info(dev, "%s: Unable to read %s\n", __func__, name);
		return rc;
	}

	return 0;
}

static int cypress_parse_dt(struct device *dev,
			struct cypress_touchkey_platform_data *pdata)
{
	int rc;
	struct device_node *np = dev->of_node;

	rc = cypress_get_keycodes(dev, "cypress,tkey-keycodes", pdata);
	if (rc)
		return rc;

	/* reset, irq gpio info */
	pdata->gpio_scl = of_get_named_gpio_flags(np, "cypress,scl-gpio",
				0, &pdata->scl_gpio_flags);
	pdata->gpio_sda = of_get_named_gpio_flags(np, "cypress,sda-gpio",
				0, &pdata->sda_gpio_flags);
	pdata->gpio_int = of_get_named_gpio_flags(np, "cypress,irq-gpio",
				0, &pdata->irq_gpio_flags);

	gpio_tlmm_config(GPIO_CFG(pdata->gpio_sda, 0,
			GPIO_CFG_INPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), 1);
	gpio_tlmm_config(GPIO_CFG(pdata->gpio_scl, 0,
			GPIO_CFG_INPUT, GPIO_CFG_PULL_DOWN, GPIO_CFG_2MA), 1);

	pr_err("%s: tkey_scl= %d, tkey_sda= %d, tkey_int= %d",
			__func__, pdata->gpio_scl, pdata->gpio_sda, pdata->gpio_int);

	return 0;
}
#else
static int cypress_parse_dt(struct device *dev,
			struct cypress_touchkey_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int cypress_touchkey_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct cypress_touchkey_platform_data *pdata;
	struct cypress_touchkey_info *info;
	struct input_dev *input_dev;
	struct device *sec_touchkey;

	int ret = 0;
	int i;
	int error;
	bool bforced = false;

	printk(KERN_ERR "%s: start!\n", __func__);
/*
	if (boot_mode_recovery == 1){
		dev_err(&client->dev, "%s: recovery mode\n", __func__);
		return -EIO;
	}
*/
/*
	if (!get_lcd_attached()){
		dev_err(&client->dev, "%s: LCD is not attached\n",__func__);
		return -EIO;
	}
*/
	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EIO;

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
			sizeof(struct cypress_touchkey_platform_data),
				GFP_KERNEL);
		if (!pdata) {
			dev_info(&client->dev, "Failed to allocate memory\n");
			return -ENOMEM;
		}

		error = cypress_parse_dt(&client->dev, pdata);
		if (error)
			return error;
	} else
		pdata = client->dev.platform_data;

	ret = gpio_request(pdata->gpio_int, "touchkey_irq");
	if (ret) {
		dev_info(&client->dev, "%s: unable to request touchkey_irq [%d]\n",
					__func__, pdata->gpio_int);
	}

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		dev_info(&client->dev, "%s: fail to memory allocation.\n", __func__);
		goto err_mem_alloc;
	}

	input_dev = input_allocate_device();
	if (!input_dev) {
		printk(KERN_ERR "%s: fail to allocate input device.\n", __func__);
		goto err_input_dev_alloc;
	}
	client->irq = gpio_to_irq(pdata->gpio_int);

	info->client = client;
	info->input_dev = input_dev;
	info->pdata = pdata;
	info->irq = client->irq;
	info->touchkey_update_status = UPDATE_NONE;
	info->power = cypress_power_onoff;

	input_dev->name = "sec_touchkey";
	input_dev->phys = info->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	set_bit(EV_SYN, input_dev->evbit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_LED, input_dev->evbit);
	set_bit(LED_MISC, input_dev->ledbit);

	atomic_set(&info->keypad_enable, 1);

	wake_lock_init(&info->fw_wakelock, WAKE_LOCK_SUSPEND, "cypress_touchkey");

	for (i = 0; i < pdata->keycodes_size; i++) {
		info->keycode[i] = pdata->touchkey_keycode[i];
		set_bit(info->keycode[i], input_dev->keybit);
	}

	i2c_set_clientdata(client, info);

	info->is_powering_on = true;
	cypress_power_onoff(&info->client->dev, 1);
/** regulator enabled in pmic init
//	msleep(150);
*/

	info->enabled = true;
	ret = tkey_i2c_check(info);
	if (ret < 0) {
		dev_err(&client->dev, "i2c_check failed\n");
		bforced = true;
	}

	input_set_drvdata(input_dev, info);

	ret = input_register_device(input_dev);
	if (ret) {
		dev_info(&client->dev, "failed to register input dev (%d).\n",
			ret);
		goto err_reg_input_dev;
	}

#ifdef USE_OPEN_CLOSE
	input_dev->open = cypress_input_open;
	input_dev->close = cypress_input_close;
#endif
	dev_info(&info->client->dev, "gpio_to_irq IRQ %d\n",
			client->irq);

#ifdef TKEY_BOOSTER
	info->tkey_booster = kzalloc(sizeof(struct input_booster), GFP_KERNEL);
	if (!info->tkey_booster) {
		dev_err(&client->dev,
			"%s: Failed to alloc mem for tsp_booster\n", __func__);
		goto err_get_tkey_booster;
	} else {
		input_booster_init_dvfs(info->tkey_booster, INPUT_BOOSTER_ID_TKEY);
	}
#endif

	ret = request_threaded_irq(client->irq, NULL,
			cypress_touchkey_interrupt,
			IRQF_DISABLED | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			client->dev.driver->name, info);
	if (ret < 0) {
		dev_info(&client->dev, "Failed to request IRQ %d (err: %d).\n",
				client->irq, ret);
		goto err_req_irq;
	}

	info->irq_enable = true;

#ifdef CONFIG_GLOVE_TOUCH
	mutex_init(&info->tkey_glove_lock);
	INIT_DELAYED_WORK(&info->glove_work, cypress_touchkey_glove_work);
#endif

#ifdef CHECH_TKEY_IC_STATUS
	INIT_DELAYED_WORK(&info->status_work, cypress_touchkey_check_status);
#endif

#ifdef TKEY_REQUEST_FW_UPDATE
#ifdef USE_TKEY_UPDATE_WORK
	info->fw_wq = create_singlethread_workqueue("cypress_touchkey");
	if (!info->fw_wq) {
		dev_err(&client->dev, "fail to create workqueue for fw_wq\n");
		goto err_create_fw_wq;
	} else {
		INIT_WORK(&info->update_work, tkey_fw_update_work);
	}
#endif
	ret = tkey_flash_fw(info, FW_BUILT_IN, bforced);
  	if (ret < 0) {
		dev_info(&info->client->dev,
			"%s: tkey fw update failed.\n", __func__);
		goto err_fw_update;
	}
#else
	info->src_fw_ver = BIN_FW_VERSION;
#endif

	sec_touchkey = device_create(sec_class, NULL, 0, NULL, "sec_touchkey");
	if (IS_ERR(sec_touchkey)) {
		dev_info(&info->client->dev,
				"%s: Failed to create device(sec_touchkey)!\n", __func__);
		goto err_sysfs;
	}

	ret = sysfs_create_link(&sec_touchkey->kobj, &input_dev->dev.kobj, "input");
	if (ret < 0) {
		dev_err(&info->client->dev,
				"%s: Failed to create input symbolic link\n",
				__func__);
	}

	dev_set_drvdata(sec_touchkey, info);

	ret = sysfs_create_group(&sec_touchkey->kobj, &touchkey_attr_group);
	if (ret < 0) {
		dev_err(&info->client->dev,
				"%s: Failed to create sysfs attributes\n",
				__func__);
		goto err_sysfs_group;
	}

	info->is_powering_on = false;
	info->init_done = true;

#ifdef CHECH_TKEY_IC_STATUS
	schedule_delayed_work(&info->status_work,
		msecs_to_jiffies(TKEY_CHECK_STATUS_TIME));
#endif

	dev_info(&info->client->dev, "%s: done\n", __func__);
	return 0;

err_sysfs_group:
	device_destroy(sec_class,  0);
err_sysfs:
#ifdef TKEY_REQUEST_FW_UPDATE
err_fw_update:
#ifdef USE_TKEY_UPDATE_WORK
	destroy_workqueue(info->fw_wq);
err_create_fw_wq:
#endif
#endif
#ifdef CONFIG_GLOVE_TOUCH
	mutex_destroy(&info->tkey_glove_lock);
#endif
	if (info->irq >= 0)
		free_irq(info->irq, info);
err_req_irq:
#ifdef TKEY_BOOSTER
	kfree(info->tkey_booster);
err_get_tkey_booster:
#endif
	input_unregister_device(input_dev);
err_reg_input_dev:
	input_free_device(input_dev);
	input_dev = NULL;
	wake_lock_destroy(&info->fw_wakelock);
	cypress_power_onoff(&info->client->dev, 0);
err_input_dev_alloc:
	kfree(info);
	return -ENXIO;
err_mem_alloc:
	return -ENOMEM;
}

static int cypress_touchkey_remove(struct i2c_client *client)
{
	struct cypress_touchkey_info *info = i2c_get_clientdata(client);
	if (info->irq >= 0)
		free_irq(info->irq, info);
	input_unregister_device(info->input_dev);
	input_free_device(info->input_dev);
#ifdef TKEY_BOOSTER
	kfree(info->tkey_booster);
#endif
	kfree(info);
	return 0;
}

#ifdef USE_OPEN_CLOSE
static void cypress_input_close(struct input_dev *dev)
{
	struct cypress_touchkey_info *info = input_get_drvdata(dev);

	dev_info(&info->client->dev, "%s\n",__func__);

#ifdef CHECH_TKEY_IC_STATUS
	cancel_delayed_work(&info->status_work);
#endif

	if (wake_lock_active(&info->fw_wakelock))
		dev_err(&info->client->dev, "wackelock active(%s)\n",
					__func__);

	if (!info->enabled)
		dev_err(&info->client->dev, " already power off(%s)\n",
					__func__);

#ifdef TKEY_BOOSTER
	if (info->tkey_booster->dvfs_set)
		info->tkey_booster->dvfs_set(info->tkey_booster, 2);
	dev_info(&info->client->dev,
			"%s: dvfs_lock free.\n", __func__);
#endif
	info->is_powering_on = true;
	if (info->irq_enable) {
		info->irq_enable = false;
		disable_irq(info->client->irq);
	}
	info->enabled = false;
	cypress_power_onoff(&info->client->dev, 0);
}

static int cypress_input_open(struct input_dev *dev)
{
	struct cypress_touchkey_info *info = input_get_drvdata(dev);
	int ret = 0;

	dev_info(&info->client->dev, "%s\n",__func__);
	if (wake_lock_active(&info->fw_wakelock)) {
		dev_err(&info->client->dev, "wackelock active(%s)\n",
					__func__);
		return 0;
	}

	if (info->enabled) {
		dev_err(&info->client->dev, " already power on(%s)\n",
					__func__);
		return 0;
	}

	cypress_power_onoff(&info->client->dev, 1);
	msleep(150);

	/* CYPRESS Firmware setting interrupt type : dual or single interrupt */
	cypress_touchkey_interrupt_set_dual(info->client);

	info->enabled = true;

	if (touchled_cmd_reversed) {
		touchled_cmd_reversed = 0;
		ret = i2c_smbus_write_byte_data(info->client,
				CYPRESS_GEN, touchkey_led_status);
		if (ret < 0) {
			dev_err(&info->client->dev,
					"%s: i2c write error [%d]\n", __func__, ret);
			goto out;
		}
		dev_info(&info->client->dev,
				"%s: LED returned on\n", __func__);
		msleep(30);
	}
out:
	if (!info->irq_enable) {
		enable_irq(info->client->irq);
		info->irq_enable = true;
	}

	info->is_powering_on = false;

#ifdef CHECH_TKEY_IC_STATUS
	info->status_count = 0;
	info->status_value = 0;
	info->status_value_old = 0;

	schedule_delayed_work(&info->status_work,
		msecs_to_jiffies(TKEY_CHECK_STATUS_TIME));
#endif

	return ret;
}
#endif

static const struct i2c_device_id cypress_touchkey_id[] = {
	{"cypress_touchkey", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, cypress_touchkey_id);

#ifdef CONFIG_OF
static struct of_device_id cypress_match_table[] = {
	{ .compatible = "cypress,cypress-tkey",},
	{ },
};
#else
#define cypress_match_table	NULL
#endif

struct i2c_driver cypress_touchkey_driver = {
	.probe = cypress_touchkey_probe,
	.remove = cypress_touchkey_remove,
	.driver = {
		.name = "cypress_touchkey",
		.owner = THIS_MODULE,
		.of_match_table = cypress_match_table,
		   },
	.id_table = cypress_touchkey_id,
};

static int __init cypress_touchkey_init(void)
{

	int ret = 0;

#ifdef CONFIG_SAMSUNG_LPM_MODE
	if (poweroff_charging) {
		pr_notice("%s : LPM Charging Mode!!\n", __func__);
		return 0;
	}
#endif

	ret = i2c_add_driver(&cypress_touchkey_driver);
	if (ret) {
		printk(KERN_ERR "cypress touch keypad registration failed. ret= %d\n",
			ret);
	}
	printk(KERN_ERR "%s: init done %d\n", __func__, ret);

	return ret;
}

static void __exit cypress_touchkey_exit(void)
{
	i2c_del_driver(&cypress_touchkey_driver);
}

module_init(cypress_touchkey_init);
//deferred_initcall(cypress_touchkey_init);
module_exit(cypress_touchkey_exit);

MODULE_DESCRIPTION("Touchkey driver for Cypress touchkey controller ");
MODULE_LICENSE("GPL");
