// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include <linux/delay.h>
#include <linux/string.h>

#include "kd_camera_typedef.h"
#include "kd_camera_feature.h"

#include "imgsensor_sensor.h"
#include "imgsensor_hw.h"

#ifdef OPLUS_FEATURE_CAMERA_COMMON
#include<soc/oppo/oppo_project.h>
int gpio57usercont = 0;
#endif

enum IMGSENSOR_RETURN imgsensor_hw_init(struct IMGSENSOR_HW *phw)
{
	struct IMGSENSOR_HW_SENSOR_POWER      *psensor_pwr;
	struct IMGSENSOR_HW_CFG               *pcust_pwr_cfg;
	struct IMGSENSOR_HW_CUSTOM_POWER_INFO *ppwr_info;
	int i, j;
	char str_prop_name[LENGTH_FOR_SNPRINTF];
	struct device_node *of_node
		= of_find_compatible_node(NULL, NULL, "mediatek,imgsensor");

	mutex_init(&phw->common.pinctrl_mutex);

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (hw_open[i] != NULL)
			(hw_open[i]) (&phw->pdev[i]);

		if (phw->pdev[i]->init != NULL)
			(phw->pdev[i]->init)(
				phw->pdev[i]->pinstance, &phw->common);
	}

	for (i = 0; i < IMGSENSOR_SENSOR_IDX_MAX_NUM; i++) {
		psensor_pwr = &phw->sensor_pwr[i];

		#ifdef OPLUS_FEATURE_CAMERA_COMMON
		if (is_project(19536) || is_project(19537) ||
			is_project(19538) || is_project(19539) ||
			is_project(19541)) {
			pcust_pwr_cfg = imgsensor_custom_config_19537;
		} else if (is_project(OPPO_19550) || is_project(OPPO_19551) || is_project(OPPO_19553)
			|| is_project(OPPO_19556)) {
			pcust_pwr_cfg = imgsensor_custom_config_P90Q;
		} else if (is_project(OPPO_19357) || is_project(OPPO_19354) || is_project(OPPO_19358) || is_project(OPPO_19359)) {
			pcust_pwr_cfg = imgsensor_custom_config_19357;
		} else if (is_project(OPPO_19011) || is_project(OPPO_19301)) {
		        pcust_pwr_cfg = imgsensor_custom_config_19301;
		} else if (is_project(OPPO_18593) || is_project(OPPO_18073)) {
			pcust_pwr_cfg = imgsensor_custom_config_18593;
		} else if (is_project(OPPO_20291) || is_project(OPPO_20292) ||
			is_project(OPPO_20293) || is_project(OPPO_20294) ||
			is_project(OPPO_20295)) {
			pcust_pwr_cfg = imgsensor_custom_config_20291;
		} else {
			pcust_pwr_cfg = imgsensor_custom_config;
		}
		#else
		pcust_pwr_cfg = imgsensor_custom_config;
		#endif
		while (pcust_pwr_cfg->sensor_idx != i &&
		       pcust_pwr_cfg->sensor_idx != IMGSENSOR_SENSOR_IDX_NONE)
			pcust_pwr_cfg++;

		if (pcust_pwr_cfg->sensor_idx == IMGSENSOR_SENSOR_IDX_NONE)
			continue;

		ppwr_info = pcust_pwr_cfg->pwr_info;
		while (ppwr_info->pin != IMGSENSOR_HW_PIN_NONE) {
			for (j = 0;
				j < IMGSENSOR_HW_ID_MAX_NUM &&
					ppwr_info->id != phw->pdev[j]->id;
				j++) {
			}

			psensor_pwr->id[ppwr_info->pin] = j;
			ppwr_info++;
		}
	}

	for (i = 0; i < IMGSENSOR_SENSOR_IDX_MAX_NUM; i++) {
		memset(str_prop_name, 0, sizeof(str_prop_name));
		snprintf(str_prop_name,
					sizeof(str_prop_name),
					"cam%d_%s",
					i,
					"enable_sensor");
		if (of_property_read_string(
			of_node, str_prop_name,
			&phw->enable_sensor_by_index[i]) < 0) {
			PK_DBG("Property cust-sensor not defined\n");
			phw->enable_sensor_by_index[i] = NULL;
		}
	}

	return IMGSENSOR_RETURN_SUCCESS;
}

