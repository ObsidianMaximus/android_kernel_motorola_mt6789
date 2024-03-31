#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/types.h>
#include "s5kjn1mipiraw_Sensor.h"
#define S5KJN1_SERIAL_NUM_SIZE 16
#define EEPROM_READY   1
static DEFINE_SPINLOCK(imgsensor_lock);
#if EEPROM_READY //stan
#define RWB_ID_OFFSET 0x0F73
#define EEPROM_READ_ID  0xA4
#define EEPROM_WRITE_ID   0xA5
#endif

#if EEPROM_READY //cfp-210812

#define DEVONN_S5KJN1_EEPROM_SLAVE_ADDR 0xA0
#define DEVONN_S5KJN1_SENSOR_IIC_SLAVE_ADDR 0xAC
#define DEVONN_S5KJN1_EEPROM_SIZE  0x39c7
#define DEVONN_S5KJN1_EEPROM_CRC_AF_CAL_SIZE 24
#define DEVONN_S5KJN1_EEPROM_CRC_AWB_CAL_SIZE 43
#define DEVONN_S5KJN1_EEPROM_CRC_LSC_SIZE 1868
#define DEVONN_S5KJN1_EEPROM_CRC_PDAF_OUTPUT1_SIZE 496
#define DEVONN_S5KJN1_EEPROM_CRC_PDAF_OUTPUT2_SIZE 1004
#define DEVONN_S5KJN1_EEPROM_CRC_MANUFACTURING_SIZE 37
#define DEVONN_S5KJN1_MANUFACTURE_PART_NUMBER "28D14866"
#define DEVONN_S5KJN1_MPN_LENGTH 8
#define DEVONN_S5KJN1_EEPROM_GGC_SIZE 346
#define DEVONN_S5KJN1_EEPROM_GGC_START_ADDR 0x386B
#define DEVONN_S5KJN1_EEPROM_GGC_END_ADDR 0x39C4
static kal_uint16 addr_data_pair_ggc_jn1sq[DEVONN_S5KJN1_EEPROM_GGC_SIZE+4] = {0};
static uint8_t DEVONN_S5KJN1_eeprom[DEVONN_S5KJN1_EEPROM_SIZE] = {0};
static calibration_status_t mnf_status = CRC_FAILURE;
static calibration_status_t af_status = CRC_FAILURE;
static calibration_status_t awb_status = CRC_FAILURE;
static calibration_status_t lsc_status = CRC_FAILURE;
static calibration_status_t pdaf_status = CRC_FAILURE;
static calibration_status_t dual_status = CRC_FAILURE;

