/*
 * Galaxycore GC2145 driver.
 * Copyright (C) 2018 Ondřej Jirman <megi@xff.cz>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#define DEBUG

#include <asm/div64.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/*
 * GC2145
 * - 2M pixel
 * - 1600 x 1200, max frame rate: 720P, 30fps@96MHz
 * - Bayer RGB, RGB565, YCbCr 4:2:2
 * - AE, AWB
 * - PLL
 * - AVDD 2.7-3V, DVDD 1.7-1.9V, IOVDD 1.7-3V
 * - Power 180mW / 200uA standby
 * - Interpolation, denoise, gamma, edge enhance
 * I2C:
 * - write reg8
 * - read reg8
 * - write reg8 multi
 *
 * Power on:
 * MCLK on
 * PWDN, RESET low
 * IOVDD, AVDD, DVDD on in sequence
 * RESET high
 *
 * Power off:
 * PWDN, RESET low
 * RESET high
 * delay
 * PWDN high
 * RESET low
 * IOVDD, AVDD, DVDD off
 * PWDN low?
 * MCLK off
 *
 * Init:
 * - check chip id
 * - setup pll
 * - setup CSI interface / PAD drive strength
 * - setup resolution/fps
 * - enable postprocessing
 *   (ISP related chapter)
 *
 * Stream on:
 * - ???
 */

#define GC2145_FIRMWARE_PARAMETERS	"gc2145-init.bin"

#define GC2145_SENSOR_WIDTH_MIN		88u
#define GC2145_SENSOR_HEIGHT_MIN	72u

//XXX: 1616x1232 8H/16V dummy pixels on each side
#define GC2145_SENSOR_WIDTH_MAX		1600u
#define GC2145_SENSOR_HEIGHT_MAX	1200u

/* {{{ Register definitions */

/* system registers */
#define GC2145_REG_CHIP_ID			0xf0
#define GC2145_REG_CHIP_ID_VALUE		0x2145

#define GC2145_REG_PAD_IO		0xf2
#define GC2145_REG_PLL_MODE1		0xf7
#define GC2145_REG_PLL_MODE2		0xf8
#define GC2145_REG_CM_MODE		0xf9
#define GC2145_REG_CLK_DIV_MODE		0xfa
#define GC2145_REG_ANALOG_PWC		0xfc
#define GC2145_REG_SCALER_MODE		0xfd
#define GC2145_REG_RESET		0xfe

#define GC2145_P0_EXPOSURE_HI		0x03
#define GC2145_P0_EXPOSURE_LO		0x04
#define GC2145_P0_HBLANK_DELAY_HI	0x05
#define GC2145_P0_HBLANK_DELAY_LO	0x06
#define GC2145_P0_VBLANK_DELAY_HI	0x07
#define GC2145_P0_VBLANK_DELAY_LO	0x08
#define GC2145_P0_ROW_START_HI		0x09
#define GC2145_P0_ROW_START_LO		0x0a
#define GC2145_P0_COL_START_HI		0x0b
#define GC2145_P0_COL_START_LO		0x0c
#define GC2145_P0_WIN_HEIGHT_HI		0x0d
#define GC2145_P0_WIN_HEIGHT_LO		0x0e
#define GC2145_P0_WIN_WIDTH_HI		0x0f
#define GC2145_P0_WIN_WIDTH_LO		0x10
#define GC2145_P0_SH_DELAY_HI		0x11
#define GC2145_P0_SH_DELAY_LO		0x12
#define GC2145_P0_START_TIME		0x13
#define GC2145_P0_END_TIME		0x14

#define GC2145_P0_ISP_BLK_ENABLE1	0x80
#define GC2145_P0_ISP_BLK_ENABLE2	0x81
#define GC2145_P0_ISP_BLK_ENABLE3	0x82
#define GC2145_P0_ISP_SPECIAL_EFFECT	0x83
#define GC2145_P0_ISP_OUT_FORMAT	0x84
#define GC2145_P0_FRAME_START		0x85
#define GC2145_P0_SYNC_MODE		0x86
#define GC2145_P0_ISP_BLK_ENABLE4	0x87
#define GC2145_P0_ISP_MODULE_GATING	0x88
#define GC2145_P0_ISP_BYPASS_MODE	0x89
#define GC2145_P0_DEBUG_MODE2		0x8c
#define GC2145_P0_DEBUG_MODE3		0x8d

#define GC2145_P0_CROP_ENABLE		0x90
#define GC2145_P0_CROP_Y1_HI		0x91
#define GC2145_P0_CROP_Y1_LO		0x92
#define GC2145_P0_CROP_X1_HI		0x93
#define GC2145_P0_CROP_X1_LO		0x94
#define GC2145_P0_CROP_WIN_HEIGHT_HI	0x95
#define GC2145_P0_CROP_WIN_HEIGHT_LO	0x96
#define GC2145_P0_CROP_WIN_WIDTH_HI	0x97
#define GC2145_P0_CROP_WIN_WIDTH_LO	0x98

#define GC2145_P0_SUBSAMPLE_RATIO	0x99
#define GC2145_P0_SUBSAMPLE_MODE	0x9a
#define GC2145_P0_SUB_ROW_N1		0x9b
#define GC2145_P0_SUB_ROW_N2		0x9c
#define GC2145_P0_SUB_ROW_N3		0x9d
#define GC2145_P0_SUB_ROW_N4		0x9e
#define GC2145_P0_SUB_COL_N1		0x9f
#define GC2145_P0_SUB_COL_N2		0xa0
#define GC2145_P0_SUB_COL_N3		0xa1
#define GC2145_P0_SUB_COL_N4		0xa2
#define GC2145_P0_OUT_BUF_ENABLE	0xc2

/* }}} */

struct gc2145_pixfmt {
	u32 code;
	u32 colorspace;
	u8 fmt_setup;
};

static const struct gc2145_pixfmt gc2145_formats[] = {
	{
		.code              = MEDIA_BUS_FMT_UYVY8_2X8,
		.colorspace        = V4L2_COLORSPACE_SRGB,
		.fmt_setup         = 0x00,
	},
	{
		.code              = MEDIA_BUS_FMT_VYUY8_2X8,
		.colorspace        = V4L2_COLORSPACE_SRGB,
		.fmt_setup         = 0x01,
	},
	{
		.code              = MEDIA_BUS_FMT_YUYV8_2X8,
		.colorspace        = V4L2_COLORSPACE_SRGB,
		.fmt_setup         = 0x02,
	},
	{
		.code              = MEDIA_BUS_FMT_YVYU8_2X8,
		.colorspace        = V4L2_COLORSPACE_SRGB,
		.fmt_setup         = 0x03,
	},
	{
		.code              = MEDIA_BUS_FMT_RGB565_2X8_LE,
		.colorspace        = V4L2_COLORSPACE_SRGB,
		.fmt_setup         = 0x06,
	},
	{
		.code              = MEDIA_BUS_FMT_SBGGR8_1X8,
		.colorspace        = V4L2_COLORSPACE_RAW,
		.fmt_setup         = 0x17,
	},
};

static const struct gc2145_pixfmt *gc2145_find_format(u32 code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gc2145_formats); i++)
		if (gc2145_formats[i].code == code)
			return &gc2145_formats[i];

	return NULL;
}

/* regulator supplies */
static const char * const gc2145_supply_name[] = {
	"IOVDD", /* Digital I/O (1.7-3V) suppply */
	"AVDD",  /* Analog (2.7-3V) supply */
	"DVDD",  /* Digital Core (1.7-1.9V) supply */
};

#define GC2145_NUM_SUPPLIES ARRAY_SIZE(gc2145_supply_name)

struct gc2145_ctrls {
	struct v4l2_ctrl_handler handler;
	struct {
		struct v4l2_ctrl *auto_exposure;
		struct v4l2_ctrl *exposure;
		struct v4l2_ctrl *d_gain;
		struct v4l2_ctrl *a_gain;
	};
	struct v4l2_ctrl *metering;
	struct v4l2_ctrl *exposure_bias;
	struct {
		struct v4l2_ctrl *wb;
		struct v4l2_ctrl *blue_balance;
		struct v4l2_ctrl *red_balance;
	};
	struct v4l2_ctrl *aaa_lock;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *pl_freq;
	struct v4l2_ctrl *colorfx;
	struct v4l2_ctrl *brightness;
	struct v4l2_ctrl *saturation;
	struct v4l2_ctrl *contrast;
	struct v4l2_ctrl *gamma;
	struct v4l2_ctrl *test_pattern;
	struct v4l2_ctrl *test_data[4];
};

enum {
	TX_WRITE = 1,
	TX_WRITE16,
	TX_UPDATE_BITS,
};

#define GC2145_MAX_OPS 64

struct gc2145_tx_op {
	int op;
	u16 reg;
	u16 val;
	u16 mask;
};

struct gc2145_dev {
	struct i2c_client *i2c_client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
	struct clk *xclk; /* external clock for GC2145 */

