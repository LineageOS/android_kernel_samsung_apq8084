/* Copyright (c) 2011-2014, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt) "%s:%d " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/of_gpio.h>
#include "msm_sd.h"
#include "msm_actuator.h"
#include "msm_cci.h"
#include "msm_camera_io_util.h"

DEFINE_MSM_MUTEX(msm_actuator_mutex);
//#define MSM_ACUTUATOR_DEBUG
#undef CDBG
#ifdef MSM_ACUTUATOR_DEBUG
#define CDBG(fmt, args...) pr_err(fmt, ##args)
#else
#define CDBG(fmt, args...) pr_debug(fmt, ##args)
#endif

extern int remove_af_noise_register(struct remove_af_noise *af_sensor_interface);

static int32_t msm_actuator_power_up(struct msm_actuator_ctrl_t *a_ctrl);
static int32_t msm_actuator_power_down(struct msm_actuator_ctrl_t *a_ctrl);

#if defined(CONFIG_OIS)
struct msm_actuator_ctrl_t *g_msm_actuator_t;
#endif

static struct msm_actuator msm_vcm_actuator_table;
static struct msm_actuator msm_piezo_actuator_table;
static struct i2c_driver msm_actuator_i2c_driver;
static struct msm_actuator msm_hall_effect_actuator_table;
static struct msm_actuator msm_hvcm_actuator_table;
static uint16_t hvca_inf_position, hvca_mac_position, torg;

static struct msm_actuator *actuators[] = {
	&msm_vcm_actuator_table,
	&msm_piezo_actuator_table,
	&msm_hall_effect_actuator_table,
	&msm_hvcm_actuator_table,
};

#if defined(CONFIG_OIS)
/***** for only ois selftest , set the actuator initial position to 256 *****/
int16_t msm_actuator_move_for_ois_test(void)
{
	int rc = 0;
	pr_err("[%s::%d] : E \n", __func__, __LINE__);

	g_msm_actuator_t->i2c_data_type = MSM_ACTUATOR_BYTE_DATA;
	g_msm_actuator_t->i2c_client.addr_type = MSM_ACTUATOR_BYTE_ADDR;

	rc = g_msm_actuator_t->i2c_client.i2c_func_tbl->i2c_write(
		&g_msm_actuator_t->i2c_client, 0x00, 0x80,		/* SET Position MSB - 0x00 */
		MSM_CAMERA_I2C_BYTE_DATA);

	if (rc < 0)
		pr_err("[%s:%d] i2c failed rc :: %d\n", __func__, __LINE__,rc);

	rc = g_msm_actuator_t->i2c_client.i2c_func_tbl->i2c_write(
		&g_msm_actuator_t->i2c_client, 0x01, 0x00,		/* SET Position LSB - 0x00 */
		MSM_CAMERA_I2C_BYTE_DATA);

	if (rc < 0)
		pr_err("[%s:%d] i2c failed rc :: %d\n", __func__, __LINE__,rc);

	rc = g_msm_actuator_t->i2c_client.i2c_func_tbl->i2c_write(
		&g_msm_actuator_t->i2c_client, 0x02, 0x00,		/* SET Active Mode */
		MSM_CAMERA_I2C_BYTE_DATA);

	if (rc < 0)
		pr_err("[%s:%d] i2c failed rc :: %d\n", __func__, __LINE__,rc);

	pr_err("[%s::%d] : X \n", __func__, __LINE__);
	return rc;
}
#endif

static int16_t msm_actuator_remove_noise(void *pdata, bool bOnOff)
{
	int rc = 0;
	struct msm_actuator_ctrl_t * a_ctrl = (struct msm_actuator_ctrl_t *)pdata;
	pr_info("[%s::%d][E][bOnOff::%d]\n", __func__, __LINE__, bOnOff);

	/* For Camera running Check */
	if (a_ctrl->is_camera_run == TRUE) {
		pr_err("[%s::%d][X][CAMERA RUNNING!!! - fn EXIT]\n", __func__, __LINE__);
		return rc;
	}

	a_ctrl->i2c_data_type = MSM_ACTUATOR_BYTE_DATA;
	a_ctrl->i2c_client.addr_type = MSM_ACTUATOR_BYTE_ADDR;

	if (bOnOff) {
		rc = msm_actuator_power_up(a_ctrl);
		if (rc < 0) {
			pr_err("[%s:%d][X] msm_actuator_power_up failed rc :: %d\n", __func__, __LINE__,rc);
			return rc;
		}

		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, 0x02, 0x00,		/* SET Active Mode */
			MSM_CAMERA_I2C_BYTE_DATA);
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, 0x00, 0x00,		/* SET Position MSB - 0x00 */
			MSM_CAMERA_I2C_BYTE_DATA);
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, 0x01, 0x00,		/* SET Position LSB - 0x00 */
			MSM_CAMERA_I2C_BYTE_DATA);
		if (rc < 0) {
			pr_err("[%s:%d] i2c failed rc :: %d\n", __func__, __LINE__,rc);

			rc = msm_actuator_power_down(a_ctrl);
			if (rc < 0)
				pr_err("[%s:%d] msm_actuator_power_down failed rc :: %d\n", __func__, __LINE__,rc);
		}
	} else {
		rc = msm_actuator_power_down(a_ctrl);
		if (rc < 0)
			pr_err("[%s:%d] msm_actuator_power_down failed rc :: %d\n", __func__, __LINE__,rc);
	}

	pr_info("[%s::%d][X][bOnOff::%d]\n", __func__, __LINE__, bOnOff);
	return rc;
}

static int32_t msm_actuator_piezo_set_default_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	struct msm_camera_i2c_reg_setting reg_setting;
	CDBG("Enter\n");

	if (a_ctrl->curr_step_pos != 0) {
		a_ctrl->i2c_tbl_index = 0;
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			a_ctrl->initial_code, 0, 0);
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			a_ctrl->initial_code, 0, 0);
		reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
		reg_setting.data_type = a_ctrl->i2c_data_type;
		reg_setting.size = a_ctrl->i2c_tbl_index;
		rc = a_ctrl->i2c_client.i2c_func_tbl->
			i2c_write_table_w_microdelay(
			&a_ctrl->i2c_client, &reg_setting);
		if (rc < 0) {
			pr_err("[%s::%d] i2c write error:%d\n",
				__func__, __LINE__, rc);
			return rc;
		}
		a_ctrl->i2c_tbl_index = 0;
		a_ctrl->curr_step_pos = 0;
	}
	CDBG("Exit\n");
	return rc;
}

static void msm_actuator_parse_i2c_params(struct msm_actuator_ctrl_t *a_ctrl,
	int16_t next_lens_position, uint32_t hw_params, uint16_t delay)
{
	struct msm_actuator_reg_params_t *write_arr = a_ctrl->reg_tbl;
	uint32_t hw_dword = hw_params;
	uint16_t i2c_byte1 = 0, i2c_byte2 = 0;
	uint16_t value = 0;
	uint32_t size = a_ctrl->reg_tbl_size, i = 0;
	struct msm_camera_i2c_reg_array *i2c_tbl = a_ctrl->i2c_reg_tbl;
	CDBG("Enter\n");
	for (i = 0; i < size; i++) {
		if (write_arr[i].reg_write_type == MSM_ACTUATOR_WRITE_DAC) {
			value = (next_lens_position <<
				write_arr[i].data_shift) |
				((hw_dword & write_arr[i].hw_mask) >>
				write_arr[i].hw_shift);

			if (write_arr[i].reg_addr != 0xFFFF) {
				i2c_byte1 = write_arr[i].reg_addr;
				i2c_byte2 = value;
				if (size != (i+1)) {
					i2c_byte2 = value & 0xFF;
					CDBG("byte1:0x%x, byte2:0x%x\n",
						i2c_byte1, i2c_byte2);
					if (a_ctrl->i2c_tbl_index >
						a_ctrl->total_steps) {
						pr_err("failed:i2c table index out of bound\n");
						break;
					}
					i2c_tbl[a_ctrl->i2c_tbl_index].
						reg_addr = i2c_byte1;
					i2c_tbl[a_ctrl->i2c_tbl_index].
						reg_data = i2c_byte2;
					i2c_tbl[a_ctrl->i2c_tbl_index].
						delay = 0;
					a_ctrl->i2c_tbl_index++;
					i++;
					i2c_byte1 = write_arr[i].reg_addr;
					i2c_byte2 = (value & 0xFF00) >> 8;
				}
			} else {
				i2c_byte1 = (value & 0xFF00) >> 8;
				i2c_byte2 = value & 0xFF;
			}
		} else {
			i2c_byte1 = write_arr[i].reg_addr;
			i2c_byte2 = (hw_dword & write_arr[i].hw_mask) >>
				write_arr[i].hw_shift;
		}
		if (a_ctrl->i2c_tbl_index > a_ctrl->total_steps) {
			pr_err("failed: i2c table index out of bound\n");
			break;
		}
		CDBG("i2c_byte1:0x%x, i2c_byte2:0x%x\n", i2c_byte1, i2c_byte2);
		i2c_tbl[a_ctrl->i2c_tbl_index].reg_addr = i2c_byte1;
		i2c_tbl[a_ctrl->i2c_tbl_index].reg_data = i2c_byte2;
		i2c_tbl[a_ctrl->i2c_tbl_index].delay = delay;
		a_ctrl->i2c_tbl_index++;
	}
	CDBG("Exit\n");
}