static uint8_t crc_reverse_byte(uint32_t data)
{
	return ((data * 0x0802LU & 0x22110LU) |
		(data * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
}

static uint32_t convert_crc(uint8_t *crc_ptr)
{
	return (crc_ptr[0] << 8) | (crc_ptr[1]);
}

static uint16_t to_uint16_swap(uint8_t *data)
{
	uint16_t converted;
	memcpy(&converted, data, sizeof(uint16_t));
	return ntohs(converted);
}

static void DEVONN_S5KJN1_read_data_from_eeprom(kal_uint8 slave, kal_uint32 start_add, uint32_t size)
{
	int i = 0;
	spin_lock(&imgsensor_lock);
	imgsensor.i2c_write_id = slave;
	spin_unlock(&imgsensor_lock);

	//read eeprom data
	for (i = 0; i < size; i ++) {
		DEVONN_S5KJN1_eeprom[i] = read_cmos_sensor_byte(start_add);
		start_add ++;
	}

	spin_lock(&imgsensor_lock);
	imgsensor.i2c_write_id = DEVONN_S5KJN1_SENSOR_IIC_SLAVE_ADDR;
	spin_unlock(&imgsensor_lock);
}

static int32_t eeprom_util_check_crc16(uint8_t *data, uint32_t size, uint32_t ref_crc)
{
	int32_t crc_match = 0;
	uint16_t crc = 0x0000;
	uint16_t crc_reverse = 0x0000;
	uint32_t i, j;

	uint32_t tmp;
	uint32_t tmp_reverse;

	/* Calculate both methods of CRC since integrators differ on
	* how CRC should be calculated. */
	for (i = 0; i < size; i++) {
		tmp_reverse = crc_reverse_byte(data[i]);
		tmp = data[i] & 0xff;
		for (j = 0; j < 8; j++) {
			if (((crc & 0x8000) >> 8) ^ (tmp & 0x80))
				crc = (crc << 1) ^ 0x8005;
			else
				crc = crc << 1;
			tmp <<= 1;

			if (((crc_reverse & 0x8000) >> 8) ^ (tmp_reverse & 0x80))
				crc_reverse = (crc_reverse << 1) ^ 0x8005;
			else
				crc_reverse = crc_reverse << 1;

			tmp_reverse <<= 1;
		}
	}

	crc_reverse = (crc_reverse_byte(crc_reverse) << 8) |
		crc_reverse_byte(crc_reverse >> 8);

	if (crc == ref_crc || crc_reverse == ref_crc)
		crc_match = 1;

	LOG_INF("REF_CRC 0x%x CALC CRC 0x%x CALC Reverse CRC 0x%x matches? %d\n",
		ref_crc, crc, crc_reverse, crc_match);

	return crc_match;
}


static uint8_t mot_eeprom_util_check_awb_limits(awb_t unit, awb_t golden)
{
	uint8_t result = 0;

	if (unit.r < AWB_R_MIN || unit.r > AWB_R_MAX) {
		LOG_INF("unit r out of range! MIN: %d, r: %d, MAX: %d",
			AWB_R_MIN, unit.r, AWB_R_MAX);
		result = 1;
	}
	if (unit.gr < AWB_GR_MIN || unit.gr > AWB_GR_MAX) {
		LOG_INF("unit gr out of range! MIN: %d, gr: %d, MAX: %d",
			AWB_GR_MIN, unit.gr, AWB_GR_MAX);
		result = 1;
	}
	if (unit.gb < AWB_GB_MIN || unit.gb > AWB_GB_MAX) {
		LOG_INF("unit gb out of range! MIN: %d, gb: %d, MAX: %d",
			AWB_GB_MIN, unit.gb, AWB_GB_MAX);
		result = 1;
	}
	if (unit.b < AWB_B_MIN || unit.b > AWB_B_MAX) {
		LOG_INF("unit b out of range! MIN: %d, b: %d, MAX: %d",
			AWB_B_MIN, unit.b, AWB_B_MAX);
		result = 1;
	}

	if (golden.r < AWB_R_MIN || golden.r > AWB_R_MAX) {
		LOG_INF("golden r out of range! MIN: %d, r: %d, MAX: %d",
			AWB_R_MIN, golden.r, AWB_R_MAX);
		result = 1;
	}
	if (golden.gr < AWB_GR_MIN || golden.gr > AWB_GR_MAX) {
		LOG_INF("golden gr out of range! MIN: %d, gr: %d, MAX: %d",
			AWB_GR_MIN, golden.gr, AWB_GR_MAX);
		result = 1;
	}
	if (golden.gb < AWB_GB_MIN || golden.gb > AWB_GB_MAX) {
		LOG_INF("golden gb out of range! MIN: %d, gb: %d, MAX: %d",
			AWB_GB_MIN, golden.gb, AWB_GB_MAX);
		result = 1;
	}
	if (golden.b < AWB_B_MIN || golden.b > AWB_B_MAX) {
		LOG_INF("golden b out of range! MIN: %d, b: %d, MAX: %d",
			AWB_B_MIN, golden.b, AWB_B_MAX);
		result = 1;
	}

	return result;
}

static uint8_t mot_eeprom_util_calculate_awb_factors_limit(awb_t unit, awb_t golden,
		awb_limit_t limit)
{
	uint32_t r_g;
	uint32_t b_g;
	uint32_t golden_rg, golden_bg;
	uint32_t r_g_golden_min;
	uint32_t r_g_golden_max;
	uint32_t b_g_golden_min;
	uint32_t b_g_golden_max;

	r_g = unit.r_g * 1000;
	b_g = unit.b_g*1000;

	golden_rg = golden.r_g* 1000;
	golden_bg = golden.b_g* 1000;

	r_g_golden_min = limit.r_g_golden_min*16384;
	r_g_golden_max = limit.r_g_golden_max*16384;
	b_g_golden_min = limit.b_g_golden_min*16384;
	b_g_golden_max = limit.b_g_golden_max*16384;
	LOG_INF("rg = %d, bg=%d,rgmin=%d,bgmax =%d\n",r_g,b_g,r_g_golden_min,r_g_golden_max);
	LOG_INF("grg = %d, gbg=%d,bgmin=%d,bgmax =%d\n",golden_rg,golden_bg,b_g_golden_min,b_g_golden_max);
	if (r_g < (golden_rg - r_g_golden_min) || r_g > (golden_rg + r_g_golden_max)) {
		LOG_INF("Final RG calibration factors out of range!");
		return 1;
	}

	if (b_g < (golden_bg - b_g_golden_min) || b_g > (golden_bg + b_g_golden_max)) {
		LOG_INF("Final BG calibration factors out of range!");
		return 1;
	}
	return 0;
}

static calibration_status_t DEVONN_S5KJN1_check_awb_data(void *data)
{
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;
	awb_t unit;
	awb_t golden;
	awb_limit_t golden_limit;

	if(!eeprom_util_check_crc16(eeprom->cie_src_1_ev,
		DEVONN_S5KJN1_EEPROM_CRC_AWB_CAL_SIZE,
		convert_crc(eeprom->awb_crc16))) {
		LOG_INF("AWB CRC Fails!");
		return CRC_FAILURE;
	}

	unit.r = to_uint16_swap(eeprom->awb_src_1_r)/64;
	unit.gr = to_uint16_swap(eeprom->awb_src_1_gr)/64;
	unit.gb = to_uint16_swap(eeprom->awb_src_1_gb)/64;
	unit.b = to_uint16_swap(eeprom->awb_src_1_b)/64;
	unit.r_g = to_uint16_swap(eeprom->awb_src_1_rg_ratio);
	unit.b_g = to_uint16_swap(eeprom->awb_src_1_bg_ratio);
	unit.gr_gb = to_uint16_swap(eeprom->awb_src_1_gr_gb_ratio);

	golden.r = to_uint16_swap(eeprom->awb_src_1_golden_r)/64;
	golden.gr = to_uint16_swap(eeprom->awb_src_1_golden_gr)/64;
	golden.gb = to_uint16_swap(eeprom->awb_src_1_golden_gb)/64;
	golden.b = to_uint16_swap(eeprom->awb_src_1_golden_b)/64;
	golden.r_g = to_uint16_swap(eeprom->awb_src_1_golden_rg_ratio);
	golden.b_g = to_uint16_swap(eeprom->awb_src_1_golden_bg_ratio);
	golden.gr_gb = to_uint16_swap(eeprom->awb_src_1_golden_gr_gb_ratio);
	if (mot_eeprom_util_check_awb_limits(unit, golden)) {
		LOG_INF("AWB CRC limit Fails!");
		return LIMIT_FAILURE;
	}

	golden_limit.r_g_golden_min = eeprom->awb_r_g_golden_min_limit[0];
	golden_limit.r_g_golden_max = eeprom->awb_r_g_golden_max_limit[0];
	golden_limit.b_g_golden_min = eeprom->awb_b_g_golden_min_limit[0];
	golden_limit.b_g_golden_max = eeprom->awb_b_g_golden_max_limit[0];

	if (mot_eeprom_util_calculate_awb_factors_limit(unit, golden,golden_limit)) {
		LOG_INF("AWB CRC factor limit Fails!");
		return LIMIT_FAILURE;
	}
	LOG_INF("AWB CRC Pass");
	return NO_ERRORS;
}

static calibration_status_t DEVONN_S5KJN1_check_lsc_data_mtk(void *data)
{
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;

	if (!eeprom_util_check_crc16(eeprom->lsc_data_mtk, DEVONN_S5KJN1_EEPROM_CRC_LSC_SIZE,
		convert_crc(eeprom->lsc_crc16_mtk))) {
		LOG_INF("LSC CRC Fails!");
		return CRC_FAILURE;
	}
	LOG_INF("LSC CRC Pass");
	return NO_ERRORS;
}

static void DEVONN_S5KJN1_eeprom_get_mnf_data(void *data,
		mot_calibration_mnf_t *mnf)
{
	int ret;
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;

	ret = snprintf(mnf->table_revision, MAX_CALIBRATION_STRING, "0x%x",
		eeprom->eeprom_table_version[0]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->table_revision failed");
		mnf->table_revision[0] = 0;
	}

	ret = snprintf(mnf->mot_part_number, MAX_CALIBRATION_STRING, "%c%c%c%c%c%c%c%c",
		eeprom->mpn[0], eeprom->mpn[1], eeprom->mpn[2], eeprom->mpn[3],
		eeprom->mpn[4], eeprom->mpn[5], eeprom->mpn[6], eeprom->mpn[7]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->mot_part_number failed");
		mnf->mot_part_number[0] = 0;
	}

	ret = snprintf(mnf->actuator_id, MAX_CALIBRATION_STRING, "0x%x", eeprom->actuator_id[0]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->actuator_id failed");
		mnf->actuator_id[0] = 0;
	}

	ret = snprintf(mnf->lens_id, MAX_CALIBRATION_STRING, "0x%x", eeprom->lens_id[0]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->lens_id failed");
		mnf->lens_id[0] = 0;
	}

	if (eeprom->manufacturer_id[0] == 'S' && eeprom->manufacturer_id[1] == 'U') {
		ret = snprintf(mnf->integrator, MAX_CALIBRATION_STRING, "Sunny");
	} else if (eeprom->manufacturer_id[0] == 'O' && eeprom->manufacturer_id[1] == 'F') {
		ret = snprintf(mnf->integrator, MAX_CALIBRATION_STRING, "OFilm");
	} else if (eeprom->manufacturer_id[0] == 'Q' && eeprom->manufacturer_id[1] == 'T') {
		ret = snprintf(mnf->integrator, MAX_CALIBRATION_STRING, "Qtech");
	} else {
		ret = snprintf(mnf->integrator, MAX_CALIBRATION_STRING, "Unknown");
		LOG_INF("unknown manufacturer_id");
	}

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->integrator failed");
		mnf->integrator[0] = 0;
	}

	ret = snprintf(mnf->factory_id, MAX_CALIBRATION_STRING, "%c%c",
		eeprom->factory_id[0], eeprom->factory_id[1]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->factory_id failed");
		mnf->factory_id[0] = 0;
	}

	ret = snprintf(mnf->manufacture_line, MAX_CALIBRATION_STRING, "%u",
		eeprom->manufacture_line[0]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->manufacture_line failed");
		mnf->manufacture_line[0] = 0;
	}

	ret = snprintf(mnf->manufacture_date, MAX_CALIBRATION_STRING, "20%u/%u/%u",
		eeprom->manufacture_date[0], eeprom->manufacture_date[1], eeprom->manufacture_date[2]);

	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->manufacture_date failed");
		mnf->manufacture_date[0] = 0;
	}

	ret = snprintf(mnf->serial_number, MAX_CALIBRATION_STRING, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
		eeprom->serial_number[0], eeprom->serial_number[1],
		eeprom->serial_number[2], eeprom->serial_number[3],
		eeprom->serial_number[4], eeprom->serial_number[5],
		eeprom->serial_number[6], eeprom->serial_number[7],
		eeprom->serial_number[8], eeprom->serial_number[9],
		eeprom->serial_number[10], eeprom->serial_number[11],
		eeprom->serial_number[12], eeprom->serial_number[13],
		eeprom->serial_number[14], eeprom->serial_number[15]);
	if (ret < 0 || ret >= MAX_CALIBRATION_STRING) {
		LOG_INF("snprintf of mnf->serial_number failed");
		mnf->serial_number[0] = 0;
	}
}