	struct regulator_bulk_data supplies[GC2145_NUM_SUPPLIES];
	struct gpio_desc *reset_gpio; // nrst pin
	struct gpio_desc *enable_gpio; // ce pin

	/* lock to protect all members below */
	struct mutex lock;

	struct v4l2_mbus_framefmt fmt;
	struct v4l2_fract frame_interval;
	struct gc2145_ctrls ctrls;

	bool pending_mode_change;
	bool powered;
	bool streaming;

	u8 current_bank;

	struct gc2145_tx_op ops[GC2145_MAX_OPS];
	int n_ops;
	int tx_started;
};

static inline struct gc2145_dev *to_gc2145_dev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc2145_dev, sd);
}

/* {{{ Register access helpers */

static int gc2145_write_regs(struct gc2145_dev *sensor, u8 addr,
			     u8 *data, int data_size)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[128 + 1];
	int ret;

	if (data_size > sizeof(buf) - 1) {
		v4l2_err(&sensor->sd, "%s: oversized transfer (size=%d)\n",
			 __func__, data_size);
		return -EINVAL;
	}

	buf[0] = addr;
	memcpy(buf + 1, data, data_size);

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = data_size + 1;

	dev_dbg(&sensor->i2c_client->dev, "[wr %02x] <= %*ph\n",
		(u32)addr, data_size, data);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		v4l2_err(&sensor->sd,
			 "%s: error %d: addr=%x, data=%*ph\n",
			 __func__, ret, (u32)addr, data_size, data);
		return ret;
	}

	return 0;
}

static int gc2145_read_regs(struct gc2145_dev *sensor, u8 addr,
			    u8 *data, int data_size)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	int ret;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = &addr;
	msg[0].len = 1;

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = data;
	msg[1].len = data_size;

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		v4l2_err(&sensor->sd,
			 "%s: error %d: start_index=%x, data_size=%d\n",
			 __func__, ret, (u32)addr, data_size);
		return ret;
	}

	dev_dbg(&sensor->i2c_client->dev, "[rd %02x] => %*ph\n",
		(u32)addr, data_size, data);

	return 0;
}

static int gc2145_switch_bank(struct gc2145_dev *sensor, u16 reg)
{
	int ret;
	u8 bank = reg >> 8;

	if (bank & ~3u)
		return -ERANGE;

	if (sensor->current_bank != bank) {
		ret = gc2145_write_regs(sensor, GC2145_REG_RESET, &bank, 1);
		if (ret)
			return ret;

		sensor->current_bank = bank;
		dev_info(&sensor->i2c_client->dev, "bank switch: 0x%02x\n",
				(unsigned int)sensor->current_bank);
	}

	return 0;
}

static int gc2145_read(struct gc2145_dev *sensor, u16 reg, u8 *val)
{
	int ret;

	ret = gc2145_switch_bank(sensor, reg);
	if (ret)
		return ret;

	return gc2145_read_regs(sensor, reg, val, 1);
}

static int gc2145_write(struct gc2145_dev *sensor, u16 reg, u8 val)
{
	int ret;

	ret = gc2145_switch_bank(sensor, reg);
	if (ret)
		return ret;

	if ((reg & 0xffu) == GC2145_REG_RESET)
		sensor->current_bank = val & 3;

	return gc2145_write_regs(sensor, reg, &val, 1);
}

static int gc2145_update_bits(struct gc2145_dev *sensor, u16 reg, u8 mask, u8 val)
{
	int ret;
	u8 tmp;

	ret = gc2145_read(sensor, reg, &tmp);
	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= val & mask;

	return gc2145_write(sensor, reg, tmp);
}

static int gc2145_read16(struct gc2145_dev *sensor, u16 reg, u16 *val)
{
	int ret;

	ret = gc2145_switch_bank(sensor, reg);
	if (ret)
		return ret;

	ret = gc2145_read_regs(sensor, reg, (u8 *)val, sizeof(*val));
	if (ret)
		return ret;

	*val = be16_to_cpu(*val);
	return 0;
}

static int gc2145_write16(struct gc2145_dev *sensor, u16 reg, u16 val)
{
	u16 tmp = cpu_to_be16(val);
	int ret;

	ret = gc2145_switch_bank(sensor, reg);
	if (ret)
		return ret;

	return gc2145_write_regs(sensor, reg, (u8 *)&tmp, sizeof(tmp));
}

static void gc2145_tx_start(struct gc2145_dev *sensor)
{
	if (sensor->tx_started++)
		dev_err(&sensor->i2c_client->dev,
				"tx_start called multiple times\n");

	sensor->n_ops = 0;
}

static void gc2145_tx_add(struct gc2145_dev *sensor, int kind,
			  u16 reg, u16 val, u16 mask)
{
	struct gc2145_tx_op *op;

	if (!sensor->tx_started) {
		dev_err(&sensor->i2c_client->dev,
				"op added without calling tx_start\n");
		return;
	}

	if (sensor->n_ops >= ARRAY_SIZE(sensor->ops)) {
		dev_err(&sensor->i2c_client->dev,
				"ops overflow, increase GC2145_MAX_OPS\n");
		return;
	}

	op = &sensor->ops[sensor->n_ops++];
	op->op = kind;
	op->reg = reg;
	op->val = val;
	op->mask = mask;
}

static void gc2145_tx_write8(struct gc2145_dev *sensor, u16 reg, u8 val)
{
	return gc2145_tx_add(sensor, TX_WRITE, reg, val, 0);
}

static void gc2145_tx_write16(struct gc2145_dev *sensor, u16 reg, u16 val)
{
	return gc2145_tx_add(sensor, TX_WRITE16, reg, val, 0);
}

static void gc2145_tx_update_bits(struct gc2145_dev *sensor, u16 reg,
				  u8 mask, u8 val)
{
	return gc2145_tx_add(sensor, TX_UPDATE_BITS, reg, val, mask);
}

static int gc2145_tx_commit(struct gc2145_dev *sensor)
{
	struct gc2145_tx_op* op;
	int i, ret, n_ops;

	if (!sensor->tx_started) {
		dev_err(&sensor->i2c_client->dev,
				"tx_commit called without tx_start\n");
		return 0;
	}

	n_ops = sensor->n_ops;
	sensor->tx_started = 0;
	sensor->n_ops = 0;

	for (i = 0; i < n_ops; i++) {
		op = &sensor->ops[i];

		switch (op->op) {
		case TX_WRITE:
			ret = gc2145_write(sensor, op->reg, op->val);
			break;
		case TX_WRITE16:
			ret = gc2145_write16(sensor, op->reg, op->val);
			break;
		case TX_UPDATE_BITS:
			ret = gc2145_update_bits(sensor, op->reg, op->mask, op->val);
			break;
		default:
			dev_err(&sensor->i2c_client->dev, "invalid op at %d\n", i);
			ret = -EINVAL;
		}

		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Efficiently write to a set of registers, using auto-increment
 * when possible. User must not use address 0xff. To switch banks,
 * use sequence: 0xfe, bank_no.
 */
static int gc2145_set_registers(struct gc2145_dev *sensor,
				const uint8_t* data, size_t data_len)
{
	int ret = 0, i = 0;
	u16 start, len;
	u8 buf[128];

	if (data_len % 2 != 0) {
		v4l2_err(&sensor->sd, "Register map has invalid size\n");
		return -EINVAL;
	}

	/* we speed up communication by using auto-increment functionality */
	while (i < data_len) {
		start = data[i];
		len = 0;

		while (i < data_len && data[i] == (start + len) &&
		       len < sizeof(buf)) {
			buf[len++] = data[i + 1];
			i += 2;
		}

		ret = gc2145_write_regs(sensor, start, buf, len);
		if (ret)
			return ret;
	}

	sensor->current_bank = 0xff;
	return 0;
}

/*
 * The firmware format:
 * <record 0>, ..., <record N - 1>
 * "record" is a 1-byte register address followed by 1-byte data
 */
static int gc2145_load_firmware(struct gc2145_dev *sensor, const char *name)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, name, sensor->sd.v4l2_dev->dev);
	if (ret) {
		v4l2_warn(&sensor->sd,
			  "Failed to read firmware %s, continuing anyway...\n",
			  name);
		return 1;
	}

	if (fw->size == 0)
		return 1;

	ret = gc2145_set_registers(sensor, fw->data, fw->size);

	release_firmware(fw);
	return ret;
}

/* }}} */
/* {{{ Controls */

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return &container_of(ctrl->handler, struct gc2145_dev,
			     ctrls.handler)->sd;
}