static int32_t msm_actuator_init_focus(struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t size, struct reg_settings_t *settings)
{
	int32_t rc = -EFAULT;
	int32_t i = 0;
	enum msm_camera_i2c_reg_addr_type save_addr_type;
	CDBG("Enter\n");

	save_addr_type = a_ctrl->i2c_client.addr_type;
	for (i = 0; i < size; i++) {
		switch (settings[i].addr_type) {
		case MSM_ACTUATOR_BYTE_ADDR:
			a_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_BYTE_ADDR;
			break;
		case MSM_ACTUATOR_WORD_ADDR:
			a_ctrl->i2c_client.addr_type = MSM_CAMERA_I2C_WORD_ADDR;
			break;
		default:
			pr_err("[%s::%d] Unsupport addr type: %d\n",
				__func__, __LINE__, settings[i].addr_type);
			break;
		}

		switch (settings[i].i2c_operation) {
		case MSM_ACT_WRITE:
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
				&a_ctrl->i2c_client,
				settings[i].reg_addr,
				settings[i].reg_data,
				settings[i].data_type);
			break;
		case MSM_ACT_POLL:
			rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_poll(
				&a_ctrl->i2c_client,
				settings[i].reg_addr,
				settings[i].reg_data,
				settings[i].data_type);
			break;
		default:
			pr_err("[%s::%d] Unsupport i2c_operation: %d\n",
				__func__, __LINE__, settings[i].i2c_operation);
			break;

		if (0 != settings[i].delay)
			msleep(settings[i].delay);

		if (rc < 0)
			break;
		}
	}

	a_ctrl->curr_step_pos = 0;
	/* recover register addr_type after the init
	settings are written  */
	a_ctrl->i2c_client.addr_type = save_addr_type;
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_hall_effect_init_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t size,
	struct reg_settings_t *settings)
{
	int32_t rc = 0;

	CDBG("%s [QTI] Enter %d\n", __func__, __LINE__);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x02, 0x00,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}
	usleep_range(1000, 1100);

	#if 0 ///////////////////////////////////////////////////////////////////////////////////
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x08, 0x00,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}
	usleep_range(1000, 1100);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x02, 0x01,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}
	msleep(200);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x03, 0x04,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}
	msleep(50);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
		&a_ctrl->i2c_client, 0x02, 0x00,
		MSM_CAMERA_I2C_BYTE_DATA);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}
	usleep_range(1000, 1100);

	#endif ///////////////////////////////////////////////////////////////////////////////////

	CDBG("%s exit Line %d\n", __func__, __LINE__);
	return rc;
}

static int32_t msm_actuator_hvcm_init_focus(struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t size,
	struct reg_settings_t *settings)
{
	int32_t rc = -EFAULT;
	uint16_t posmsb, poslsb;
	uint8_t temp;

	CDBG("Enter\n");
	usleep_range(1000, 1100);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x00,
	   0x08, MSM_CAMERA_I2C_BYTE_DATA);
	// software reset, (OpMode_Nor | StCal_Off | TstMd_Off | NdPrbMd_Off | SWreset_On | AngBlg_Off | HalBias_Off | Output_Off)
	usleep_range(6000, 6100);
	CDBG("%s %d\n", __func__, __LINE__);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x03,
	   0x00, MSM_CAMERA_I2C_BYTE_DATA);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x00,
	   0x06, MSM_CAMERA_I2C_BYTE_DATA);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x2A,
	   &torg, MSM_CAMERA_I2C_BYTE_DATA);

	CDBG("%s %d torg=%d\n", __func__, __LINE__, torg);
	temp = 0x00 | (0x0F & torg);

	CDBG("%s %d temp=%d\n", __func__, __LINE__, temp);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x0A,
	   temp, MSM_CAMERA_I2C_BYTE_DATA);


	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x04,
	   &poslsb, MSM_CAMERA_I2C_BYTE_DATA);

	CDBG("%s %d poslsb=%d\n", __func__, __LINE__, poslsb);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x05,
	   &posmsb, MSM_CAMERA_I2C_BYTE_DATA);

	posmsb = posmsb & 0x03;
	CDBG("%s %d posmsb=%d\n", __func__, __LINE__, posmsb);
	//write position
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x01,
	   poslsb, MSM_CAMERA_I2C_BYTE_DATA);


	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x02,
	   posmsb, MSM_CAMERA_I2C_BYTE_DATA);

	CDBG("%s %d position=%d\n", __func__, __LINE__, ((posmsb<<8)|poslsb));
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x00,
	   0x07, MSM_CAMERA_I2C_BYTE_DATA);

	usleep_range(10000, 10100);
	temp = 0x20 | (0x0F & torg);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
	   &a_ctrl->i2c_client,
	   0x0A,
	   temp, MSM_CAMERA_I2C_BYTE_DATA);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x10,
	   &poslsb, MSM_CAMERA_I2C_BYTE_DATA);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x11,
	   &posmsb, MSM_CAMERA_I2C_BYTE_DATA);
	posmsb = posmsb & 0x03;
	hvca_inf_position = (posmsb << 8) | poslsb;
	pr_info("%s %d hvca_inf_position=%d\n", __func__, __LINE__, hvca_inf_position);
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x12,
	   &poslsb, MSM_CAMERA_I2C_BYTE_DATA);

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_read(
	   &a_ctrl->i2c_client,
	   0x13,
	   &posmsb, MSM_CAMERA_I2C_BYTE_DATA);
	posmsb = posmsb & 0x03;
	hvca_mac_position = (posmsb << 8) | poslsb;
	pr_info("%s %d hvca_mac_position=%d\n", __func__, __LINE__, hvca_mac_position);
	a_ctrl->curr_step_pos = 0;
	CDBG("Exit\n");
	return rc;
}

static void msm_actuator_write_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	uint16_t curr_lens_pos,
	struct damping_params_t *damping_params,
	int8_t sign_direction,
	int16_t code_boundary)
{
	int16_t next_lens_pos = 0;
	uint16_t damping_code_step = 0;
	uint16_t wait_time = 0;
	CDBG("Enter\n");

	damping_code_step = damping_params->damping_step;
	wait_time = damping_params->damping_delay;
	if (sign_direction == 1)
	   sign_direction = -1;
	else if (sign_direction == -1)
	   sign_direction = 1;

	/* Write code based on damping_code_step in a loop */
	for (next_lens_pos =
		curr_lens_pos + (sign_direction * damping_code_step);
		(sign_direction * next_lens_pos) <=
			(sign_direction * code_boundary);
		next_lens_pos =
			(next_lens_pos +
				(sign_direction * damping_code_step))) {
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			next_lens_pos, damping_params->hw_params, wait_time);
		curr_lens_pos = next_lens_pos;
	}

	if (curr_lens_pos != code_boundary) {
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
			code_boundary, damping_params->hw_params, wait_time);
	}
	CDBG("Exit\n");
}