static calibration_status_t DEVONN_S5KJN1_check_af_data(void *data)
{
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;

	if (!eeprom_util_check_crc16(eeprom->af_data, DEVONN_S5KJN1_EEPROM_CRC_AF_CAL_SIZE,
		convert_crc(eeprom->af_crc16))) {
		LOG_INF("Autofocus CRC Fails!");
		return CRC_FAILURE;
	}
	LOG_INF("Autofocus CRC Pass");
	return NO_ERRORS;
}

static calibration_status_t DEVONN_S5KJN1_check_pdaf_data(void *data)
{
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;

	if (!eeprom_util_check_crc16(eeprom->pdaf_out1_data_mtk, DEVONN_S5KJN1_EEPROM_CRC_PDAF_OUTPUT1_SIZE,
		convert_crc(eeprom->pdaf_out1_crc16_mtk))) {
		LOG_INF("PDAF OUTPUT1 CRC Fails!");
		return CRC_FAILURE;
	}

	if (!eeprom_util_check_crc16(eeprom->pdaf_out2_data_mtk, DEVONN_S5KJN1_EEPROM_CRC_PDAF_OUTPUT2_SIZE,
		convert_crc(eeprom->pdaf_out2_crc16_mtk))) {
		LOG_INF("PDAF OUTPUT2 CRC Fails!");
		return CRC_FAILURE;
	}

	LOG_INF("PDAF CRC Pass");
	return NO_ERRORS;
}