#if 0
static const u8 gc2145_wb_opts[][2] = {
	{ V4L2_WHITE_BALANCE_MANUAL, GC2145_REG_WB_MODE_OFF },
	{ V4L2_WHITE_BALANCE_INCANDESCENT, GC2145_REG_WB_MODE_TUNGSTEN_PRESET },
	{ V4L2_WHITE_BALANCE_FLUORESCENT,
		GC2145_REG_WB_MODE_FLUORESCENT_PRESET },
	{ V4L2_WHITE_BALANCE_HORIZON, GC2145_REG_WB_MODE_HORIZON_PRESET },
	{ V4L2_WHITE_BALANCE_CLOUDY, GC2145_REG_WB_MODE_CLOUDY_PRESET },
	{ V4L2_WHITE_BALANCE_DAYLIGHT, GC2145_REG_WB_MODE_SUNNY_PRESET },
	{ V4L2_WHITE_BALANCE_AUTO, GC2145_REG_WB_MODE_AUTOMATIC },
};

static int gc2145_set_power_line_frequency(struct gc2145_dev *sensor, s32 val)
{
	u16 freq;
	int ret;

	switch (val) {
	case V4L2_CID_POWER_LINE_FREQUENCY_DISABLED:
		ret = gc2145_write(sensor, GC2145_REG_ANTI_FLICKER_MODE, 0);
		if (ret)
			return ret;

		return gc2145_write(sensor, GC2145_REG_FD_ENABLE_DETECT, 0);
	case V4L2_CID_POWER_LINE_FREQUENCY_50HZ:
	case V4L2_CID_POWER_LINE_FREQUENCY_60HZ:
		ret = gc2145_write(sensor, GC2145_REG_ANTI_FLICKER_MODE, 1);
		if (ret)
			return ret;

		ret = gc2145_write(sensor, GC2145_REG_FD_ENABLE_DETECT, 0);
		if (ret)
			return ret;

		freq = (val == V4L2_CID_POWER_LINE_FREQUENCY_50HZ) ?
			0x4b20 : 0x4bc0;

		return gc2145_write16(sensor, GC2145_REG_FD_FLICKER_FREQUENCY,
				      freq);
	case V4L2_CID_POWER_LINE_FREQUENCY_AUTO:
		ret = gc2145_write(sensor, GC2145_REG_FD_ENABLE_DETECT, 1);
		if (ret)
			return ret;

		ret = gc2145_write(sensor, GC2145_REG_ANTI_FLICKER_MODE, 1);
		if (ret)
			return ret;

		ret = gc2145_write16(sensor, GC2145_REG_FD_MAX_NUMBER_ATTEMP,
				     100);
		if (ret)
			return ret;

		ret = gc2145_write16(sensor, GC2145_REG_FD_FLICKER_FREQUENCY,
				     0);
		if (ret)
			return ret;

		return gc2145_write(sensor, GC2145_REG_FD_DETECTION_START, 1);
	default:
		return -EINVAL;
	}
}

static int gc2145_set_colorfx(struct gc2145_dev *sensor, s32 val)
{
	int ret;

	ret = gc2145_write(sensor, GC2145_REG_EFFECTS_COLOR,
			   GC2145_REG_EFFECTS_COLOR_NORMAL);
	if (ret)
		return ret;

	ret = gc2145_write(sensor, GC2145_REG_EFFECTS_NEGATIVE, 0);
	if (ret)
		return ret;

	ret = gc2145_write(sensor, GC2145_REG_EFFECTS_SOLARISING, 0);
	if (ret)
		return ret;

	ret = gc2145_write(sensor, GC2145_REG_EFFECTS_SKECTH, 0);
	if (ret)
		return ret;

	switch (val) {
	case V4L2_COLORFX_NONE:
		return 0;
	case V4L2_COLORFX_NEGATIVE:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_NEGATIVE, 1);
	case V4L2_COLORFX_SOLARIZATION:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_SOLARISING, 1);
	case V4L2_COLORFX_SKETCH:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_SKECTH, 1);
	case V4L2_COLORFX_ANTIQUE:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_COLOR,
				    GC2145_REG_EFFECTS_COLOR_ANTIQUE);
	case V4L2_COLORFX_SEPIA:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_COLOR,
				    GC2145_REG_EFFECTS_COLOR_SEPIA);
	case V4L2_COLORFX_AQUA:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_COLOR,
				    GC2145_REG_EFFECTS_COLOR_AQUA);
	case V4L2_COLORFX_BW:
		return gc2145_write(sensor, GC2145_REG_EFFECTS_COLOR,
				    GC2145_REG_EFFECTS_COLOR_BLACK_WHITE);
	default:
		return -EINVAL;
	}
}

static int gc2145_3a_lock(struct gc2145_dev *sensor, struct v4l2_ctrl *ctrl)
{
	bool awb_lock = ctrl->val & V4L2_LOCK_WHITE_BALANCE;
	bool ae_lock = ctrl->val & V4L2_LOCK_EXPOSURE;
	int ret = 0;

	if ((ctrl->val ^ ctrl->cur.val) & V4L2_LOCK_EXPOSURE
	    && sensor->ctrls.auto_exposure->val == V4L2_EXPOSURE_AUTO) {
		ret = gc2145_write(sensor, GC2145_REG_FREEZE_AUTO_EXPOSURE,
				   ae_lock);
		if (ret)
			return ret;
	}

	if (((ctrl->val ^ ctrl->cur.val) & V4L2_LOCK_WHITE_BALANCE)
	    && sensor->ctrls.wb->val == V4L2_WHITE_BALANCE_AUTO) {
		ret = gc2145_write(sensor, GC2145_REG_WB_MISC_SETTINGS,
				   awb_lock ?
				   GC2145_REG_WB_MISC_SETTINGS_FREEZE_ALGO : 0);
		if (ret)
			return ret;
	}

	return ret;
}

static int gc2145_set_white_balance(struct gc2145_dev *sensor)
{
	struct gc2145_ctrls *ctrls = &sensor->ctrls;
	bool manual_wb = ctrls->wb->val == V4L2_WHITE_BALANCE_MANUAL;
	int ret = 0, i;
	s32 val;

	if (ctrls->wb->is_new) {
		for (i = 0; i < ARRAY_SIZE(gc2145_wb_opts); i++) {
			if (gc2145_wb_opts[i][0] != ctrls->wb->val)
				continue;

			ret = gc2145_write(sensor, GC2145_REG_WB_MODE,
					    gc2145_wb_opts[i][1]);
			if (ret)
				return ret;
			goto next;
		}

		return -EINVAL;
	}

next:
	if (ctrls->wb->is_new || ctrls->blue_balance->is_new) {
		val = manual_wb ? ctrls->blue_balance->val : 1000;
		ret = gc2145_write16(sensor, GC2145_REG_WB_HUE_B_BIAS,
				     gc2145_mili_to_fp16(val));
		if (ret)
			return ret;
	}

	if (ctrls->wb->is_new || ctrls->red_balance->is_new) {
		val = manual_wb ? ctrls->red_balance->val : 1000;
		ret = gc2145_write16(sensor, GC2145_REG_WB_HUE_R_BIAS,
				     gc2145_mili_to_fp16(val));
	}

	return ret;
}

#endif

/* Exposure */

static int gc2145_get_exposure(struct gc2145_dev *sensor)
{
	struct gc2145_ctrls *ctrls = &sensor->ctrls;
	u8 again, dgain;
	u16 exp;
	int ret;

	ret = gc2145_read(sensor, 0xb1, &again);
	if (ret)
		return ret;

	ret = gc2145_read(sensor, 0xb2, &dgain);
	if (ret)
		return ret;

	ret = gc2145_read16(sensor, 0x03, &exp);
	if (ret)
		return ret;

	ctrls->exposure->val = exp;
	ctrls->d_gain->val = dgain;
	ctrls->a_gain->val = again;

	return 0;
}

#define AE_BIAS_MENU_DEFAULT_VALUE_INDEX 4
static const s64 ae_bias_menu_values[] = {
	-4000, -3000, -2000, -1000, 0, 1000, 2000, 3000, 4000
};

static const s8 ae_bias_menu_reg_values[] = {
	0x55, 0x60, 0x65, 0x70, 0x7b, 0x85, 0x90, 0x95, 0xa0
};

static int gc2145_set_exposure(struct gc2145_dev *sensor)
{
	struct gc2145_ctrls *ctrls = &sensor->ctrls;
	bool is_auto = (ctrls->auto_exposure->val != V4L2_EXPOSURE_MANUAL);

	gc2145_tx_start(sensor);

	if (ctrls->auto_exposure->is_new) {
		gc2145_tx_write8(sensor, 0xb6, is_auto ? 1 : 0);

		//XXX: remove?
		//if (ctrls->auto_exposure->cur.val != ctrls->auto_exposure->val &&
		    //!is_auto) {
			/*
			 * Hack: At this point, there are current volatile
			 * values in val, but control framework will not
			 * update the cur values for our autocluster, as it
			 * should. I couldn't find the reason. This fixes
			 * it for our driver. Remove this after the kernel
			 * is fixed.
			 */
			//ctrls->exposure->cur.val = ctrls->exposure->val;
			//ctrls->d_gain->cur.val = ctrls->d_gain->val;
			//ctrls->a_gain->cur.val = ctrls->a_gain->val;
		//}
	}

	if (!is_auto && ctrls->exposure->is_new)
		gc2145_tx_write16(sensor, 0x03, ctrls->exposure->val);

	if (!is_auto && ctrls->d_gain->is_new)
		gc2145_tx_write8(sensor, 0xb2, ctrls->d_gain->val);

	if (!is_auto && ctrls->a_gain->is_new)
		gc2145_tx_write8(sensor, 0xb1, ctrls->a_gain->val);

	return gc2145_tx_commit(sensor);;
}