static int32_t msm_actuator_piezo_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t dest_step_position = move_params->dest_step_pos;
	struct damping_params_t ringing_params_kernel;
	int32_t rc = 0;
	int32_t num_steps = move_params->num_steps;
	struct msm_camera_i2c_reg_setting reg_setting;
	CDBG("Enter\n");

	if (copy_from_user(&ringing_params_kernel,
		&(move_params->ringing_params[0]),
		sizeof(struct damping_params_t))) {
		pr_err("[%s::%d] copy_from_user failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (num_steps == 0)
		return rc;

	a_ctrl->i2c_tbl_index = 0;
	a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
		(num_steps *
		a_ctrl->region_params[0].code_per_step),
		ringing_params_kernel.hw_params, 0);

	reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
	reg_setting.data_type = a_ctrl->i2c_data_type;
	reg_setting.size = a_ctrl->i2c_tbl_index;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_table_w_microdelay(
		&a_ctrl->i2c_client, &reg_setting);
	if (rc < 0) {
		pr_err("[%s::%d] i2c write error:%d\n", __func__, __LINE__, rc);
		return rc;
	}
	a_ctrl->i2c_tbl_index = 0;
	a_ctrl->curr_step_pos = dest_step_position;
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_hall_effect_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	uint16_t wait_time = 0;
	int32_t dest_step_position = move_params->dest_step_pos;
	struct damping_params_t *damping_params =
		&move_params->ringing_params[0];

	uint16_t curr_lens_pos =
		a_ctrl->step_position_table[a_ctrl->curr_step_pos];
	int16_t target_lens_pos =
		a_ctrl->step_position_table[dest_step_position];
	uint8_t lsb;
	uint8_t msb;
	CDBG("%s Enter %d\n", __func__, __LINE__);
	wait_time = damping_params->damping_delay;

	/* NULL CHECK */
	if (a_ctrl->step_position_table == NULL ) {
		pr_err("[%s::%d] NULL pointer check fail !\n", __func__, __LINE__);
		return -EFAULT;
	}

	CDBG("%s curr_lens_pos=%d dest_lens_pos=%d\n", __func__,
		curr_lens_pos, target_lens_pos);
	CDBG("%s curr_step_pos=%d dest_step_position=%d\n", __func__,
		a_ctrl->curr_step_pos, dest_step_position);

	if (curr_lens_pos != target_lens_pos) {
		msb = ((target_lens_pos << 7) & 0x80);
		lsb = target_lens_pos >> 1;

		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, 0x00, lsb,
			MSM_CAMERA_I2C_BYTE_DATA);
		if (rc < 0) {
			pr_err("%s Failed I2C write Line %d\n", __func__,
				__LINE__);
			return rc;
		}

		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write(
			&a_ctrl->i2c_client, 0x01, msb,
			MSM_CAMERA_I2C_BYTE_DATA);
		if (rc < 0) {
			pr_err("%s Failed I2C write Line %d\n", __func__,
				__LINE__);
			return rc;
		}
		usleep_range(wait_time, wait_time + 1000);
	}

	a_ctrl->curr_step_pos = dest_step_position;
	CDBG("%s exit %d\n", __func__, __LINE__);
	return rc;
}

static int32_t msm_actuator_move_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	struct damping_params_t ringing_params_kernel;
	int8_t sign_dir = move_params->sign_dir;
	uint16_t step_boundary = 0;
	uint16_t target_step_pos = 0;
	uint16_t target_lens_pos = 0;
	int16_t dest_step_pos = move_params->dest_step_pos;
	uint16_t curr_lens_pos = 0;
	int dir = move_params->dir;
	int32_t num_steps = move_params->num_steps;
	struct msm_camera_i2c_reg_setting reg_setting;

	/* NULL CHECK */
	if (a_ctrl->step_position_table == NULL ) {
		pr_err("[%s::%d] NULL pointer check fail !\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (copy_from_user(&ringing_params_kernel,
		&(move_params->ringing_params[a_ctrl->curr_region_index]),
		sizeof(struct damping_params_t))) {
		pr_err("[%s::%d]copy_from_user failed\n", __func__, __LINE__);
		return -EFAULT;
	}


	CDBG("called, dir %d, num_steps %d\n", dir, num_steps);

	if (dest_step_pos == a_ctrl->curr_step_pos)
		return rc;

	if ((sign_dir > MSM_ACTUATOR_MOVE_SIGNED_NEAR) ||
		(sign_dir < MSM_ACTUATOR_MOVE_SIGNED_FAR)) {
		pr_err("[%s::%d]Invalid sign_dir = %d\n", __func__, __LINE__, sign_dir);
		return -EFAULT;
	}
	if ((dir > MOVE_FAR) || (dir < MOVE_NEAR)) {
		pr_err("[%s::%d]Invalid direction = %d\n", __func__, __LINE__, dir);
		return -EFAULT;
	}
	if (dest_step_pos > a_ctrl->total_steps) {
		pr_err("[%s::%d] Step pos greater than total steps = %d\n",
		__func__, __LINE__,dest_step_pos);
		return -EFAULT;
	}
	curr_lens_pos = a_ctrl->step_position_table[a_ctrl->curr_step_pos];
	a_ctrl->i2c_tbl_index = 0;
	CDBG("curr_step_pos =%d dest_step_pos =%d curr_lens_pos=%d\n",
		a_ctrl->curr_step_pos, dest_step_pos, curr_lens_pos);

	while (a_ctrl->curr_step_pos != dest_step_pos) {
		step_boundary =
			a_ctrl->region_params[a_ctrl->curr_region_index].
			step_bound[dir];
		if ((dest_step_pos * sign_dir) <=
			(step_boundary * sign_dir)) {

			target_step_pos = dest_step_pos;
			target_lens_pos =
				a_ctrl->step_position_table[target_step_pos];
			a_ctrl->func_tbl->actuator_write_focus(a_ctrl,
					curr_lens_pos,
					&ringing_params_kernel,
					sign_dir,
					target_lens_pos);
			curr_lens_pos = target_lens_pos;

		} else {
			target_step_pos = step_boundary;
			target_lens_pos =
				a_ctrl->step_position_table[target_step_pos];
			a_ctrl->func_tbl->actuator_write_focus(a_ctrl,
					curr_lens_pos,
					&ringing_params_kernel,
					sign_dir,
					target_lens_pos);
			curr_lens_pos = target_lens_pos;

			a_ctrl->curr_region_index += sign_dir;
		}
		a_ctrl->curr_step_pos = target_step_pos;
	}

	move_params->curr_lens_pos = curr_lens_pos;
	reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
	reg_setting.data_type = a_ctrl->i2c_data_type;
	reg_setting.size = a_ctrl->i2c_tbl_index;
	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_table_w_microdelay(
		&a_ctrl->i2c_client, &reg_setting);
	if (rc < 0) {
		pr_err("[%s::%d]i2c write error:%d\n", __func__, __LINE__, rc);
		return rc;
	}
	a_ctrl->i2c_tbl_index = 0;
	CDBG("Exit\n");

	return rc;
}