static calibration_status_t DEVONN_S5KJN1_check_manufacturing_data(void *data)
{
	struct DEVONN_S5KJN1_eeprom_t *eeprom = (struct DEVONN_S5KJN1_eeprom_t*)data;
        LOG_INF("Manufacturing eeprom->mpn = %s !",eeprom->mpn);

	if (!eeprom_util_check_crc16(data, DEVONN_S5KJN1_EEPROM_CRC_MANUFACTURING_SIZE,
		convert_crc(eeprom->manufacture_crc16))) {
		LOG_INF("Manufacturing CRC Fails!");
		return CRC_FAILURE;
	}
	LOG_INF("Manufacturing CRC Pass");
	return NO_ERRORS;
}

static void DEVONN_S5KJN1_eeprom_format_calibration_data(void *data)
{
	if (NULL == data) {
		LOG_INF("data is NULL");
		return;
	}

	mnf_status = DEVONN_S5KJN1_check_manufacturing_data(data);
	af_status = DEVONN_S5KJN1_check_af_data(data);
	awb_status = DEVONN_S5KJN1_check_awb_data(data);;
	lsc_status = DEVONN_S5KJN1_check_lsc_data_mtk(data);;
	pdaf_status = DEVONN_S5KJN1_check_pdaf_data(data);
	dual_status = 0;

	LOG_INF("status mnf:%d, af:%d, awb:%d, lsc:%d, pdaf:%d, dual:%d",
		mnf_status, af_status, awb_status, lsc_status, pdaf_status, dual_status);
}