/* Test patterns */

enum {
	GC2145_TEST_PATTERN_DISABLED,
	GC2145_TEST_PATTERN_VGA_COLOR_BARS,
	GC2145_TEST_PATTERN_UXGA_COLOR_BARS,
	GC2145_TEST_PATTERN_SKIN_MAP,
	GC2145_TEST_PATTERN_SOLID_COLOR,
};

static const char * const test_pattern_menu[] = {
	"Disabled",
	"VGA color bars",
	"UXGA color bars",
	"Skin map",
	"Solid black color",
	"Solid light gray color",
	"Solid gray color",
	"Solid dark gray color",
	"Solid white color",
	"Solid red color",
	"Solid green color",
	"Solid blue color",
	"Solid yellow color",
	"Solid cyan color",
	"Solid magenta color",
};

static int gc2145_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	int ret;

	/* v4l2_ctrl_lock() locks our own mutex */

	if (!sensor->powered)
		return -EIO;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		ret = gc2145_get_exposure(sensor);
		if (ret)
			return ret;
		break;
	default:
		dev_err(&sensor->i2c_client->dev, "getting unknown control %d\n", ctrl->id);
		return -EINVAL;
	}

	return 0;
}

static int gc2145_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	struct gc2145_ctrls *ctrls = &sensor->ctrls;
	s32 val = ctrl->val;
	unsigned int i;
	int ret;
	u8 test1, test2;

	/* v4l2_ctrl_lock() locks our own mutex */

	/*
	 * If the device is not powered up by the host driver do
	 * not apply any controls to H/W at this time. Instead
	 * the controls will be restored right after power-up.
	 */
	if (!sensor->powered)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE_AUTO:
		return gc2145_set_exposure(sensor);

	case V4L2_CID_AUTO_EXPOSURE_BIAS:
		if (val < 0 || val >= ARRAY_SIZE(ae_bias_menu_reg_values)) {
			dev_err(&sensor->i2c_client->dev, "ae bias out of range\n");
			return -EINVAL;
		}

		return gc2145_write(sensor, 0x113,
				    (u8)ae_bias_menu_reg_values[val]);

	case V4L2_CID_VFLIP:
		return gc2145_update_bits(sensor, 0x17, BIT(1), val ? BIT(1) : 0);

	case V4L2_CID_HFLIP:
		return gc2145_update_bits(sensor, 0x17, BIT(0), val ? BIT(0) : 0);

	case V4L2_CID_TEST_PATTERN:
		for (i = 0; i < ARRAY_SIZE(ctrls->test_data); i++)
			v4l2_ctrl_activate(ctrls->test_data[i],
					   val == 6); /* solid color */

		test1 = 0;
		test2 = 0x01;

		if (val == GC2145_TEST_PATTERN_VGA_COLOR_BARS)
			test1 = 0x04;
		else if (val == GC2145_TEST_PATTERN_UXGA_COLOR_BARS)
			test1 = 0x44;
		else if (val == GC2145_TEST_PATTERN_SKIN_MAP)
			test1 = 0x10;
		else if (val >= GC2145_TEST_PATTERN_SOLID_COLOR) {
			test1 = 0x04;
			test2 = ((val - GC2145_TEST_PATTERN_SOLID_COLOR) << 4) | 0x8;
		} else if (val != GC2145_TEST_PATTERN_DISABLED) {
			dev_err(&sensor->i2c_client->dev, "test pattern out of range\n");
			return -EINVAL;
		}

		ret = gc2145_write(sensor, 0x8c, test1);
		if (ret)
			return ret;

		return gc2145_write(sensor, 0x8d, test2);

#if 0
	case V4L2_CID_EXPOSURE_METERING:
		if (val == V4L2_EXPOSURE_METERING_AVERAGE)
			reg = GC2145_REG_EXPOSURE_METERING_FLAT;
		else if (val == V4L2_EXPOSURE_METERING_CENTER_WEIGHTED)
			reg = GC2145_REG_EXPOSURE_METERING_CENTERED;
		else
			return -EINVAL;

		return gc2145_write(sensor, GC2145_REG_EXPOSURE_METERING, reg);

	case V4L2_CID_CONTRAST:
		return gc2145_write(sensor, GC2145_REG_CONTRAST, val);

	case V4L2_CID_SATURATION:
		return gc2145_write(sensor, GC2145_REG_COLOR_SATURATION, val);

	case V4L2_CID_BRIGHTNESS:
		return gc2145_write(sensor, GC2145_REG_BRIGHTNESS, val);

	case V4L2_CID_POWER_LINE_FREQUENCY:
		return gc2145_set_power_line_frequency(sensor, val);

	case V4L2_CID_GAMMA:
		return gc2145_write(sensor, GC2145_REG_P0_GAMMA_GAIN, val);

	case V4L2_CID_COLORFX:
		return gc2145_set_colorfx(sensor, val);

	case V4L2_CID_3A_LOCK:
		return gc2145_3a_lock(sensor, ctrl);

	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		return gc2145_set_white_balance(sensor);

	case V4L2_CID_TEST_PATTERN_RED:
		return gc2145_write16(sensor, GC2145_REG_TESTDATA_RED, val);

	case V4L2_CID_TEST_PATTERN_GREENR:
		return gc2145_write16(sensor, GC2145_REG_TESTDATA_GREEN_R, val);

	case V4L2_CID_TEST_PATTERN_BLUE:
		return gc2145_write16(sensor, GC2145_REG_TESTDATA_BLUE, val);

	case V4L2_CID_TEST_PATTERN_GREENB:
		return gc2145_write16(sensor, GC2145_REG_TESTDATA_GREEN_B, val);

#endif
	default:
		dev_err(&sensor->i2c_client->dev, "setting unknown control %d\n", ctrl->id);
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops gc2145_ctrl_ops = {
	.g_volatile_ctrl = gc2145_g_volatile_ctrl,
	.s_ctrl = gc2145_s_ctrl,
};

static int gc2145_init_controls(struct gc2145_dev *sensor)
{
	const struct v4l2_ctrl_ops *ops = &gc2145_ctrl_ops;
	struct gc2145_ctrls *ctrls = &sensor->ctrls;
	struct v4l2_ctrl_handler *hdl = &ctrls->handler;
	//u8 wb_max = 0;
	//u64 wb_mask = 0;
	//unsigned int i;
	int ret;

	v4l2_ctrl_handler_init(hdl, 32);

	/* we can use our own mutex for the ctrl lock */
	hdl->lock = &sensor->lock;

	/* Exposure controls */
	ctrls->auto_exposure = v4l2_ctrl_new_std_menu(hdl, ops,
						      V4L2_CID_EXPOSURE_AUTO,
						      V4L2_EXPOSURE_MANUAL, 0,
						      V4L2_EXPOSURE_AUTO);
	ctrls->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
					    1, 0x1fff, 1, 0x80);
	ctrls->a_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_ANALOGUE_GAIN,
					  0, 255, 1, 0x20);
	ctrls->d_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_DIGITAL_GAIN,
					  0, 255, 1, 0x40);
	ctrls->exposure_bias =
		v4l2_ctrl_new_int_menu(hdl, ops, V4L2_CID_AUTO_EXPOSURE_BIAS,
				       ARRAY_SIZE(ae_bias_menu_values) - 1,
				       AE_BIAS_MENU_DEFAULT_VALUE_INDEX,
				       ae_bias_menu_values);

	/* V/H flips */
	ctrls->hflip = v4l2_ctrl_new_std(hdl, ops,
					 V4L2_CID_HFLIP, 0, 1, 1, 0);
	ctrls->vflip = v4l2_ctrl_new_std(hdl, ops,
					 V4L2_CID_VFLIP, 0, 1, 1, 0);


	/* Test patterns */
	ctrls->test_pattern =
		v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
					     ARRAY_SIZE(test_pattern_menu) - 1,
					     0, 0, test_pattern_menu);