enum IMGSENSOR_RETURN imgsensor_hw_release_all(struct IMGSENSOR_HW *phw)
{
	int i;

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (phw->pdev[i]->release != NULL)
			(phw->pdev[i]->release)(phw->pdev[i]->pinstance);
	}
	return IMGSENSOR_RETURN_SUCCESS;
}

#ifdef OPLUS_FEATURE_CAMERA_COMMON
static bool g_is_sub2_gc02m0 = false;
static bool g_is_main3_gc02m0 = false;
void set_gc02m0_flag(enum IMGSENSOR_SENSOR_IDX sensor_idx)
{
	if (sensor_idx == IMGSENSOR_SENSOR_IDX_SUB2)
		g_is_sub2_gc02m0 = true;
	else if (sensor_idx == IMGSENSOR_SENSOR_IDX_MAIN3)
		g_is_main3_gc02m0 = true;
}
#endif
static enum IMGSENSOR_RETURN imgsensor_hw_power_sequence(
		struct IMGSENSOR_HW             *phw,
		enum   IMGSENSOR_SENSOR_IDX      sensor_idx,
		enum   IMGSENSOR_HW_POWER_STATUS pwr_status,
		struct IMGSENSOR_HW_POWER_SEQ   *ppower_sequence,
		char *pcurr_idx)
{
	struct IMGSENSOR_HW_SENSOR_POWER *psensor_pwr =
					&phw->sensor_pwr[sensor_idx];
	struct IMGSENSOR_HW_POWER_SEQ    *ppwr_seq = ppower_sequence;
	struct IMGSENSOR_HW_POWER_INFO   *ppwr_info;
	struct IMGSENSOR_HW_DEVICE       *pdev;
	int                               pin_cnt = 0;

	static DEFINE_RATELIMIT_STATE(ratelimit, 1 * HZ, 30);

#ifdef CONFIG_FPGA_EARLY_PORTING  /*for FPGA*/
	if (1) {
		PK_DBG("FPGA return true for power control\n");
		return IMGSENSOR_RETURN_SUCCESS;
	}
#endif

	while (ppwr_seq < ppower_sequence + IMGSENSOR_HW_SENSOR_MAX_NUM &&
		ppwr_seq->name != NULL) {
		if (!strcmp(ppwr_seq->name, PLATFORM_POWER_SEQ_NAME)) {
			if (sensor_idx == ppwr_seq->_idx)
				break;
		} else {
			if (!strcmp(ppwr_seq->name, pcurr_idx))
				break;
		}
		ppwr_seq++;
	}

	if (ppwr_seq->name == NULL)
		return IMGSENSOR_RETURN_ERROR;

	ppwr_info = ppwr_seq->pwr_info;

	while (ppwr_info->pin != IMGSENSOR_HW_PIN_NONE &&
	       ppwr_info < ppwr_seq->pwr_info + IMGSENSOR_HW_POWER_INFO_MAX) {

		if (pwr_status == IMGSENSOR_HW_POWER_STATUS_ON) {
			if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
				pdev =
				phw->pdev[psensor_pwr->id[ppwr_info->pin]];

				if (__ratelimit(&ratelimit))
					PK_DBG(
					"sensor_idx %d, ppwr_info->pin %d, ppwr_info->pin_state_on %d",
					sensor_idx,
					ppwr_info->pin,
					ppwr_info->pin_state_on);
				#ifdef OPLUS_FEATURE_CAMERA_COMMON
				if (is_project(OPPO_19550) || is_project(OPPO_19551) || is_project(OPPO_19553) || is_project(OPPO_19556) || is_project(OPPO_19357) || is_project(OPPO_19354) || is_project(OPPO_19358) || is_project(OPPO_19359)) {
					if((sensor_idx == 2||(sensor_idx == 3)) && (ppwr_info->pin == 8))
						gpio57usercont ++;
					PK_DBG("on gpio57usercont = %d",gpio57usercont);
				}
				#endif
				if (pdev->set != NULL)
					pdev->set(pdev->pinstance,
					sensor_idx,
				    ppwr_info->pin, ppwr_info->pin_state_on);
			}

			mdelay(ppwr_info->pin_on_delay);
		}

		ppwr_info++;
		pin_cnt++;
	}

	if (pwr_status == IMGSENSOR_HW_POWER_STATUS_OFF) {
		while (pin_cnt) {
			ppwr_info--;
			pin_cnt--;

			if (__ratelimit(&ratelimit))
				PK_DBG(
				"sensor_idx %d, ppwr_info->pin %d, ppwr_info->pin_state_off %d",
				sensor_idx,
				ppwr_info->pin,
				ppwr_info->pin_state_off);
			#ifdef OPLUS_FEATURE_CAMERA_COMMON
			if (is_project(OPPO_19550) || is_project(OPPO_19551) || is_project(OPPO_19553) || is_project(OPPO_19556) || is_project(OPPO_19357) || is_project(OPPO_19354) || is_project(OPPO_19358) || is_project(OPPO_19359)) {
				if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
					pdev =phw->pdev[psensor_pwr->id[ppwr_info->pin]];
					if((ppwr_info->pin == 8)&&(gpio57usercont ==1)&&((sensor_idx == 2)||(sensor_idx == 3)))
					{
						pdev->set(pdev->pinstance,
						sensor_idx,
						ppwr_info->pin, ppwr_info->pin_state_off);
						PK_DBG("off gpio57usercont poweroff= %d",gpio57usercont);
					}
					if ((pdev->set != NULL)&&(ppwr_info->pin != 8))
						pdev->set(pdev->pinstance,sensor_idx,ppwr_info->pin,ppwr_info->pin_state_off);

					if((sensor_idx == 2||(sensor_idx == 3)) && (ppwr_info->pin == 8))
						gpio57usercont --;
					PK_DBG("off gpio57usercont = %d",gpio57usercont);
				}
			} else {
				if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
					pdev =
					phw->pdev[psensor_pwr->id[ppwr_info->pin]];

					if (pdev->set != NULL)
						pdev->set(pdev->pinstance,
						sensor_idx,
					ppwr_info->pin, ppwr_info->pin_state_off);
				}
			    #ifdef OPLUS_FEATURE_CAMERA_COMMON
			    if (is_project(19011) || is_project(19301)) {
				PK_DBG("19301 iovdd/avdd/dvdd need more delay time");
				mdelay(ppwr_info->pin_off_delay);
			    } else {
				mdelay(ppwr_info->pin_on_delay);
			    }
			    #else
				mdelay(ppwr_info->pin_on_delay);
			    #endif
			}
			#else
			if (ppwr_info->pin != IMGSENSOR_HW_PIN_UNDEF) {
				pdev =
				phw->pdev[psensor_pwr->id[ppwr_info->pin]];

				if (pdev->set != NULL)
					pdev->set(pdev->pinstance,
					sensor_idx,
				ppwr_info->pin, ppwr_info->pin_state_off);
			}
			#endif
		}
	}
	#ifdef OPLUS_FEATURE_CAMERA_COMMON
	if ((sensor_idx == IMGSENSOR_SENSOR_IDX_MAIN) && (is_project(OPPO_19011) || is_project(OPPO_19301))) {
		if (pwr_status == IMGSENSOR_HW_POWER_STATUS_ON) {//power up
			pdev = phw->pdev[IMGSENSOR_HW_ID_GPIO];
			if (g_is_sub2_gc02m0 && pdev->set != NULL) {
				PK_DBG("force sub2_gc02 avdd to high");
				pdev->set(pdev->pinstance, IMGSENSOR_SENSOR_IDX_SUB2, IMGSENSOR_HW_PIN_AVDD, Vol_2800);
			}
			if (g_is_main3_gc02m0 && pdev->set != NULL) {
				PK_DBG("force main3_gc02 avdd to high");
				pdev->set(pdev->pinstance, IMGSENSOR_SENSOR_IDX_MAIN3, IMGSENSOR_HW_PIN_AVDD, Vol_2800);
			}
		} else if (pwr_status == IMGSENSOR_HW_POWER_STATUS_OFF) {//power down
			pdev = phw->pdev[IMGSENSOR_HW_ID_GPIO];
			if (g_is_sub2_gc02m0 && pdev->set != NULL) {
				PK_DBG("force sub2_gc02 avdd to low");
				pdev->set(pdev->pinstance, IMGSENSOR_SENSOR_IDX_SUB2, IMGSENSOR_HW_PIN_AVDD, Vol_Low);
			}
			if (g_is_main3_gc02m0 && pdev->set != NULL) {
				PK_DBG("force main3_gc02 avdd to low");
				pdev->set(pdev->pinstance, IMGSENSOR_SENSOR_IDX_MAIN3, IMGSENSOR_HW_PIN_AVDD, Vol_Low);
			}
		}
	}
	#endif

	return IMGSENSOR_RETURN_SUCCESS;
}