static int32_t msm_actuator_init_step_table(struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_info_t *set_info)
{
	int16_t code_per_step = 0;
	int16_t cur_code = 0;
	uint16_t step_index = 0, region_index = 0;
	uint16_t step_boundary = 0;
	uint32_t max_code_size = 1;
	uint16_t data_size = set_info->actuator_params.data_size;
	CDBG("Enter\n");

	for (; data_size > 0; data_size--)
		max_code_size *= 2;

	/* free the step_position_table to allocate a new one */
	if (a_ctrl->step_position_table) {
		kfree(a_ctrl->step_position_table);
		a_ctrl->step_position_table = NULL;
	}

	if (set_info->af_tuning_params.total_steps
		>  MAX_ACTUATOR_AF_TOTAL_STEPS) {
		pr_err("[%s::%d] Max actuator totalsteps exceeded = %d\n",
		__func__, __LINE__, set_info->af_tuning_params.total_steps);
		return -EFAULT;
	}
	/* Fill step position table */
	a_ctrl->step_position_table =
		kmalloc(sizeof(uint16_t) *
		(set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);
	if (a_ctrl->step_position_table == NULL) {
		pr_err("[%s::%d] mem alloc fail !\n", __func__, __LINE__);
		return -ENOMEM;
	}

	cur_code = set_info->af_tuning_params.initial_code;
	a_ctrl->step_position_table[step_index++] = cur_code;
	for (region_index = 0;
		region_index < a_ctrl->region_size;
		region_index++) {
		code_per_step =
			a_ctrl->region_params[region_index].code_per_step;
		step_boundary =
			a_ctrl->region_params[region_index].
			step_bound[MOVE_NEAR];
		if (step_boundary >
			set_info->af_tuning_params.total_steps) {
			pr_err("invalid step_boundary = %d, max_val = %d",
				step_boundary,
				set_info->af_tuning_params.total_steps);
			kfree(a_ctrl->step_position_table);
			a_ctrl->step_position_table = NULL;
			return -EINVAL;
		}
		for (; step_index <= step_boundary;
			step_index++) {
			cur_code += code_per_step;
			if (cur_code < max_code_size)
				a_ctrl->step_position_table[step_index] =
					cur_code;
			else {
				for (; step_index <
					set_info->af_tuning_params.total_steps;
					step_index++)
					a_ctrl->
						step_position_table[
						step_index] =
						max_code_size;
			}
		}
	}
	CDBG("Exit\n");
	return 0;
}

static int32_t msm_actuator_hvcm_init_step_table(struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_info_t *set_info)
{
	int16_t code_per_step = 0;
	int16_t cur_code = 0;
	uint16_t step_index = 0, region_index = 0;
	uint16_t step_boundary = 0;
	uint32_t max_code_size = 1;
	uint16_t data_size = set_info->actuator_params.data_size;
	CDBG("Enter\n");

	for (; data_size > 0; data_size--)
		max_code_size *= 2;
	max_code_size = 0;

	/* free the step_position_table to allocate a new one */
	if (a_ctrl->step_position_table) {
		kfree(a_ctrl->step_position_table);
		a_ctrl->step_position_table = NULL;
	}

	/* Fill step position table */
	a_ctrl->step_position_table =
		kmalloc(sizeof(uint16_t) *
		(set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);
	if (a_ctrl->step_position_table == NULL) {
		pr_err("[%s::%d] mem alloc fail !\n", __func__, __LINE__);
		return -ENOMEM;
	}

	//cur_code = set_info->af_tuning_params.initial_code;
	cur_code = hvca_inf_position;
	/*Needs to be enabled once we have proper values in the EEPROM*/
	//a_ctrl->region_params[0].code_per_step =
	  //  (hvca_inf_position  - hvca_mac_position) /
	    //(set_info->af_tuning_params.total_steps - 10);
	a_ctrl->step_position_table[step_index++] = cur_code;
	for (region_index = 0;
		region_index < a_ctrl->region_size;
		region_index++) {
		code_per_step =
			a_ctrl->region_params[region_index].code_per_step;
		step_boundary =
			a_ctrl->region_params[region_index].
			step_bound[MOVE_NEAR];
		if (step_boundary >
			set_info->af_tuning_params.total_steps) {
			pr_err("invalid step_boundary = %d, max_val = %d",
				step_boundary,
				set_info->af_tuning_params.total_steps);
			kfree(a_ctrl->step_position_table);
			a_ctrl->step_position_table = NULL;
			return -EINVAL;
		}
		for (; step_index <= step_boundary;
			step_index++) {
			cur_code -= code_per_step;
			if (cur_code > max_code_size)
				a_ctrl->step_position_table[step_index] =
					cur_code;
			else {
				for (; step_index <
					set_info->af_tuning_params.total_steps;
					step_index++)
					a_ctrl->
						step_position_table[
						step_index] =
						max_code_size;
			}
		}
	}
	for (step_index = 0; step_index <
	      set_info->af_tuning_params.total_steps;
	      step_index++)
	      pr_info("[%s::%d]step_position_table[%d] = %d\n",
	      __func__, __LINE__,
	      step_index, a_ctrl->step_position_table[step_index]);
	CDBG("Exit\n");
	return 0;
}

static int32_t msm_actuator_set_default_focus(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_move_params_t *move_params)
{
	int32_t rc = 0;
	CDBG("Enter\n");

	if (a_ctrl->curr_step_pos != 0)
		rc = a_ctrl->func_tbl->actuator_move_focus(a_ctrl, move_params);

	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_vreg_control(struct msm_actuator_ctrl_t *a_ctrl,
							int config)
{
	int rc = 0, i, cnt;
	int idx = 0;

	struct msm_actuator_vreg *vreg_cfg;

	vreg_cfg = &a_ctrl->vreg_cfg;
	cnt = vreg_cfg->num_vreg;
	if (!cnt)
		return 0;

	if (cnt >= MSM_ACTUATOT_MAX_VREGS) {
		pr_err("%s failed %d cnt %d\n", __func__, __LINE__, cnt);
		return -EINVAL;
	}

	for (i = 0; i < cnt; i++) {
		if (config) {
			idx = i;
		} else {
			idx = cnt - (i + 1);
		}
		if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
			rc = msm_camera_config_single_vreg(&(a_ctrl->pdev->dev),
				&vreg_cfg->cam_vreg[idx],
				(struct regulator **)&vreg_cfg->data[idx],
				config);
		} else {
			rc = msm_camera_config_single_vreg(&(a_ctrl->i2c_client.client->dev),
				&vreg_cfg->cam_vreg[idx],
				(struct regulator **)&vreg_cfg->data[idx],
				config);
		}
	}
	return rc;
}

static int32_t msm_actuator_power_down(struct msm_actuator_ctrl_t *a_ctrl)
{
	int32_t rc = 0;
	CDBG("Enter\n");
	if (a_ctrl->actuator_state != ACTUATOR_POWER_DOWN) {
		if (a_ctrl->vcm_enable) {
			rc = gpio_direction_output(a_ctrl->vcm_pwd, 0);
			if (!rc)
				gpio_free(a_ctrl->vcm_pwd);
		}

		rc = msm_actuator_vreg_control(a_ctrl, 0);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			return rc;
		}
		CDBG("%s Exit:%d, Addr:0x%x\n", __func__, rc,
			a_ctrl->i2c_client.client->addr);

		if (a_ctrl->step_position_table) {
			kfree(a_ctrl->step_position_table);
			a_ctrl->step_position_table = NULL;
		}

		if (a_ctrl->i2c_reg_tbl) {
			kfree(a_ctrl->i2c_reg_tbl);
			a_ctrl->i2c_reg_tbl = NULL;
		}
		a_ctrl->i2c_tbl_index = 0;

		a_ctrl->actuator_state = ACTUATOR_POWER_DOWN;
	}
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_set_position(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_position_t *set_pos)
{
	int32_t rc = 0;
	int32_t index;
#ifdef CHECK_MERGE
	uint16_t next_lens_position;
	uint16_t delay;
	uint32_t hw_params = 0;
#endif
	uint8_t lsb, msb;
	struct msm_camera_i2c_reg_setting reg_setting;
	CDBG("%s Enter : steps = %d \n", __func__, set_pos->number_of_steps);
	if (set_pos->number_of_steps  == 0)
		return rc;

	/* NULL CHECK */
	if (a_ctrl->i2c_reg_tbl == NULL ) {
		pr_err("[%s::%d] NULL pointer check fail !\n", __func__, __LINE__);
		return -EFAULT;
	}

	a_ctrl->i2c_tbl_index = 0;
#ifdef CHECK_MERGE
	for (index = 0; index < set_pos->number_of_steps; index++) {
		next_lens_position = set_pos->pos[index];
		delay = set_pos->delay[index];
		a_ctrl->func_tbl->actuator_parse_i2c_params(a_ctrl,
		next_lens_position, hw_params, delay);

		reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
		reg_setting.size = a_ctrl->i2c_tbl_index;
		reg_setting.data_type = a_ctrl->i2c_data_type;

		rc = a_ctrl->i2c_client.i2c_func_tbl->
			i2c_write_table_w_microdelay(
			&a_ctrl->i2c_client, &reg_setting);
		if (rc < 0) {
			pr_err("%s Failed I2C write Line %d\n",
				__func__, __LINE__);
			return rc;
		}
		a_ctrl->i2c_tbl_index = 0;
	}
#endif

	for (index = 0; index < set_pos->number_of_steps; index++) {
		msb = ((set_pos->pos[index] << 7) & 0x80);
		lsb = set_pos->pos[index] >> 1;
		CDBG("%s index=%d msb= 0x%X, lsb=0x%X\n", __func__, index, msb, lsb);
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_addr = 0x00;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_data = lsb;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].delay = 0;
		a_ctrl->i2c_tbl_index++;

		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_addr = 0x01;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_data = msb;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].delay = set_pos->delay[index];
		a_ctrl->i2c_tbl_index++;
	}

	reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
	reg_setting.size = a_ctrl->i2c_tbl_index;
	reg_setting.data_type = a_ctrl->i2c_data_type;
		rc = a_ctrl->i2c_client.i2c_func_tbl->
			i2c_write_table_w_microdelay(
			&a_ctrl->i2c_client, &reg_setting);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}

	CDBG("%s exit %d\n", __func__, __LINE__);
	return rc;
}

 /*Added by Justin_Qualcomm for SEMCO Actuator Direct Move : 20130718*/
static int32_t msm_actuator_hvcm_set_position(
	struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_position_t *set_pos)
{
	int32_t rc = 0;
	int32_t index;
	uint8_t lsb, msb;
        struct msm_camera_i2c_reg_setting reg_setting;

	CDBG("%s Enter : steps = %d \n", __func__, set_pos->number_of_steps);
	if (set_pos->number_of_steps  == 0)
		return rc;

	/* NULL CHECK */
	if (a_ctrl->i2c_reg_tbl == NULL ) {
		pr_err("[%s::%d] NULL pointer check fail !\n", __func__, __LINE__);
		return -EFAULT;
	}

	a_ctrl->i2c_tbl_index = 0;
	for (index = 0; index < set_pos->number_of_steps; index++) {
		msb = (set_pos->pos[index]>>8)&0x00ff;
		lsb = set_pos->pos[index]&0x00ff;
		CDBG("%s index=%d msb= 0x%X, lsb=0x%X\n", __func__, index, msb, lsb);
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_addr = 0x01;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_data = lsb;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].delay = 0;
		a_ctrl->i2c_tbl_index++;

		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_addr = 0x02;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].reg_data = msb;
		a_ctrl->i2c_reg_tbl[a_ctrl->i2c_tbl_index].delay = set_pos->delay[index];
		a_ctrl->i2c_tbl_index++;
	}
	reg_setting.reg_setting = a_ctrl->i2c_reg_tbl;
	reg_setting.data_type = a_ctrl->i2c_data_type;
	reg_setting.size = a_ctrl->i2c_tbl_index;

	rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_write_table_w_microdelay(
		&a_ctrl->i2c_client, &reg_setting);
	if (rc < 0) {
		pr_err("%s Failed I2C write Line %d\n", __func__, __LINE__);
		return rc;
	}

	CDBG("%s exit %d\n", __func__, __LINE__);
	return rc;
}
 /*End - Added by Justin_Qualcomm for SEMCO Actuator Direct Move : 20130718*/