#if 0

	ctrls->metering =
		v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_EXPOSURE_METERING,
				       V4L2_EXPOSURE_METERING_CENTER_WEIGHTED,
				       0, V4L2_EXPOSURE_METERING_AVERAGE);

	for (i = 0; i < ARRAY_SIZE(gc2145_wb_opts); i++) {
		if (wb_max < gc2145_wb_opts[i][0])
			wb_max = gc2145_wb_opts[i][0];
		wb_mask |= BIT(gc2145_wb_opts[i][0]);
	}

	ctrls->wb = v4l2_ctrl_new_std_menu(hdl, ops,
			V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
			wb_max, ~wb_mask, V4L2_WHITE_BALANCE_AUTO);

	ctrls->blue_balance = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_BLUE_BALANCE,
						0, 4000, 1, 1000);
	ctrls->red_balance = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_RED_BALANCE,
					       0, 4000, 1, 1000);

	ctrls->gamma = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_GAMMA,
					 0, 31, 1, 20);

	ctrls->colorfx =
		v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_COLORFX, 15,
				       ~(BIT(V4L2_COLORFX_NONE) |
					 BIT(V4L2_COLORFX_NEGATIVE) |
					 BIT(V4L2_COLORFX_SOLARIZATION) |
					 BIT(V4L2_COLORFX_SKETCH) |
					 BIT(V4L2_COLORFX_SEPIA) |
					 BIT(V4L2_COLORFX_ANTIQUE) |
					 BIT(V4L2_COLORFX_AQUA) |
					 BIT(V4L2_COLORFX_BW)),
				       V4L2_COLORFX_NONE);

	ctrls->pl_freq =
		v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_POWER_LINE_FREQUENCY,
				V4L2_CID_POWER_LINE_FREQUENCY_AUTO, 0,
				V4L2_CID_POWER_LINE_FREQUENCY_50HZ);

	ctrls->brightness = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_BRIGHTNESS,
					      0, 200, 1, 90);
	ctrls->saturation = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_SATURATION,
					      0, 200, 1, 110);
	ctrls->contrast = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_CONTRAST,
					    0, 200, 1, 108);

	ctrls->aaa_lock = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_3A_LOCK,
					    0, 0x7, 0, 0);

	for (i = 0; i < ARRAY_SIZE(ctrls->test_data); i++)
		ctrls->test_data[i] =
			v4l2_ctrl_new_std(hdl, ops,
					  V4L2_CID_TEST_PATTERN_RED + i,
					  0, 1023, 1, 0);

	ctrls->af_status->flags |= V4L2_CTRL_FLAG_VOLATILE |
		V4L2_CTRL_FLAG_READ_ONLY;

	v4l2_ctrl_auto_cluster(3, &ctrls->wb, V4L2_WHITE_BALANCE_MANUAL, false);
#endif

	v4l2_ctrl_auto_cluster(4, &ctrls->auto_exposure, V4L2_EXPOSURE_MANUAL,
			       true);

	if (hdl->error) {
		ret = hdl->error;
		goto free_ctrls;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdl);
	return ret;
}

/* }}} */
/* {{{ Video ops */

static int gc2145_g_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);

	if (fi->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);
	fi->interval = sensor->frame_interval;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int gc2145_s_frame_interval(struct v4l2_subdev *sd,
				   struct v4l2_subdev_frame_interval *fi)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	int ret = 0, fps;

	if (fi->pad != 0)
		return -EINVAL;

	mutex_lock(&sensor->lock);

	/* user requested infinite frame rate */
	if (fi->interval.numerator == 0)
		fps = 60;
	else
		fps = DIV_ROUND_CLOSEST(fi->interval.denominator,
					fi->interval.numerator);

	fps = clamp(fps, 1, 60);

	sensor->frame_interval.numerator = 1;
	sensor->frame_interval.denominator = fps;
	fi->interval = sensor->frame_interval;

#if 0
	if (sensor->streaming) {
		ret = gc2145_write16(sensor, GC2145_REG_DESIRED_FRAME_RATE_NUM,
				     fps);
		if (ret)
			goto err_unlock;
	}
err_unlock:
#endif

	mutex_unlock(&sensor->lock);
	return ret;
}

/*
 * Clock tree
 * ----------
 *
 *     MCLK pin
 *         |
 *  DIV2 (optional)        - Divide input MCLK by 2 when 0xf7[1] == 1
 *         |
 *   /- PLL mux -\         - PLL selected by 0xf8[7], otherwise fixed 32x mult
 *   |           |
 * PLL           |         - PLL multiplies by 0xf8[5:0]+1 * 4
 *   |      Fixed 32/48x   - Multiplies 32x when 0xf7[2] == 1 otherwise 48x
 *    \_________/
 *         |
 *       DOUBLE  (div by 4 or 8) based on 0xf7[3]
 *         |
 *     /-------\
 *     |       |
 * pclk_div  sclk_div
 *     |       |
 *   2pclk    sclk
 */
__maybe_unused
static int gc2145_get_2pclk(struct gc2145_dev *sensor, unsigned long* pclk)
{
	u8 pll_mode1, pll_mode2, clk_div_mode;
	bool mclk_div2_en; // 0xf7[1]
	bool pll_en; // 0xf8[7]
	bool double_clk; // 0xf7[3]
	bool fixed_32x; // 0xf7[2]
	unsigned long pll_mult; // 0xf8[5:0] + 1
	unsigned long sclk_div; // 1 << (0xf7[5:4] + 1)
	unsigned long pclk_div; // 0xfa[7:4] + 1
	unsigned long int_clk;
	unsigned long mclk;
        int ret;

	ret = gc2145_read(sensor, 0xf7, &pll_mode1);
	if (ret)
		return ret;

	ret = gc2145_read(sensor, 0xf8, &pll_mode2);
	if (ret)
		return ret;

	ret = gc2145_read(sensor, 0xfa, &clk_div_mode);
	if (ret)
		return ret;

	mclk = clk_get_rate(sensor->xclk);
	if (mclk == 0)
		return -EINVAL;

	mclk_div2_en = pll_mode1 & BIT(1);
	pll_en = pll_mode2 & BIT(7);
	double_clk = pll_mode1 & BIT(3);
	fixed_32x = pll_mode1 & BIT(2);
	pll_mult = (pll_mode2 & 0x3f) + 1;
	pclk_div = (clk_div_mode >> 4) + 1;
	sclk_div = 1 << (((pll_mode1 >> 4) & 0x3) + 1);

	int_clk = mclk / (mclk_div2_en ? 2 : 1);

	if (pll_en)
		int_clk *= pll_mult * 4;
	else
		int_clk *= fixed_32x ? 32 : 48;

	int_clk /= double_clk ? 4 : 8;

	if (pclk)
		*pclk = int_clk / pclk_div;

	return 0;
}

static int gc2145_set_2pclk(struct gc2145_dev *sensor,
			    unsigned long *freq, bool apply)
{
	unsigned long pll_mult, pll_mult_max, /*sclk_div,*/ pclk_div, pclk2,/* sclk,*/
		      mclk;
	unsigned long pll_mult_best = 0, pclk_div_best = 0, diff_best = ULONG_MAX, diff,
		      pclk2_best = 0;
	int mclk_div2_en; //, double_clk;
	int mclk_div2_en_best = 0; //, double_clk_best;

	mclk = clk_get_rate(sensor->xclk);
	if (mclk == 0)
		return -EINVAL;

        for (mclk_div2_en = 0; mclk_div2_en <= 1; mclk_div2_en++) {
		pll_mult_max = 768000000 / 4 / (mclk / (mclk_div2_en ? 2 : 1));
		if (pll_mult_max > 32)
			pll_mult_max = 32;

		for (pll_mult = 2; pll_mult <= pll_mult_max; pll_mult++) {
			for (pclk_div = 1; pclk_div <= 8; pclk_div++) {
				pclk2 = mclk / (mclk_div2_en ? 2 : 1) * pll_mult / pclk_div;

				if (pclk2 > *freq)
					continue;

				diff = *freq - pclk2;

				if (diff < diff_best) {
					diff_best = diff;
					pclk2_best = pclk2;

					pll_mult_best = pll_mult;
					pclk_div_best = pclk_div;
					mclk_div2_en_best = mclk_div2_en;
				}

				if (diff == 0)
					goto found;
			}
		}
	}

	if (diff_best == ULONG_MAX)
		return -1;

found:
	*freq = pclk2_best;
	if (!apply)
		return 0;

	gc2145_tx_start(sensor);

	gc2145_tx_write8(sensor, 0xf7,
			 ((pclk_div_best - 1)) << 4 |
			 (mclk_div2_en_best << 1) | BIT(0) /* pll_en */);
	gc2145_tx_write8(sensor, 0xf8, BIT(7) | (pll_mult_best - 1));
	gc2145_tx_write8(sensor, 0xfa,
			 (pclk_div_best - 1) << 4 |
			 (((pclk_div_best - 1) / 2) & 0xf));

	return gc2145_tx_commit(sensor);
}