enum IMGSENSOR_RETURN imgsensor_hw_power(
		struct IMGSENSOR_HW *phw,
		struct IMGSENSOR_SENSOR *psensor,
		enum IMGSENSOR_HW_POWER_STATUS pwr_status)
{
	enum IMGSENSOR_SENSOR_IDX sensor_idx = psensor->inst.sensor_idx;
	char *curr_sensor_name = psensor->inst.psensor_list->name;
	char str_index[LENGTH_FOR_SNPRINTF];
	int ret = 0;

	PK_DBG("sensor_idx %d, power %d curr_sensor_name %s, enable list %s\n",
		sensor_idx,
		pwr_status,
		curr_sensor_name,
		phw->enable_sensor_by_index[(uint32_t)sensor_idx] == NULL
		? "NULL"
		: phw->enable_sensor_by_index[(uint32_t)sensor_idx]);

	if (phw->enable_sensor_by_index[(uint32_t)sensor_idx] &&
	!strstr(phw->enable_sensor_by_index[(uint32_t)sensor_idx], curr_sensor_name))
		return IMGSENSOR_RETURN_ERROR;


	ret = snprintf(str_index, sizeof(str_index), "%d", sensor_idx);
	if (ret == 0) {
		pr_info("Error! snprintf allocate 0");
		ret = IMGSENSOR_RETURN_ERROR;
		return ret;
	}
#ifdef OPLUS_FEATURE_CAMERA_COMMON
	if (is_project(19536) || is_project(19537) ||
		is_project(19538) || is_project(19539) ||
		is_project(19541)) {
		imgsensor_hw_power_sequence(
				phw,
				sensor_idx,
				pwr_status,
				platform_power_sequence_19537,
				str_index);
		imgsensor_hw_power_sequence(
				phw,
				sensor_idx,
				pwr_status, sensor_power_sequence_19537, curr_sensor_name);
	} else if (is_project(OPPO_20291) || is_project(OPPO_20292)
		|| is_project(OPPO_20293) || is_project(OPPO_20294)
		|| is_project(OPPO_20295)) {
		imgsensor_hw_power_sequence(
				phw,
				sensor_idx,
				pwr_status,
				platform_power_sequence_20291,
				str_index);
		imgsensor_hw_power_sequence(
				phw,
				sensor_idx,
				pwr_status, sensor_power_sequence_20291, curr_sensor_name);
	} else if (is_project(OPPO_19550) || is_project(OPPO_19551)
		|| is_project(OPPO_19553) || is_project(OPPO_19556)) {
		imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence_19551, curr_sensor_name);
	} else if (is_project(OPPO_19357) || is_project(OPPO_19354)
		|| is_project(OPPO_19358) || is_project(OPPO_19359)) {
		imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence_19357, curr_sensor_name);
	} else if(is_project(OPPO_19011) || is_project(OPPO_19301)){
        imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status,
			platform_power_sequence_19301,
			str_index);
        imgsensor_hw_power_sequence(
            phw,
            sensor_idx,
            pwr_status, sensor_power_sequence_19301, curr_sensor_name);
	}else if (is_project(OPPO_18593) || is_project(OPPO_18073)) {
		imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence_18593, curr_sensor_name);
	} else {
		imgsensor_hw_power_sequence(
				phw,
				sensor_idx,
				pwr_status,
				platform_power_sequence,
				str_index);
		imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence, curr_sensor_name);
	}
#else
	imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status,
			platform_power_sequence,
			str_index);
	imgsensor_hw_power_sequence(
			phw,
			sensor_idx,
			pwr_status, sensor_power_sequence, curr_sensor_name);
#endif

	return IMGSENSOR_RETURN_SUCCESS;
}

enum IMGSENSOR_RETURN imgsensor_hw_dump(struct IMGSENSOR_HW *phw)
{
	int i;

	for (i = 0; i < IMGSENSOR_HW_ID_MAX_NUM; i++) {
		if (phw->pdev[i]->dump != NULL)
			(phw->pdev[i]->dump)(phw->pdev[i]->pinstance);
	}
	return IMGSENSOR_RETURN_SUCCESS;
}