static int32_t msm_actuator_set_param(struct msm_actuator_ctrl_t *a_ctrl,
	struct msm_actuator_set_info_t *set_info) {
	struct reg_settings_t *init_settings = NULL;
	int32_t rc = -EFAULT;
	uint16_t i = 0;
	struct msm_camera_cci_client *cci_client = NULL;
	pr_info("[%s::%d]Enter\n", __func__, __LINE__);

	for (i = 0; i < ARRAY_SIZE(actuators); i++) {
		if (set_info->actuator_params.act_type ==
			actuators[i]->act_type) {
			a_ctrl->func_tbl = &actuators[i]->func_tbl;
			rc = 0;
		}
	}

	if (rc < 0) {
		pr_err("[%s::%d]Actuator function table not found\n", __func__, __LINE__);
		return rc;
	}
	if (set_info->af_tuning_params.total_steps
		>  MAX_ACTUATOR_AF_TOTAL_STEPS) {
		pr_err("[%s::%d]]Max actuator totalsteps exceeded = %d\n",
		__func__, __LINE__,set_info->af_tuning_params.total_steps);
		return -EFAULT;
	}
	if (set_info->af_tuning_params.region_size
		> MAX_ACTUATOR_REGION) {
		pr_err("[%s::%d] MAX_ACTUATOR_REGION is exceeded.\n", __func__, __LINE__);
		return -EFAULT;
	}

	a_ctrl->region_size = set_info->af_tuning_params.region_size;
	a_ctrl->pwd_step = set_info->af_tuning_params.pwd_step;
	a_ctrl->total_steps = set_info->af_tuning_params.total_steps;

	if (copy_from_user(&a_ctrl->region_params,
		(void *)set_info->af_tuning_params.region_params,
		a_ctrl->region_size * sizeof(struct region_params_t)))
		return -EFAULT;

	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		cci_client = a_ctrl->i2c_client.cci_client;
		cci_client->sid =
			set_info->actuator_params.i2c_addr >> 1;
		cci_client->retries = 3;
		cci_client->id_map = 0;
		cci_client->cci_i2c_master = a_ctrl->cci_master;
	} else {
		a_ctrl->i2c_client.client->addr =
			set_info->actuator_params.i2c_addr;
	}

	a_ctrl->i2c_data_type = set_info->actuator_params.i2c_data_type;
	a_ctrl->i2c_client.addr_type = set_info->actuator_params.i2c_addr_type;
	if (set_info->actuator_params.reg_tbl_size <=
		MAX_ACTUATOR_REG_TBL_SIZE) {
		a_ctrl->reg_tbl_size = set_info->actuator_params.reg_tbl_size;
	} else {
		a_ctrl->reg_tbl_size = 0;
		pr_err("[%s::%d]MAX_ACTUATOR_REG_TBL_SIZE is exceeded.\n", __func__, __LINE__);
		return -EFAULT;
	}

	if (a_ctrl->i2c_reg_tbl) {
		kfree(a_ctrl->i2c_reg_tbl);
		a_ctrl->i2c_reg_tbl = NULL;
	}
	a_ctrl->i2c_reg_tbl =
		kmalloc(sizeof(struct msm_camera_i2c_reg_array) *
		(set_info->af_tuning_params.total_steps + 1), GFP_KERNEL);
	if (!a_ctrl->i2c_reg_tbl) {
		pr_err("[%s::%d] kmalloc fail\n", __func__, __LINE__);
		return -ENOMEM;
	}

	if (copy_from_user(&a_ctrl->reg_tbl,
		(void *)set_info->actuator_params.reg_tbl_params,
		a_ctrl->reg_tbl_size *
		sizeof(struct msm_actuator_reg_params_t))) {
		kfree(a_ctrl->i2c_reg_tbl);
		a_ctrl->i2c_reg_tbl = NULL;
		return -EFAULT;
	}

	if (set_info->actuator_params.init_setting_size &&
		set_info->actuator_params.init_setting_size
		<= MAX_ACTUATOR_INIT_SET) {
		if (a_ctrl->func_tbl->actuator_init_focus) {
			init_settings = kmalloc(sizeof(struct reg_settings_t) *
				(set_info->actuator_params.init_setting_size),
				GFP_KERNEL);
			if (init_settings == NULL) {
				kfree(a_ctrl->i2c_reg_tbl);
				a_ctrl->i2c_reg_tbl = NULL;
				pr_err("[%s::%d]Error allocating memory for init_settings\n", __func__, __LINE__);
				return -EFAULT;
			}
			if (copy_from_user(init_settings,
				(void *)set_info->actuator_params.init_settings,
				set_info->actuator_params.init_setting_size *
				sizeof(struct reg_settings_t))) {
				kfree(init_settings);
				kfree(a_ctrl->i2c_reg_tbl);
				a_ctrl->i2c_reg_tbl = NULL;
				pr_err("[%s::%d]Error copying init_settings\n", __func__, __LINE__);
				return -EFAULT;
			}
			rc = a_ctrl->func_tbl->actuator_init_focus(a_ctrl,
				set_info->actuator_params.init_setting_size,
				init_settings);
			kfree(init_settings);
			if (rc < 0) {
				kfree(a_ctrl->i2c_reg_tbl);
				pr_err("[%s::%d]Error actuator_init_focus\n", __func__, __LINE__);
				return -EFAULT;
			}
		}
	}

	a_ctrl->initial_code = set_info->af_tuning_params.initial_code;
	if (a_ctrl->func_tbl->actuator_init_step_table)
		rc = a_ctrl->func_tbl->
			actuator_init_step_table(a_ctrl, set_info);

	a_ctrl->curr_step_pos = 0;
	a_ctrl->curr_region_index = 0;
	a_ctrl->actuator_state = ACTUATOR_POWER_UP;
	pr_info("[%s::%d]Exit\n", __func__, __LINE__);

	return rc;
}