static int gc2145_setup_awb(struct gc2145_dev *sensor,
			     u16 x1, u16 y1, u16 x2, u16 y2)
{
	int ratio = 8; //XXX: manual for gc2035 FAE says 4

	gc2145_tx_start(sensor);

	// disable awb
	gc2145_tx_update_bits(sensor, 0x82, BIT(1), 0);

	// reset white balance RGB gains
	gc2145_tx_write8(sensor, 0xb3, 0x40);
	gc2145_tx_write8(sensor, 0xb4, 0x40);
	gc2145_tx_write8(sensor, 0xb5, 0x40);

	// awb window
	gc2145_tx_write8(sensor, 0x1ec, x1 / ratio);
	gc2145_tx_write8(sensor, 0x1ed, y1 / ratio);
	gc2145_tx_write8(sensor, 0x1ee, x2 / ratio);
	gc2145_tx_write8(sensor, 0x1ef, y2 / ratio);

	// eanble awb
	gc2145_tx_update_bits(sensor, 0x82, BIT(1), BIT(1));

	//1051  { 0xfe, 0x01 },
	//1052  { 0x74, 0x01 },

	return gc2145_tx_commit(sensor);
}

static int gc2145_setup_aec(struct gc2145_dev *sensor,
			     u16 x1, u16 y1, u16 x2, u16 y2,
			     u16 cx1, u16 cy1, u16 cx2, u16 cy2)
{
        u16 x_ratio = 8;

	//XXX: gc2035 has x ratio 16
	//XXX: gc2035 doesn't have low light mode
	gc2145_tx_start(sensor);

	// disable AEC
	gc2145_tx_write8(sensor, 0xb6, 0);

	// set reasonable initial exposure and gains
	gc2145_tx_write16(sensor, 0x03, 1200);
	gc2145_tx_write8(sensor, 0xb1, 0x20);
	gc2145_tx_write8(sensor, 0xb2, 0xe0);

	// setup measure window
	gc2145_tx_write8(sensor, 0x101, x1 / x_ratio);
	gc2145_tx_write8(sensor, 0x102, x2 / x_ratio);
	gc2145_tx_write8(sensor, 0x103, y1 / 8);
	gc2145_tx_write8(sensor, 0x104, y2 / 8);

	// setup center
	gc2145_tx_write8(sensor, 0x105, cx1 / x_ratio);
	gc2145_tx_write8(sensor, 0x106, cx2 / x_ratio);
	gc2145_tx_write8(sensor, 0x107, cy1 / 8);
	gc2145_tx_write8(sensor, 0x108, cy2 / 8);

	// increase maximum exposure level to 4
	//gc2145_tx_write8(sensor, 0x13c, 0x60);
	// setup AEC mode: measure point, adjust_max_gain, skip_mode = 2
	//gc2145_tx_write8(sensor, 0x10a, 0xc2);

	// AEC_ASDE_select_luma_value AEC_low_light_exp_THD_max:
	//gc2145_tx_write8(sensor, 0x121, 0x15);

	// enable AEC again
	gc2145_tx_write8(sensor, 0xb6, 1);

	return gc2145_tx_commit(sensor);
}

struct gc2145_sensor_params {
	unsigned int enable_scaler;
	unsigned int col_scaler_only;
	unsigned int row_skip;
	unsigned int col_skip;
	unsigned long sh_delay;
	unsigned long hb;
	unsigned long vb;
	unsigned long st;
	unsigned long et;
	unsigned long win_width;
	unsigned long win_height;
	unsigned long width;
	unsigned long height;
};

static void gc2145_sensor_params_init(struct gc2145_sensor_params* p, int width, int height)
{
	p->win_height = height + 32;
	p->win_width = (width + 16);
	p->width = width;
	p->height = height;
	p->st = 2;
	p->et = 2;
	p->vb = 8;
	p->hb = 0x1f0;
	p->sh_delay = 30;
}

// unit is PCLK periods
static unsigned long
gc2145_sensor_params_get_row_period(struct gc2145_sensor_params* p)
{
	return 2 * (p->win_width / 2 / (p->col_skip + 1) + p->sh_delay + p->hb + 4);
}

static unsigned long
gc2145_sensor_params_get_frame_period(struct gc2145_sensor_params* p)
{
	unsigned long rt = gc2145_sensor_params_get_row_period(p);

	return rt * (p->vb + p->win_height) / (p->row_skip + 1);
}

static void
gc2145_sensor_params_fit_hb_to_power_line_period(struct gc2145_sensor_params* p,
					  unsigned long power_line_freq,
					  unsigned long pclk)
{
	unsigned long rt, power_line_ratio;

        for (p->hb = 0x1f0; p->hb < 2047; p->hb++) {
		rt = gc2145_sensor_params_get_row_period(p);

		// power_line_ratio is row_freq / power_line_freq * 1000
                power_line_ratio = pclk / power_line_freq * 1000 / rt;

		// if we're close enough, stop the search
                if (power_line_ratio % 1000 < 50)
                        break;
        }

	// finding the optimal Hb is not critical
	if (p->hb == 2047)
		p->hb = 0x1f0;
}

static void
gc2145_sensor_params_fit_vb_to_frame_period(struct gc2145_sensor_params* p,
				     unsigned long frame_period)
{
	unsigned long rt, fp;

	p->vb = 8;
	rt = gc2145_sensor_params_get_row_period(p);
	fp = gc2145_sensor_params_get_frame_period(p);

	if (frame_period > fp)
		p->vb = frame_period * (p->row_skip + 1) / rt - p->win_height;

	if (p->vb > 4095)
		p->vb = 4095;
}

static int gc2145_sensor_params_apply(struct gc2145_dev *sensor,
				      struct gc2145_sensor_params* p)
{
	u32 off_x = (GC2145_SENSOR_WIDTH_MAX - p->width) / 2;
	u32 off_y = (GC2145_SENSOR_HEIGHT_MAX - p->height) / 2;

	gc2145_tx_start(sensor);

	gc2145_tx_write8(sensor, 0xfd, (p->enable_scaler ? BIT(0) : 0)
			| (p->col_scaler_only ? BIT(1) : 0));

	gc2145_tx_write8(sensor, 0x18, 0x0a
		       | (p->col_skip ? BIT(7) : 0)
		       | (p->row_skip ? BIT(6) : 0));

	gc2145_tx_write16(sensor, 0x09, off_y);
	gc2145_tx_write16(sensor, 0x0b, off_x);
	gc2145_tx_write16(sensor, 0x0d, p->win_height);
	gc2145_tx_write16(sensor, 0x0f, p->win_width);
	gc2145_tx_write16(sensor, 0x05, p->hb);
	gc2145_tx_write16(sensor, 0x07, p->vb);
	gc2145_tx_write16(sensor, 0x11, p->sh_delay);

	gc2145_tx_write8(sensor, 0x13, p->st);
	gc2145_tx_write8(sensor, 0x14, p->et);

	return gc2145_tx_commit(sensor);
}