#define S5KJN1_REMOSAIC_PARAM_XTC_DATA_START_ADDR 0x191D
#define S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE 2612

#define S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_START_ADDR 0x2353
#define S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE 768

#define S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_START_ADDR 0x2655
#define S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE 4000

#define S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_START_ADDR 0x35F7
#define S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE 626

#define S5KJN1_REMOSAIC_PARAM_TOTAL_DATA_SIZE (\
                                S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE + \
                                S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE + \
                                S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE + \
                                S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE)

static uint8_t DEVONN_S5KJN1_eeprom_for_remosaic[S5KJN1_REMOSAIC_PARAM_TOTAL_DATA_SIZE + 2] = {0};
enum REMOSAIC_DATA_TYPE {
    REMOSAIC_DATA_TYPE_XTC,
    REMOSAIC_DATA_TYPE_SENSOR_XTC,
    REMOSAIC_DATA_TYPE_PDXTC,
    REMOSAIC_DATA_TYPE_SW_GGC,
};

void * remosaic_data_parse(enum REMOSAIC_DATA_TYPE type, void * data)
{
    void * data_point = NULL;
    uint8_t data_crc16[2] = {0};
    if(data == NULL)
    {
        LOG_INF("NULL of data");
        return NULL;
    }

    LOG_INF("type: %d", type);
    switch(type)
    {
        case REMOSAIC_DATA_TYPE_XTC:
            data_point = data + S5KJN1_REMOSAIC_PARAM_XTC_DATA_START_ADDR;
            data_crc16[0] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE);
            data_crc16[1] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE + 1);
            if (!eeprom_util_check_crc16(data_point, S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE, convert_crc(data_crc16)))
            {
                LOG_INF("XTC data CRC Fails!");
                return NULL;
            }
            break;
        case REMOSAIC_DATA_TYPE_SENSOR_XTC:
            data_point = data + S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_START_ADDR;
            data_crc16[0] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE);
            data_crc16[1] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE + 1);
            if (!eeprom_util_check_crc16(data_point, S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE, convert_crc(data_crc16)))
            {
                LOG_INF("sensor XTC data CRC Fails!");
                return NULL;
            }
            break;
        case REMOSAIC_DATA_TYPE_PDXTC:
            data_point = data + S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_START_ADDR;
            data_crc16[0] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE);
            data_crc16[1] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE + 1);
            if (!eeprom_util_check_crc16(data_point, S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE, convert_crc(data_crc16)))
            {
                LOG_INF("PDXTC data CRC Fails!");
                return NULL;
            }
            break;
        case REMOSAIC_DATA_TYPE_SW_GGC:
            data_point = data + S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_START_ADDR;
            data_crc16[0] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE);
            data_crc16[1] = *(uint8_t *)(data_point + S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE + 1);
            if (!eeprom_util_check_crc16(data_point, S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE, convert_crc(data_crc16)))
            {
                LOG_INF("SW_GGC data CRC Fails!");
                return NULL;
            }
            break;
        default:
            return NULL;
            break;
    }

    return data_point;
}