static int msm_actuator_init(struct msm_actuator_ctrl_t *a_ctrl)
{
	int rc = 0;
	CDBG("Enter\n");
	if (!a_ctrl) {
		pr_err("[%s::%d]failed\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_util(
			&a_ctrl->i2c_client, MSM_CCI_INIT);
		if (rc < 0)
			pr_err("[%s::%d] cci_init failed\n", __func__, __LINE__);
	}
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_config(struct msm_actuator_ctrl_t *a_ctrl,
	void __user *argp)
{
	struct msm_actuator_cfg_data *cdata =
		(struct msm_actuator_cfg_data *)argp;
	int32_t rc = -EINVAL;
	mutex_lock(a_ctrl->actuator_mutex);
	CDBG("Enter\n");
	CDBG("%s type %d\n", __func__, cdata->cfgtype);
	switch (cdata->cfgtype) {
	case CFG_ACTUATOR_INIT:
		rc = msm_actuator_init(a_ctrl);
		if (rc < 0)
			pr_err("[%s::%d]msm_actuator_init failed %d\n", __func__, __LINE__, rc);
		break;
	case CFG_GET_ACTUATOR_INFO:
		cdata->is_af_supported = 1;
		cdata->cfg.cam_name = a_ctrl->cam_name;
		CDBG("%s a_ctrl->cam_name = %d\n", __func__, a_ctrl->cam_name);
		rc = 0;
		break;

	case CFG_SET_ACTUATOR_INFO:
		rc = msm_actuator_set_param(a_ctrl, &cdata->cfg.set_info);
		if (rc < 0)
			pr_err("[%s::%d] init table failed %d\n", __func__, __LINE__, rc);
		break;

	case CFG_SET_DEFAULT_FOCUS:
		if (a_ctrl->func_tbl &&
			a_ctrl->func_tbl->actuator_set_default_focus)
			rc = a_ctrl->func_tbl->actuator_set_default_focus(
				a_ctrl,	&cdata->cfg.move);
		if (rc < 0)
			pr_err("[%s::%d] move focus failed %d\n", __func__, __LINE__, rc);
		break;

	case CFG_MOVE_FOCUS:
		if (a_ctrl->func_tbl &&
			a_ctrl->func_tbl->actuator_move_focus)
			rc = a_ctrl->func_tbl->actuator_move_focus(a_ctrl,
				&cdata->cfg.move);
		if (rc < 0)
			pr_err("[%s::%d] move focus failed %d\n", __func__, __LINE__, rc);
		break;
	case CFG_ACTUATOR_POWERDOWN:
		rc = msm_actuator_power_down(a_ctrl);
		if (rc < 0)
			pr_err("[%s::%d] msm_actuator_power_down failed %d\n", __func__, __LINE__, rc);
		break;

	case CFG_SET_POSITION:
		if (a_ctrl->func_tbl &&
			a_ctrl->func_tbl->actuator_set_position)
			rc = a_ctrl->func_tbl->actuator_set_position(a_ctrl,
				&cdata->cfg.setpos);
		if (rc < 0)
			pr_err("[%s::%d] actuator_set_position failed %d\n", __func__, __LINE__, rc);
		break;

	case CFG_ACTUATOR_POWERUP:
		rc = msm_actuator_power_up(a_ctrl);
		if (rc < 0)
			pr_err("[%s::%d] Failed actuator power up%d\n", __func__, __LINE__, rc);
		break;

	default:
		break;
	}
	mutex_unlock(a_ctrl->actuator_mutex);
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_get_subdev_id(struct msm_actuator_ctrl_t *a_ctrl,
	void *arg)
{
	uint32_t *subdev_id = (uint32_t *)arg;
	CDBG("Enter\n");
	if (!subdev_id) {
		pr_err("[%s::%d] failed\n", __func__ , __LINE__);
		return -EINVAL;
	}
	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE)
		*subdev_id = a_ctrl->pdev->id;
	else
		*subdev_id = a_ctrl->subdev_id;

	CDBG("subdev_id %d\n", *subdev_id);
	CDBG("Exit\n");
	return 0;
}

static struct msm_camera_i2c_fn_t msm_sensor_cci_func_tbl = {
	.i2c_read = msm_camera_cci_i2c_read,
	.i2c_read_seq = msm_camera_cci_i2c_read_seq,
	.i2c_write = msm_camera_cci_i2c_write,
	.i2c_write_table = msm_camera_cci_i2c_write_table,
	.i2c_write_seq_table = msm_camera_cci_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_cci_i2c_write_table_w_microdelay,
	.i2c_util = msm_sensor_cci_i2c_util,
	.i2c_poll =  msm_camera_cci_i2c_poll,
};

static struct msm_camera_i2c_fn_t msm_sensor_qup_func_tbl = {
	.i2c_read = msm_camera_qup_i2c_read,
	.i2c_read_seq = msm_camera_qup_i2c_read_seq,
	.i2c_write = msm_camera_qup_i2c_write,
	.i2c_write_table = msm_camera_qup_i2c_write_table,
	.i2c_write_seq_table = msm_camera_qup_i2c_write_seq_table,
	.i2c_write_table_w_microdelay =
		msm_camera_qup_i2c_write_table_w_microdelay,
	.i2c_poll = msm_camera_qup_i2c_poll,
};

static int msm_actuator_open(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl =  v4l2_get_subdevdata(sd);
	pr_info("[%s::%d]Enter\n", __func__, __LINE__);
	if (!a_ctrl) {
		pr_err("[%s::%d] failed\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_util(
			&a_ctrl->i2c_client, MSM_CCI_INIT);
		if (rc < 0)
			pr_err("[%s::%d] cci_init failed\n", __func__, __LINE__);
	}
	if (a_ctrl->gpio_conf && a_ctrl->gpio_conf->cam_gpio_req_tbl) {
		CDBG("%s:%d request gpio\n", __func__, __LINE__);
		rc = msm_camera_request_gpio_table(
			a_ctrl->gpio_conf->cam_gpio_req_tbl,
			a_ctrl->gpio_conf->cam_gpio_req_tbl_size, 1);
		if (rc < 0) {
			pr_err("%s: request gpio failed\n", __func__);
			return rc;
		}
	}
	a_ctrl->is_camera_run = TRUE;
	pr_info("[%s::%d] Exit\n", __func__, __LINE__);
	return rc;
}

static int msm_actuator_close(struct v4l2_subdev *sd,
	struct v4l2_subdev_fh *fh) {
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl =  v4l2_get_subdevdata(sd);
	pr_info("[%s::%d] Enter\n", __func__, __LINE__);
	if (!a_ctrl) {
		pr_err("[%s::%d]failed\n", __func__, __LINE__);
		return -EINVAL;
	}
	if (a_ctrl->gpio_conf && a_ctrl->gpio_conf->cam_gpio_req_tbl) {
		CDBG("%s:%d release gpio\n", __func__, __LINE__);
		msm_camera_request_gpio_table(
			a_ctrl->gpio_conf->cam_gpio_req_tbl,
			a_ctrl->gpio_conf->cam_gpio_req_tbl_size, 0);
	}
	if (a_ctrl->act_device_type == MSM_CAMERA_PLATFORM_DEVICE) {
		rc = a_ctrl->i2c_client.i2c_func_tbl->i2c_util(
			&a_ctrl->i2c_client, MSM_CCI_RELEASE);
		if (rc < 0)
			pr_err("[%s::%d]cci_init failed\n", __func__, __LINE__);
	}
	a_ctrl->actuator_state = ACTUATOR_POWER_DOWN;
	a_ctrl->is_camera_run = FALSE;

	if (a_ctrl->step_position_table) {
		kfree(a_ctrl->step_position_table);
		a_ctrl->step_position_table = NULL;
	}

	if (a_ctrl->i2c_reg_tbl) {
		kfree(a_ctrl->i2c_reg_tbl);
		a_ctrl->i2c_reg_tbl = NULL;
	}
	pr_info("[%s::%d]Exit\n", __func__, __LINE__);
	return rc;
}

static const struct v4l2_subdev_internal_ops msm_actuator_internal_ops = {
	.open = msm_actuator_open,
	.close = msm_actuator_close,
};

static long msm_actuator_subdev_ioctl(struct v4l2_subdev *sd,
			unsigned int cmd, void *arg)
{
	struct msm_actuator_ctrl_t *a_ctrl = v4l2_get_subdevdata(sd);
	void __user *argp = (void __user *)arg;
	CDBG("Enter\n");
	CDBG("%s:%d a_ctrl %pK argp %pK\n", __func__, __LINE__, a_ctrl, argp);
	switch (cmd) {
	case VIDIOC_MSM_SENSOR_GET_SUBDEV_ID:
		return msm_actuator_get_subdev_id(a_ctrl, argp);
	case VIDIOC_MSM_ACTUATOR_CFG:
		return msm_actuator_config(a_ctrl, argp);
	case MSM_SD_SHUTDOWN:
		msm_actuator_close(sd, NULL);
		return 0;
	default:
		return -ENOIOCTLCMD;
	}
}

static int32_t msm_actuator_power_up(struct msm_actuator_ctrl_t *a_ctrl)
{
	int rc = 0;
	CDBG("%s called\n", __func__);

	if (a_ctrl->actuator_state != ACTUATOR_POWER_UP) {
		CDBG("vcm info: %d %d\n", a_ctrl->vcm_pwd,
			a_ctrl->vcm_enable);

		rc = msm_actuator_vreg_control(a_ctrl, 1);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			return rc;
		}

		if (a_ctrl->vcm_enable) {
			rc = gpio_request(a_ctrl->vcm_pwd, "msm_actuator");
			if (!rc) {
				CDBG("Enable VCM PWD\n");
				gpio_direction_output(a_ctrl->vcm_pwd, 1);
			}
		}
		a_ctrl->actuator_state = ACTUATOR_POWER_UP;
	}
	CDBG("Exit\n");
	return rc;
}

static int32_t msm_actuator_power(struct v4l2_subdev *sd, int on)
{
	int rc = 0;
	struct msm_actuator_ctrl_t *a_ctrl = v4l2_get_subdevdata(sd);
	CDBG("Enter\n");
	CDBG("%s:%d \n", __func__, __LINE__);
	mutex_lock(a_ctrl->actuator_mutex);
	if (on)
		rc = msm_actuator_power_up(a_ctrl);
	else
		rc = msm_actuator_power_down(a_ctrl);
	mutex_unlock(a_ctrl->actuator_mutex);
	CDBG("Exit\n");
	return rc;
}

static struct v4l2_subdev_core_ops msm_actuator_subdev_core_ops = {
	.ioctl = msm_actuator_subdev_ioctl,
	.s_power = msm_actuator_power,
};

static struct v4l2_subdev_ops msm_actuator_subdev_ops = {
	.core = &msm_actuator_subdev_core_ops,
};