static int gc2145_setup_mode(struct gc2145_dev *sensor)
{
	int scaling_desired, ret, pad;
	struct gc2145_sensor_params params = {0};
	unsigned long pclk2, frame_period;
	unsigned long power_line_freq = 50;
	unsigned long width = sensor->fmt.width;
	unsigned long height = sensor->fmt.height;
	unsigned long framerate = sensor->frame_interval.denominator;
	const struct gc2145_pixfmt *pix_fmt;

	pix_fmt = gc2145_find_format(sensor->fmt.code);
	if (!pix_fmt) {
		dev_err(&sensor->i2c_client->dev,
			"pixel format not supported %u\n", sensor->fmt.code);
		return -EINVAL;
	}

        /*
	 * Equations for calculating framerate are:
	 *
	 *    ww = width + 16
	 *    wh = height + 32
	 *    Rt = (ww / 2 / (col_skip + 1) + sh_delay + Hb + 4)
	 *    Ft = Rt * (Vb + wh) / (row_skip + 1)
	 *    framerate = 2pclk / 4 / Ft
	 *
	 * Based on these equations:
	 *
	 * 1) First we need to determine what 2PCLK frequency to use. The 2PCLK
	 *    frequency is not arbitrarily precise, so we need to calculate the
	 *    actual frequency used, after setting our target frequency.
	 *
	 *    We use a simple heuristic:
	 *
	 *      If pixel_count * 2 * framerate * 1.15 is > 40MHz, we use 60MHz,
	 *      otherwise we use 40MHz.
	 *
	 * 2) We want to determine lowest Hb that we can use to extend row
	 *    period so that row time takes an integer fraction of the power
	 *    line frequency period. Minimum Hb is 0x1f0.
	 *
	 * 3) If the requested resolution is less than half the sensor's size,
	 *    we'll use scaling, or row skipping + column scaling, or row and
	 *    column skiping, depending on what allows us to achieve the
	 *    requested framerate.
         *
	 * 4) We use the selected Hb to calculate Vb value that will give
	 *    us the desired framerate, given the scaling/skipping option
	 *    selected in 3).
	 */

	scaling_desired = width <= GC2145_SENSOR_WIDTH_MAX / 2
			&& height <= GC2145_SENSOR_HEIGHT_MAX / 2;

	pclk2 = 60000000;

	ret = gc2145_set_2pclk(sensor, &pclk2, false);
	if (ret < 0)
		return ret;

	gc2145_sensor_params_init(&params, width, height);

	// if the resolution is < half the sensor size, enable the scaler
	// to cover more area of the chip
	if (scaling_desired) {
		params.enable_scaler = 1;
		pclk2 *= 2;
		gc2145_sensor_params_init(&params, width * 2, height * 2);
	}

	// we need to call this each time pclk or power_line_freq is changed
	gc2145_sensor_params_fit_hb_to_power_line_period(&params,
							 power_line_freq,
							 pclk2 / 2);

	frame_period = gc2145_sensor_params_get_frame_period(&params);
	if (framerate <= pclk2 / 2 / frame_period)
		goto apply;

	if (scaling_desired) {
		// try using just the column scaler + row skip
		params.col_scaler_only = 1;
		params.row_skip = 1;
		gc2145_sensor_params_fit_hb_to_power_line_period(&params,
								 power_line_freq,
								 pclk2 / 2);

		frame_period = gc2145_sensor_params_get_frame_period(&params);
		if (framerate <= pclk2 / 2 / frame_period)
			goto apply;


		/*
		// try disabling the scaler and just use skipping
		params.enable_scaler = 0;
		pclk2 /= 2;
		params.col_scaler_only = 0;
		params.col_skip = 1;
		gc2145_sensor_params_fit_hb_to_power_line_period(&params, power_line_freq, pclk2 / 2);

		frame_period = gc2145_sensor_params_get_frame_period(&params);

		if (framerate <= pclk2 / 2 / frame_period)
			goto apply;
                  */
	}

apply:
        // adjust vb to fit the target framerate
	gc2145_sensor_params_fit_vb_to_frame_period(&params,
						    pclk2 / 2 / framerate);

	gc2145_sensor_params_apply(sensor, &params);

	ret = gc2145_set_2pclk(sensor, &pclk2, true);
	if (ret < 0)
		return ret;

	pad = (width > 256 && height > 256) ? 32 : 16;

	ret = gc2145_setup_awb(sensor, pad, pad, width - pad * 2, height - pad * 2);
	if (ret)
		return ret;

	ret = gc2145_setup_aec(sensor,
				pad, pad, width - pad * 2, height - pad * 2,
				2 * pad, 2 * pad, width - pad * 4, height - pad * 4);
	if (ret)
		return ret;

	gc2145_tx_start(sensor);

	// anti-flicker step
	//gc2145_tx_write16(sensor, 0x125, 360); //XXX: get this from the calculator (hb related)

	//XXX: calculate auto exposure settings, there are 4 slots that the HW
	//uses and exposure settings are set in row_time units

	unsigned long rt = gc2145_sensor_params_get_row_period(&params);
	unsigned long ft = gc2145_sensor_params_get_frame_period(&params);
	unsigned long ft_rt = ft / rt / 4;
	int i;

	for (i = 0; i < 7; i++) {
		// exposure settings for exposure levels
		gc2145_tx_write16(sensor, 0x127 + 2 * i, ft_rt * (i + 1));
		// max dg gains
		gc2145_tx_write8(sensor, 0x135 + i, 0x50);
	}

	// max analog gain
	gc2145_tx_write8(sensor, 0x11f, 0x50);
	// max digital gain
	gc2145_tx_write8(sensor, 0x120, 0xe0);

	gc2145_tx_write8(sensor, GC2145_P0_ISP_OUT_FORMAT, pix_fmt->fmt_setup);

	// set gamma curve
	gc2145_tx_update_bits(sensor, 0x80, BIT(6), BIT(6));

	// disable denoising
	gc2145_tx_update_bits(sensor, 0x80, BIT(2), 0);

	// drive strength
	gc2145_tx_write8(sensor, 0x24,
			 (pclk2 / (params.enable_scaler + 1)) > 40000000 ?
				0xff : 0x55);

	return gc2145_tx_commit(sensor);
}

static int gc2145_set_stream(struct gc2145_dev *sensor, int enable)
{
	gc2145_tx_start(sensor);

	gc2145_tx_write8(sensor, GC2145_REG_PAD_IO, enable ? 0x0f : 0);

	//XXX: maybe disable cam module function blocks that are not used
	//and downclock the PLL/disable it when not streaming?

	return gc2145_tx_commit(sensor);
}

static int gc2145_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	int ret = 0;

	mutex_lock(&sensor->lock);

	if (sensor->streaming == !enable) {
		if (enable && sensor->pending_mode_change) {
			ret = gc2145_setup_mode(sensor);
			if (ret)
				goto out;
		}

		ret = gc2145_set_stream(sensor, enable);
		if (ret)
			goto out;

		sensor->streaming = !!enable;
	}

out:
	mutex_unlock(&sensor->lock);
	return ret;
}

/* }}} */
/* {{{ Pad ops */

static int gc2145_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad != 0 || code->index >= ARRAY_SIZE(gc2145_formats))
		return -EINVAL;

	code->code = gc2145_formats[code->index].code;

	return 0;
}

static int gc2145_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_pad_config *cfg,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->pad != 0 || fse->index > 0)
		return -EINVAL;

	fse->min_width = GC2145_SENSOR_WIDTH_MIN;
	fse->max_width = GC2145_SENSOR_WIDTH_MAX;

	fse->min_height = GC2145_SENSOR_HEIGHT_MIN;
	fse->max_height = GC2145_SENSOR_HEIGHT_MAX;

	return 0;
}

static int gc2145_enum_frame_interval(
	struct v4l2_subdev *sd,
	struct v4l2_subdev_pad_config *cfg,
	struct v4l2_subdev_frame_interval_enum *fie)
{
	if (fie->pad != 0 || fie->index > 0)
		return -EINVAL;

	fie->interval.numerator = 1;
	fie->interval.denominator = 30;

	return 0;
}

static int gc2145_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	struct v4l2_mbus_framefmt *mf;

	if (format->pad != 0)
		return -EINVAL;

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		mf = v4l2_subdev_get_try_format(sd, cfg, format->pad);
		format->format = *mf;
		return 0;
	}

	mutex_lock(&sensor->lock);
	format->format = sensor->fmt;
	mutex_unlock(&sensor->lock);

	return 0;
}

static int gc2145_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_pad_config *cfg,
			  struct v4l2_subdev_format *format)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	struct v4l2_mbus_framefmt *mf = &format->format;
	const struct gc2145_pixfmt *pixfmt;
	int ret = 0;

	if (format->pad != 0)
		return -EINVAL;

	/* check if we support requested mbus fmt */
	pixfmt = gc2145_find_format(mf->code);
	if (!pixfmt)
		pixfmt = &gc2145_formats[0];

	mf->code = pixfmt->code;
	mf->colorspace = pixfmt->colorspace;
	mf->xfer_func = V4L2_XFER_FUNC_DEFAULT;
	mf->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	mf->quantization = V4L2_QUANTIZATION_DEFAULT;
	mf->field = V4L2_FIELD_NONE;

	mutex_lock(&sensor->lock);

	mf->width = clamp(mf->width, GC2145_SENSOR_WIDTH_MIN,
		      GC2145_SENSOR_WIDTH_MAX);
	mf->height = clamp(mf->height, GC2145_SENSOR_HEIGHT_MIN,
		       GC2145_SENSOR_HEIGHT_MAX);

	if (format->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_mf;

		try_mf = v4l2_subdev_get_try_format(sd, cfg, format->pad);
		*try_mf = *mf;
		goto out;
	}

	if (sensor->streaming) {
		ret = -EBUSY;
		goto out;
	}

	sensor->fmt = *mf;
	sensor->pending_mode_change = true;
out:
	mutex_unlock(&sensor->lock);
	return ret;
}

/* }}} */
/* {{{ Core Ops */

static int gc2145_configure(struct gc2145_dev *sensor)
{
	struct v4l2_fwnode_bus_parallel *bus = &sensor->ep.bus.parallel;
	u8 sync_mode = 0;
	u16 chip_id;
	int ret;

	ret = gc2145_read16(sensor, GC2145_REG_CHIP_ID, &chip_id);
	if (ret)
		return ret;

	dev_info(&sensor->i2c_client->dev, "device id: 0x%04x\n",
		 (unsigned int)chip_id);

	if (chip_id != GC2145_REG_CHIP_ID_VALUE) {
		dev_err(&sensor->i2c_client->dev,
			"unsupported device id: 0x%04x\n",
			(unsigned int)chip_id);
		return -EINVAL;
	}

        // setup parallel bus

	if (bus->flags & V4L2_MBUS_VSYNC_ACTIVE_LOW)
		sync_mode |= 0x01;

	if (bus->flags & V4L2_MBUS_HSYNC_ACTIVE_LOW)
		sync_mode |= 0x02;

	if (bus->flags & V4L2_MBUS_PCLK_SAMPLE_FALLING)
		sync_mode |= 0x04;

	gc2145_tx_start(sensor);

	// soft reset
	gc2145_tx_write8(sensor, GC2145_REG_RESET, 0xf0);

	// enable analog/digital parts
	gc2145_tx_write8(sensor, GC2145_REG_ANALOG_PWC, 0x06);

	// safe initial PLL setting
	gc2145_tx_write8(sensor, GC2145_REG_PLL_MODE1, 0x1d);
	gc2145_tx_write8(sensor, GC2145_REG_PLL_MODE2, 0x84);
	gc2145_tx_write8(sensor, GC2145_REG_CLK_DIV_MODE, 0x00);

	gc2145_tx_write8(sensor, GC2145_REG_CM_MODE, 0xfe);

	// disable pads
	gc2145_tx_write8(sensor, GC2145_REG_PAD_IO, 0);

	gc2145_tx_write8(sensor, 0x19, 0x0c); // set AD pipe number
	gc2145_tx_write8(sensor, 0x20, 0x01); // AD clk mode

	// enable defect correction, etc.
	gc2145_tx_write8(sensor, 0x80, 0x0b);

	gc2145_tx_write8(sensor, GC2145_P0_SYNC_MODE, sync_mode);

	ret = gc2145_tx_commit(sensor);
	if (ret)
		return ret;

	// load default register values from the firmware file
	ret = gc2145_load_firmware(sensor, GC2145_FIRMWARE_PARAMETERS);
	if (ret < 0)
		return ret;

	return 0;
}