static void remosaic_data_get(void *src_data)
{
    int dst_data_index = 0;
    void * src_data_point = NULL;
    char * dst_data_point = &DEVONN_S5KJN1_eeprom_for_remosaic[0];

    dst_data_point[0] = (S5KJN1_REMOSAIC_PARAM_TOTAL_DATA_SIZE & 0xff);/*Low*/
    dst_data_point[1] = ((S5KJN1_REMOSAIC_PARAM_TOTAL_DATA_SIZE >> 8) & 0xff);/*High*/
    dst_data_index = 2;

    src_data_point = remosaic_data_parse(REMOSAIC_DATA_TYPE_XTC, src_data);
    memcpy(&dst_data_point[dst_data_index], src_data_point, S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE);
    dst_data_index = dst_data_index + S5KJN1_REMOSAIC_PARAM_XTC_DATA_SIZE;

    src_data_point = remosaic_data_parse(REMOSAIC_DATA_TYPE_SENSOR_XTC, src_data);
    memcpy(&dst_data_point[dst_data_index], src_data_point, S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE);
    dst_data_index = dst_data_index + S5KJN1_REMOSAIC_PARAM_SENSOR_XTC_DATA_SIZE;

    src_data_point = remosaic_data_parse(REMOSAIC_DATA_TYPE_PDXTC, src_data);
    memcpy(&dst_data_point[dst_data_index], src_data_point, S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE);
    dst_data_index = dst_data_index + S5KJN1_REMOSAIC_PARAM_PDXTC_DATA_SIZE ;


    src_data_point = remosaic_data_parse(REMOSAIC_DATA_TYPE_SW_GGC, src_data);
    memcpy(&dst_data_point[dst_data_index], src_data_point, S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE);
    dst_data_index = dst_data_index + S5KJN1_REMOSAIC_PARAM_SW_GGC_DATA_SIZE ;

    LOG_INF("Total data num. is %d", dst_data_index);
}

#endif   //cfp-210812