static int32_t msm_actuator_get_dt_gpio_req_tbl(struct device_node *of_node,
	struct msm_camera_gpio_conf *gconf, uint16_t *gpio_array,
	uint16_t gpio_array_size)
{
	int32_t rc = 0, i = 0;
	uint32_t count = 0;
	uint32_t *val_array = NULL;

	if (!of_get_property(of_node, "qcom,gpio-req-tbl-num", &count))
		return 0;

	count /= sizeof(uint32_t);
	if (!count) {
		pr_err("%s qcom,gpio-req-tbl-num 0\n", __func__);
		return 0;
	}

	val_array = kzalloc(sizeof(uint32_t) * count, GFP_KERNEL);
	if (!val_array) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		return -ENOMEM;
	}

	gconf->cam_gpio_req_tbl = kzalloc(sizeof(struct gpio) * count,
		GFP_KERNEL);
	if (!gconf->cam_gpio_req_tbl) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		rc = -ENOMEM;
		goto ERROR1;
	}
	gconf->cam_gpio_req_tbl_size = count;

	rc = of_property_read_u32_array(of_node, "qcom,gpio-req-tbl-num",
		val_array, count);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto ERROR2;
	}
	for (i = 0; i < count; i++) {
		if (val_array[i] >= gpio_array_size) {
			pr_err("%s gpio req tbl index %d invalid\n",
				__func__, val_array[i]);
			return -EINVAL;
		}
		gconf->cam_gpio_req_tbl[i].gpio = gpio_array[val_array[i]];
		CDBG("%s cam_gpio_req_tbl[%d].gpio = %d\n", __func__, i,
			gconf->cam_gpio_req_tbl[i].gpio);
	}

	rc = of_property_read_u32_array(of_node, "qcom,gpio-req-tbl-flags",
		val_array, count);
	if (rc < 0) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		goto ERROR2;
	}
	for (i = 0; i < count; i++) {
		gconf->cam_gpio_req_tbl[i].flags = val_array[i];
		CDBG("%s cam_gpio_req_tbl[%d].flags = %ld\n", __func__, i,
			gconf->cam_gpio_req_tbl[i].flags);
	}

	for (i = 0; i < count; i++) {
		rc = of_property_read_string_index(of_node,
			"qcom,gpio-req-tbl-label", i,
			&gconf->cam_gpio_req_tbl[i].label);
		CDBG("%s cam_gpio_req_tbl[%d].label = %s\n", __func__, i,
			gconf->cam_gpio_req_tbl[i].label);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR2;
		}
	}

	kfree(val_array);
	return rc;

ERROR2:
	kfree(gconf->cam_gpio_req_tbl);
ERROR1:
	kfree(val_array);
	gconf->cam_gpio_req_tbl_size = 0;
	return rc;
}

static int32_t msm_actuator_get_gpio_data(struct msm_actuator_ctrl_t *a_ctrl,
	struct device_node *of_node)
{
	int32_t rc = 0;
	uint32_t i = 0;
	uint16_t gpio_array_size = 0;
	uint16_t *gpio_array = NULL;

	CDBG("%s : E\n", __func__);

	a_ctrl->gpio_conf = kzalloc(sizeof(struct msm_camera_gpio_conf),
		GFP_KERNEL);
	if (!a_ctrl->gpio_conf) {
		pr_err("%s failed %d\n", __func__, __LINE__);
		return -ENOMEM;
	}
	gpio_array_size = of_gpio_count(of_node);
	CDBG("%s gpio count %d\n", __func__, gpio_array_size);

	if (gpio_array_size) {
		gpio_array = kzalloc(sizeof(uint16_t) * gpio_array_size,
			GFP_KERNEL);
		if (!gpio_array) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			rc = -ENOMEM;
			goto ERROR1;
		}
		for (i = 0; i < gpio_array_size; i++) {
			gpio_array[i] = of_get_gpio(of_node, i);
			CDBG("%s gpio_array[%d] = %d\n", __func__, i,
				gpio_array[i]);
		}

		rc = msm_actuator_get_dt_gpio_req_tbl(of_node,
			a_ctrl->gpio_conf, gpio_array, gpio_array_size);
		if (rc < 0) {
			pr_err("%s failed %d\n", __func__, __LINE__);
			goto ERROR2;
		}
		kfree(gpio_array);
	}
	return 0;

ERROR2:
	kfree(gpio_array);
ERROR1:
	kfree(a_ctrl->gpio_conf);
	return rc;
}

static int32_t msm_actuator_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int rc = 0;
	struct msm_actuator_ctrl_t *act_ctrl_t = NULL;
	struct msm_actuator_vreg *vreg_cfg;
	bool check_use_gpios;

	CDBG("%s:%d Enter\n", __func__, __LINE__);

	if (client == NULL) {
		pr_err("[%s::%d] msm_actuator_i2c_probe: client is null\n", __func__, __LINE__);
		rc = -EINVAL;
		goto probe_failure;
	}

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		pr_err("[%s::%d] i2c_check_functionality failed\n", __func__, __LINE__);
		goto probe_failure;
	}

	if (!client->dev.of_node) {
		act_ctrl_t = (struct msm_actuator_ctrl_t *)(id->driver_data);
	} else {
		act_ctrl_t = kzalloc(sizeof(struct msm_actuator_ctrl_t),
			GFP_KERNEL);
		if (!act_ctrl_t) {
			pr_err("%s:%d no memory\n", __func__, __LINE__);
			return -ENOMEM;
		}
		if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
			pr_err("[%s::%d]i2c_check_functionality failed\n", __func__, __LINE__);
			goto probe_failure;
		}

		CDBG("client = 0x%pK\n",  client);

		rc = of_property_read_u32(client->dev.of_node, "cell-index",
			&act_ctrl_t->subdev_id);
		CDBG("cell-index %d, rc %d\n", act_ctrl_t->subdev_id, rc);
		act_ctrl_t->cam_name = act_ctrl_t->subdev_id;
		if (rc < 0) {
			pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
			kfree(act_ctrl_t);//prevent
			return rc;
		}
		check_use_gpios = of_property_read_bool(client->dev.of_node, "unuse-gpios");
		CDBG("%s: check unuse-gpio flag(%d)\n",
			__FUNCTION__, check_use_gpios);
		if (!check_use_gpios) {
			rc = msm_actuator_get_gpio_data(act_ctrl_t,
				client->dev.of_node);
		}
	}

	if (of_find_property(client->dev.of_node,
			"qcom,cam-vreg-name", NULL)) {
		vreg_cfg = &act_ctrl_t->vreg_cfg;
		rc = msm_camera_get_dt_vreg_data(client->dev.of_node,
			&vreg_cfg->cam_vreg, &vreg_cfg->num_vreg);
		if (rc < 0) {
			kfree(act_ctrl_t);
			pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
			return rc;
		}
	}

	act_ctrl_t->act_v4l2_subdev_ops = &msm_actuator_subdev_ops;
	act_ctrl_t->actuator_mutex = &msm_actuator_mutex;
	act_ctrl_t->i2c_driver = &msm_actuator_i2c_driver;

	CDBG("client = %x\n", (unsigned int) client);
	act_ctrl_t->i2c_client.client = client;
	act_ctrl_t->curr_step_pos = 0;
	act_ctrl_t->curr_region_index = 0;
	/* Set device type as I2C */
	act_ctrl_t->act_device_type = MSM_CAMERA_I2C_DEVICE;
	act_ctrl_t->i2c_client.i2c_func_tbl = &msm_sensor_qup_func_tbl;
	act_ctrl_t->act_v4l2_subdev_ops = &msm_actuator_subdev_ops;
	act_ctrl_t->actuator_mutex = &msm_actuator_mutex;
	act_ctrl_t->actuator_state = ACTUATOR_POWER_DOWN;
	act_ctrl_t->is_camera_run = FALSE;

	act_ctrl_t->cam_name = act_ctrl_t->subdev_id;
	CDBG("act_ctrl_t->cam_name: %d\n", act_ctrl_t->cam_name);
	/* Assign name for sub device */
	snprintf(act_ctrl_t->msm_sd.sd.name, sizeof(act_ctrl_t->msm_sd.sd.name),
		"%s", act_ctrl_t->i2c_driver->driver.name);

	/* Initialize sub device */
	v4l2_i2c_subdev_init(&act_ctrl_t->msm_sd.sd,
		act_ctrl_t->i2c_client.client,
		act_ctrl_t->act_v4l2_subdev_ops);
	v4l2_set_subdevdata(&act_ctrl_t->msm_sd.sd, act_ctrl_t);
	act_ctrl_t->msm_sd.sd.internal_ops = &msm_actuator_internal_ops;
	act_ctrl_t->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	media_entity_init(&act_ctrl_t->msm_sd.sd.entity, 0, NULL, 0);
	act_ctrl_t->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	act_ctrl_t->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_ACTUATOR;
	act_ctrl_t->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x2;
	msm_sd_register(&act_ctrl_t->msm_sd);

#if defined(CONFIG_OIS)
	g_msm_actuator_t = act_ctrl_t;
#endif

	if (act_ctrl_t) {
		af_sensor_interface.af_func = &msm_actuator_remove_noise;
		af_sensor_interface.af_pdata = (void *)act_ctrl_t;
		rc = remove_af_noise_register(&af_sensor_interface);
		if (rc < 0) {
			pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
		}
	}
	CDBG("Succeded Exit\n");

probe_failure:
	return rc;
}