static int gc2145_set_power(struct gc2145_dev *sensor, bool on)
{
	int ret = 0;

	if (on) {
		ret = regulator_bulk_enable(GC2145_NUM_SUPPLIES,
					    sensor->supplies);
		if (ret)
			return ret;

		ret = clk_set_rate(sensor->xclk, 24000000);
		if (ret)
			goto xclk_off;

		ret = clk_prepare_enable(sensor->xclk);
		if (ret)
			goto power_off;

		usleep_range(10000, 12000);
		gpiod_direction_output(sensor->reset_gpio, 1);
		usleep_range(10000, 12000);
		gpiod_direction_output(sensor->enable_gpio, 1);
		usleep_range(10000, 12000);
		gpiod_direction_output(sensor->reset_gpio, 0);
		usleep_range(40000, 50000);

		ret = gc2145_configure(sensor);
		if (ret)
			goto xclk_off;

		ret = gc2145_setup_mode(sensor);
		if (ret)
			goto xclk_off;

		return 0;
	}

xclk_off:
	clk_disable_unprepare(sensor->xclk);
power_off:
	gpiod_direction_input(sensor->reset_gpio);
	gpiod_direction_input(sensor->enable_gpio);
	regulator_bulk_disable(GC2145_NUM_SUPPLIES, sensor->supplies);
	msleep(100);
	return ret;
}

static int gc2145_s_power(struct v4l2_subdev *sd, int on)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	bool power_up, power_down;
	int ret = 0;

	mutex_lock(&sensor->lock);

	power_up = on && !sensor->powered;
	power_down = !on && sensor->powered;

	if (power_up || power_down) {
		ret = gc2145_set_power(sensor, power_up);
		if (!ret)
			sensor->powered = on;
	}

	mutex_unlock(&sensor->lock);

	if (!ret && power_up) {
		/* restore controls */
		ret = v4l2_ctrl_handler_setup(&sensor->ctrls.handler);
		if (ret)
			gc2145_s_power(sd, 0);
	}

	return ret;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int gc2145_g_register(struct v4l2_subdev *sd,
			     struct v4l2_dbg_register *reg)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	int ret;
	u8 val = 0;

	if (reg->reg > 0xffff)
		return -EINVAL;

	reg->size = 1;

	mutex_lock(&sensor->lock);
	ret = gc2145_read(sensor, reg->reg, &val);
	mutex_unlock(&sensor->lock);
	if (ret)
		return -EIO;

	reg->val = val;
	return 0;
}

static int gc2145_s_register(struct v4l2_subdev *sd,
			     const struct v4l2_dbg_register *reg)
{
	struct gc2145_dev *sensor = to_gc2145_dev(sd);
	int ret;

	if (reg->reg > 0xffff || reg->val > 0xff)
		return -EINVAL;

	mutex_lock(&sensor->lock);
	ret = gc2145_write(sensor, reg->reg, reg->val);
	mutex_unlock(&sensor->lock);

	return ret;
}
#endif

/* }}} */

static const struct v4l2_subdev_core_ops gc2145_core_ops = {
	.s_power = gc2145_s_power,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.g_register = gc2145_g_register,
	.s_register = gc2145_s_register,
#endif
};

static const struct v4l2_subdev_pad_ops gc2145_pad_ops = {
	.enum_mbus_code = gc2145_enum_mbus_code,
	.enum_frame_size = gc2145_enum_frame_size,
	.enum_frame_interval = gc2145_enum_frame_interval,
	.get_fmt = gc2145_get_fmt,
	.set_fmt = gc2145_set_fmt,
};

static const struct v4l2_subdev_video_ops gc2145_video_ops = {
	.g_frame_interval = gc2145_g_frame_interval,
	.s_frame_interval = gc2145_s_frame_interval,
	.s_stream = gc2145_s_stream,
};

static const struct v4l2_subdev_ops gc2145_subdev_ops = {
	.core = &gc2145_core_ops,
	.pad = &gc2145_pad_ops,
	.video = &gc2145_video_ops,
};

static int gc2145_get_regulators(struct gc2145_dev *sensor)
{
	int i;

	for (i = 0; i < GC2145_NUM_SUPPLIES; i++)
		sensor->supplies[i].supply = gc2145_supply_name[i];

	return devm_regulator_bulk_get(&sensor->i2c_client->dev,
				       GC2145_NUM_SUPPLIES,
				       sensor->supplies);
}

static int gc2145_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *endpoint;
	struct gc2145_dev *sensor;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->i2c_client = client;

	sensor->fmt.code = gc2145_formats[0].code;
	sensor->fmt.width = 1600;
	sensor->fmt.height = 1200;
	sensor->fmt.field = V4L2_FIELD_NONE;
	sensor->frame_interval.numerator = 1;
	sensor->frame_interval.denominator = 10;
	sensor->pending_mode_change = true;
	sensor->current_bank = 0xff;

	endpoint = fwnode_graph_get_next_endpoint(
		of_fwnode_handle(client->dev.of_node), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
	fwnode_handle_put(endpoint);
	if (ret) {
		dev_err(dev, "could not parse endpoint\n");
		return ret;
	}

	if (sensor->ep.bus_type != V4L2_MBUS_PARALLEL) {
		dev_err(dev, "unsupported bus type %d\n", sensor->ep.bus_type);
		return -EINVAL;
	}

	sensor->xclk = devm_clk_get(dev, "xclk");
	if (IS_ERR(sensor->xclk)) {
		dev_err(dev, "failed to get xclk\n");
		return PTR_ERR(sensor->xclk);
	}

	sensor->enable_gpio = devm_gpiod_get_optional(dev, "enable", GPIOD_IN);
	if (IS_ERR(sensor->enable_gpio)) {
		dev_err(dev, "failed to get enable gpio\n");
		return PTR_ERR(sensor->enable_gpio);
	}

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_IN);
	if (IS_ERR(sensor->reset_gpio)) {
		dev_err(dev, "failed to get reset gpio\n");
		return PTR_ERR(sensor->reset_gpio);
	}

	if (!sensor->enable_gpio || !sensor->reset_gpio) {
		dev_err(dev, "enable and reset pins must be configured\n");
		return ret;
	}

	v4l2_i2c_subdev_init(&sensor->sd, client, &gc2145_subdev_ops);

	sensor->sd.flags = V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;

	mutex_init(&sensor->lock);

	ret = gc2145_get_regulators(sensor);
	if (ret)
		goto entity_cleanup;

	ret = gc2145_init_controls(sensor);
	if (ret)
		goto entity_cleanup;

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret)
		goto free_ctrls;

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static int gc2145_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc2145_dev *sensor = to_gc2145_dev(sd);

	v4l2_async_unregister_subdev(&sensor->sd);
	mutex_destroy(&sensor->lock);
	media_entity_cleanup(&sensor->sd.entity);
	v4l2_ctrl_handler_free(&sensor->ctrls.handler);

	return 0;
}

static const struct i2c_device_id gc2145_id[] = {
	{"gc2145", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, gc2145_id);

static const struct of_device_id gc2145_dt_ids[] = {
	{ .compatible = "galaxycore,gc2145" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gc2145_dt_ids);

static struct i2c_driver gc2145_i2c_driver = {
	.driver = {
		.name  = "gc2145",
		.of_match_table	= gc2145_dt_ids,
	},
	.id_table = gc2145_id,
	.probe    = gc2145_probe,
	.remove   = gc2145_remove,
};

module_i2c_driver(gc2145_i2c_driver);

MODULE_AUTHOR("Ondrej Jirman <megi@xff.cz>");
MODULE_DESCRIPTION("GC2145 Camera Subdev Driver");
MODULE_LICENSE("GPL");