static int32_t msm_actuator_platform_probe(struct platform_device *pdev)
{
	int32_t rc = 0;
	struct msm_camera_cci_client *cci_client = NULL;
	struct msm_actuator_ctrl_t *msm_actuator_t = NULL;
	struct msm_actuator_vreg *vreg_cfg;
	CDBG("Enter\n");

	if (!pdev->dev.of_node) {
		pr_err("[%s::%d] of_node NULL\n", __func__, __LINE__);
		return -EINVAL;
	}

	msm_actuator_t = kzalloc(sizeof(struct msm_actuator_ctrl_t),
		GFP_KERNEL);
	if (!msm_actuator_t) {
		pr_err("%s:%d failed no memory\n", __func__, __LINE__);
		return -ENOMEM;
	}
	rc = of_property_read_u32((&pdev->dev)->of_node, "cell-index",
		&pdev->id);
	CDBG("cell-index %d, rc %d\n", pdev->id, rc);
	if (rc < 0) {
		kfree(msm_actuator_t);
		pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
		return rc;
	}
	msm_actuator_t->subdev_id = pdev->id;
	rc = of_property_read_u32((&pdev->dev)->of_node, "qcom,cci-master",
		&msm_actuator_t->cci_master);
	CDBG("qcom,cci-master %d, rc %d\n", msm_actuator_t->cci_master, rc);
	if (rc < 0) {
		kfree(msm_actuator_t);
		pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
		return rc;
	}
	if (of_find_property((&pdev->dev)->of_node,
			"qcom,cam-vreg-name", NULL)) {
		vreg_cfg = &msm_actuator_t->vreg_cfg;
		rc = msm_camera_get_dt_vreg_data((&pdev->dev)->of_node,
			&vreg_cfg->cam_vreg, &vreg_cfg->num_vreg);
		if (rc < 0) {
			kfree(msm_actuator_t);
			pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
			return rc;
		}
	}

	msm_actuator_t->act_v4l2_subdev_ops = &msm_actuator_subdev_ops;
	msm_actuator_t->actuator_mutex = &msm_actuator_mutex;
	msm_actuator_t->cam_name = pdev->id;

	/* Set platform device handle */
	msm_actuator_t->pdev = pdev;
	/* Set device type as platform device */
	msm_actuator_t->act_device_type = MSM_CAMERA_PLATFORM_DEVICE;
	msm_actuator_t->i2c_client.i2c_func_tbl = &msm_sensor_cci_func_tbl;
	msm_actuator_t->i2c_client.cci_client = kzalloc(sizeof(
		struct msm_camera_cci_client), GFP_KERNEL);
	if (!msm_actuator_t->i2c_client.cci_client) {
		kfree(msm_actuator_t->vreg_cfg.cam_vreg);
		kfree(msm_actuator_t);
		pr_err("[%s::%d] failed no memory\n", __func__, __LINE__);
		return -ENOMEM;
	}
	msm_actuator_t->actuator_state = ACTUATOR_POWER_DOWN;
	msm_actuator_t->is_camera_run = FALSE;

	cci_client = msm_actuator_t->i2c_client.cci_client;
	cci_client->cci_subdev = msm_cci_get_subdev();
	cci_client->cci_i2c_master = MASTER_MAX;
	v4l2_subdev_init(&msm_actuator_t->msm_sd.sd,
		msm_actuator_t->act_v4l2_subdev_ops);
	v4l2_set_subdevdata(&msm_actuator_t->msm_sd.sd, msm_actuator_t);
	msm_actuator_t->msm_sd.sd.internal_ops = &msm_actuator_internal_ops;
	msm_actuator_t->msm_sd.sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	snprintf(msm_actuator_t->msm_sd.sd.name,
		ARRAY_SIZE(msm_actuator_t->msm_sd.sd.name), "msm_actuator");
	media_entity_init(&msm_actuator_t->msm_sd.sd.entity, 0, NULL, 0);
	msm_actuator_t->msm_sd.sd.entity.type = MEDIA_ENT_T_V4L2_SUBDEV;
	msm_actuator_t->msm_sd.sd.entity.group_id = MSM_CAMERA_SUBDEV_ACTUATOR;
	msm_actuator_t->msm_sd.close_seq = MSM_SD_CLOSE_2ND_CATEGORY | 0x2;
	msm_sd_register(&msm_actuator_t->msm_sd);
	msm_actuator_t->actuator_state = ACTUATOR_POWER_DOWN;

#if defined(CONFIG_OIS)
	g_msm_actuator_t = msm_actuator_t;
#endif

	if (msm_actuator_t) {
		af_sensor_interface.af_func = &msm_actuator_remove_noise;
		af_sensor_interface.af_pdata = (void *)msm_actuator_t;
		rc = remove_af_noise_register(&af_sensor_interface);
		if (rc < 0) {
			pr_err("[%s::%d] failed rc %d\n", __func__, __LINE__, rc);
		}
	}

	CDBG("Exit\n");
	return rc;
}

static const struct i2c_device_id msm_actuator_i2c_id[] = {
	{ "msm_actuator", (kernel_ulong_t)NULL },
	{ }
};
static const struct of_device_id msm_actuator_dt_match[] = {
	{.compatible = "qcom,actuator", .data = NULL},
	{}
};

static struct i2c_driver msm_actuator_i2c_driver = {
	.id_table = msm_actuator_i2c_id,
	.probe  = msm_actuator_i2c_probe,
	.remove = __exit_p(msm_actuator_i2c_remove),
	.driver = {
		.name = "msm_actuator",
		.owner = THIS_MODULE,
		.of_match_table = msm_actuator_dt_match,
	},
};

/*static const struct of_device_id msm_actuator_dt_match[] = {
	{.compatible = "qcom,actuator", .data = NULL},
	{}
};*/

MODULE_DEVICE_TABLE(of, msm_actuator_dt_match);

static struct platform_driver msm_actuator_platform_driver = {
	.driver = {
		.name = "qcom,actuator",
		.owner = THIS_MODULE,
		.of_match_table = msm_actuator_dt_match,
	},
};

static int __init msm_actuator_init_module(void)
{
	int32_t rc = 0;
	CDBG("Enter\n");
	rc = platform_driver_probe(&msm_actuator_platform_driver,
		msm_actuator_platform_probe);
	if (rc < 0) {
	    pr_err("%s:%d failed platform driver probe rc %d\n",
		   __func__, __LINE__, rc);
		//return rc;
	} else {
		CDBG("%s:%d platform_driver_probe rc %d\n", __func__, __LINE__, rc);
	}
	rc = i2c_add_driver(&msm_actuator_i2c_driver);
	if (rc < 0)
	    pr_err("%s:%d failed i2c driver probe rc %d\n",
		   __func__, __LINE__, rc);
	else
            CDBG("%s:%d i2c_add_driver rc %d\n", __func__, __LINE__, rc);

	return rc;
}

static struct msm_actuator msm_vcm_actuator_table = {
	.act_type = ACTUATOR_VCM,
	.func_tbl = {
		.actuator_init_step_table = msm_actuator_init_step_table,
		.actuator_move_focus = msm_actuator_move_focus,
		.actuator_write_focus = msm_actuator_write_focus,
		.actuator_set_default_focus = msm_actuator_set_default_focus,
		.actuator_init_focus = msm_actuator_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
		.actuator_set_position = msm_actuator_set_position,
	},
};

static struct msm_actuator msm_piezo_actuator_table = {
	.act_type = ACTUATOR_PIEZO,
	.func_tbl = {
		.actuator_init_step_table = NULL,
		.actuator_move_focus = msm_actuator_piezo_move_focus,
		.actuator_write_focus = NULL,
		.actuator_set_default_focus =
			msm_actuator_piezo_set_default_focus,
		.actuator_init_focus = msm_actuator_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
	},
};

static struct msm_actuator msm_hall_effect_actuator_table = {
	.act_type = ACTUATOR_HALL_EFFECT,
	.func_tbl = {
		.actuator_init_step_table = msm_actuator_init_step_table,
		.actuator_move_focus = msm_actuator_hall_effect_move_focus,
		.actuator_write_focus = NULL,
		.actuator_set_default_focus =
			msm_actuator_set_default_focus,
		.actuator_init_focus = msm_actuator_hall_effect_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
		.actuator_set_position = msm_actuator_set_position,
	},
};

static struct msm_actuator msm_hvcm_actuator_table = {
	.act_type = ACTUATOR_HVCM,
	.func_tbl = {
		.actuator_init_step_table = msm_actuator_hvcm_init_step_table,
		.actuator_move_focus = msm_actuator_move_focus,
		.actuator_write_focus = msm_actuator_write_focus,
		.actuator_set_default_focus = msm_actuator_set_default_focus,
		.actuator_init_focus = msm_actuator_hvcm_init_focus,
		.actuator_parse_i2c_params = msm_actuator_parse_i2c_params,
		.actuator_set_position = msm_actuator_hvcm_set_position, /*Added by Justin_Qualcomm for SEMCO Actuator Direct Move : 20130718*/
	},
};

module_init(msm_actuator_init_module);
MODULE_DESCRIPTION("MSM ACTUATOR");
MODULE_LICENSE("GPL v2");
