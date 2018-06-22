/* linux/drivers/video/fbdev/exynos/decon_7570/panels/s6d7aa0_lcd_ctrl.c
 *
 * Samsung SoC MIPI LCD CONTROL functions
 *
 * Copyright (c) 2015 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/lcd.h>
#include <linux/backlight.h>
#include <linux/of_device.h>
#include <video/mipi_display.h>
#include <linux/i2c.h>
#include <linux/module.h>

#include "../dsim.h"
#include "../decon.h"
#include "dsim_panel.h"

#include "s6d7aa0_xcover4_param.h"
#include "../decon_board.h"
#include "../decon_helper.h"

#if defined(CONFIG_EXYNOS_DECON_MDNIE_LITE)
#include "mdnie.h"
#include "mdnie_lite_table_xcover4.h"
#endif

#define S6D7AA0_ID_REG			0xDA	/* LCD ID1,ID2,ID3 */
#define S6D7AA0_ID_LEN			1
#define BRIGHTNESS_REG			0x51

#define DSI_WRITE(cmd, size)		do {				\
	ret = dsim_write_hl_data(lcd, cmd, size);			\
	if (ret < 0) {							\
		dev_err(&lcd->ld->dev, "%s: failed to write %s\n", __func__, #cmd);	\
		ret = -EPERM;						\
		goto exit;						\
	}								\
} while (0)

struct lcd_info {
	unsigned int			bl;
	unsigned int			brightness;
	unsigned int			current_bl;
	unsigned int			state;

	struct lcd_device		*ld;
	struct backlight_device		*bd;

	unsigned char			id[3];
	unsigned char			dump_info[3];
	unsigned int 			data_type;

	int						lux;
	struct class			*mdnie_class;

	struct dsim_device		*dsim;
	struct mutex			lock;

	struct notifier_block		fb_notif_panel;
};

struct i2c_client *backlight_client;

static int dsim_write_hl_data(struct lcd_info *lcd, const u8 *cmd, u32 cmdSize)
{
	int ret;
	int retry;
	struct panel_private *priv = &lcd->dsim->priv;

	if (priv->lcdConnected == PANEL_DISCONNEDTED)
		return cmdSize;

	retry = 5;

try_write:
	if (cmdSize == 1)
		ret = dsim_write_data(lcd->dsim, MIPI_DSI_DCS_SHORT_WRITE, cmd[0], 0);
	else if (cmdSize == 2)
		ret = dsim_write_data(lcd->dsim, MIPI_DSI_DCS_SHORT_WRITE_PARAM, cmd[0], cmd[1]);
	else
		ret = dsim_write_data(lcd->dsim, MIPI_DSI_DCS_LONG_WRITE, (unsigned long)cmd, cmdSize);

	if (ret != 0) {
		if (--retry)
			goto try_write;
		else
			dev_err(&lcd->ld->dev, "%s: fail. cmd: %x, ret: %d\n", __func__, cmd[0], ret);
	}

	return ret;
}

static int dsim_read_hl_data(struct lcd_info *lcd, u8 addr, u32 size, u8 *buf)
{
	int ret;
	int retry = 4;
	struct panel_private *priv = &lcd->dsim->priv;

	if (priv->lcdConnected == PANEL_DISCONNEDTED)
		return size;

try_read:
	ret = dsim_read_data(lcd->dsim, MIPI_DSI_DCS_READ, (u32)addr, size, buf);
	dev_info(&lcd->ld->dev, "%s: addr: %x, ret: %d\n", __func__, addr, ret);
	if (ret != size) {
		if (--retry)
			goto try_read;
		else
			dev_err(&lcd->ld->dev, "%s: fail. addr: %x\n", __func__, addr);
	}

	return ret;
}

#if 0

static int dsim_read_hl_data_offset(struct lcd_info *lcd, u8 addr, u32 size, u8 *buf, u32 offset)
{
	unsigned char wbuf[] = {0xB0, 0};
	int ret = 0;

	wbuf[1] = offset;

	DSI_WRITE(wbuf, ARRAY_SIZE(wbuf));

	ret = dsim_read_hl_data(lcd, addr, size, buf);

	if (ret < 1)
		dev_err(&lcd->ld->dev, "%s: fail\n", __func__);

exit:
	return ret;
}
#endif

static int ISL98611_array_write(const struct ISL98611_rom_data *eprom_ptr, int eprom_size)
{
	int i = 0;
	int ret = 0;

	if (!backlight_client)
		return 0;

	for (i = 0; i < eprom_size; i++) {
		ret = i2c_smbus_write_byte_data(backlight_client, eprom_ptr[i].addr, eprom_ptr[i].val);
		if (ret < 0)
			dsim_err("%s: error : BL DEVICE_CTRL setting fail\n", __func__);
	}

	return ret;
}

static int dsim_panel_set_brightness(struct lcd_info *lcd, int force)
{
	int ret = 0;
	unsigned char bl_reg[2];

	mutex_lock(&lcd->lock);

	lcd->brightness = lcd->bd->props.brightness;

	lcd->bl = lcd->brightness;
	lcd->bl = (lcd->bl > UI_MAX_BRIGHTNESS) ? UI_MAX_BRIGHTNESS : lcd->bl;

	if (!force && lcd->state != PANEL_STATE_RESUMED) {
		dev_info(&lcd->ld->dev, "%s: panel is not active state\n", __func__);
		goto exit;
	}
	bl_reg[0] = BRIGHTNESS_REG;

	if (lcd->bl >= UI_DEFAULT_BRIGHTNESS) {
		bl_reg[1] = (lcd->bl - UI_DEFAULT_BRIGHTNESS) *
			(DDI_MAX_BRIGHTNESS - DDI_DEFAULT_BRIGHTNESS) / (UI_MAX_BRIGHTNESS - UI_DEFAULT_BRIGHTNESS) + DDI_DEFAULT_BRIGHTNESS;
	} else if (lcd->bl >= UI_MIN_BRIGHTNESS) {
		bl_reg[1] = (lcd->bl - UI_MIN_BRIGHTNESS) *
			(DDI_DEFAULT_BRIGHTNESS - DDI_MIN_BRIGHTNESS) / (UI_DEFAULT_BRIGHTNESS - UI_MIN_BRIGHTNESS) + DDI_MIN_BRIGHTNESS;
	} else
		bl_reg[1] = 0x00;

	if (LEVEL_IS_HBM(lcd->brightness))
		bl_reg[1] = DDI_OUTDOOR_BRIGHTNESS;

	DSI_WRITE(bl_reg, ARRAY_SIZE(bl_reg));
	dev_info(&lcd->ld->dev, "%s: platform BL : %d panel BL reg : %d\n", __func__, lcd->bd->props.brightness, bl_reg[1]);
exit:
	mutex_unlock(&lcd->lock);

	return ret;
}

static int panel_get_brightness(struct backlight_device *bd)
{
	return bd->props.brightness;
}

static int panel_set_brightness(struct backlight_device *bd)
{
	int ret = 0;
	struct lcd_info *lcd = bl_get_data(bd);

	if (lcd->state == PANEL_STATE_RESUMED) {
		ret = dsim_panel_set_brightness(lcd, 0);
		if (ret) {
			dev_err(&lcd->ld->dev, "%s: failed to set brightness\n", __func__);
			goto exit;
		}
	}

exit:
	return ret;
}

static const struct backlight_ops panel_backlight_ops = {
	.get_brightness = panel_get_brightness,
	.update_status = panel_set_brightness,
};


static int s6d7aa0_read_init_info(struct lcd_info *lcd)
{
	struct panel_private *priv = &lcd->dsim->priv;
	int i = 0;

	dev_info(&lcd->ld->dev, "MDD : %s was called\n", __func__);

	if (lcdtype == 0) {
		priv->lcdConnected = PANEL_DISCONNEDTED;
		goto read_exit;
	}

	lcd->id[0] = (lcdtype & 0xFF0000) >> 16;
	lcd->id[1] = (lcdtype & 0x00FF00) >> 8;
	lcd->id[2] = (lcdtype & 0x0000FF) >> 0;

	dev_info(&lcd->ld->dev, "READ ID : ");
	for (i = 0; i < 3; i++)
		dev_info(&lcd->ld->dev, "%02x, ", lcd->id[i]);
	dev_info(&lcd->ld->dev, "\n");

read_exit:
	return 0;
}

static int s6d7aa0_read_id(struct lcd_info *lcd)
{
	int ret = 0;
	u8 buf[3] = {0, };

	ret = dsim_read_hl_data(lcd, S6D7AA0_ID_REG, S6D7AA0_ID_LEN, (u8 *)&buf[0]);
	ret = dsim_read_hl_data(lcd, S6D7AA0_ID_REG + 1, S6D7AA0_ID_LEN, (u8 *)&buf[1]);
	ret = dsim_read_hl_data(lcd, S6D7AA0_ID_REG + 2, S6D7AA0_ID_LEN, (u8 *)&buf[2]);

	dsim_info("%s %d : 0x%02x 0x%02x 0x%02x\n", __func__, ret, buf[0], buf[1], buf[2]);

	return ret;
}

static int s6d7aa0_displayon(struct lcd_info *lcd)
{
	int ret = 0;

	dev_info(&lcd->ld->dev, "%s\n", __func__);
	DSI_WRITE(SEQ_DISPLAY_ON, ARRAY_SIZE(SEQ_DISPLAY_ON));

	usleep_range(5000, 6000);

	DSI_WRITE(SEQ_S6D7AA0_BL_ON, ARRAY_SIZE(SEQ_S6D7AA0_BL_ON));

exit:
	return ret;
}

static int s6d7aa0_exit(struct lcd_info *lcd)
{
	int ret = 0;

	dev_info(&lcd->ld->dev, "%s\n", __func__);

	DSI_WRITE(SEQ_DISPLAY_OFF, ARRAY_SIZE(SEQ_DISPLAY_OFF));

	usleep_range(5000, 6000);

	DSI_WRITE(SEQ_SLEEP_IN, ARRAY_SIZE(SEQ_SLEEP_IN));

	msleep(120);

exit:
	return ret;
}

static int s6d7aa0_init(struct lcd_info *lcd)
{
	int ret = 0;

	dev_info(&lcd->ld->dev, "%s: ++\n", __func__);

	s6d7aa0_read_id(lcd);

	DSI_WRITE(SEQ_PASSWD1, ARRAY_SIZE(SEQ_PASSWD1));
	DSI_WRITE(SEQ_PASSWD2, ARRAY_SIZE(SEQ_PASSWD2));
	DSI_WRITE(SEQ_PASSWD3, ARRAY_SIZE(SEQ_PASSWD3));

	DSI_WRITE(SEQ_S6D7AA0_INIT_1, ARRAY_SIZE(SEQ_S6D7AA0_INIT_1));
	DSI_WRITE(SEQ_S6D7AA0_INIT_2, ARRAY_SIZE(SEQ_S6D7AA0_INIT_2));
	DSI_WRITE(SEQ_S6D7AA0_INIT_3, ARRAY_SIZE(SEQ_S6D7AA0_INIT_3));
	DSI_WRITE(SEQ_S6D7AA0_INIT_4, ARRAY_SIZE(SEQ_S6D7AA0_INIT_4));
	DSI_WRITE(SEQ_S6D7AA0_INIT_5, ARRAY_SIZE(SEQ_S6D7AA0_INIT_5));
	DSI_WRITE(SEQ_S6D7AA0_INIT_6, ARRAY_SIZE(SEQ_S6D7AA0_INIT_6));
	DSI_WRITE(SEQ_S6D7AA0_INIT_7, ARRAY_SIZE(SEQ_S6D7AA0_INIT_7));
	DSI_WRITE(SEQ_S6D7AA0_INIT_8, ARRAY_SIZE(SEQ_S6D7AA0_INIT_8));
	DSI_WRITE(SEQ_S6D7AA0_INIT_9, ARRAY_SIZE(SEQ_S6D7AA0_INIT_9));
	DSI_WRITE(SEQ_S6D7AA0_INIT_10, ARRAY_SIZE(SEQ_S6D7AA0_INIT_10));
	DSI_WRITE(SEQ_S6D7AA0_INIT_11, ARRAY_SIZE(SEQ_S6D7AA0_INIT_11));
	DSI_WRITE(SEQ_S6D7AA0_INIT_12, ARRAY_SIZE(SEQ_S6D7AA0_INIT_12));
	DSI_WRITE(SEQ_S6D7AA0_INIT_13, ARRAY_SIZE(SEQ_S6D7AA0_INIT_13));
	DSI_WRITE(SEQ_S6D7AA0_INIT_14, ARRAY_SIZE(SEQ_S6D7AA0_INIT_14));
	DSI_WRITE(SEQ_S6D7AA0_INIT_15, ARRAY_SIZE(SEQ_S6D7AA0_INIT_15));
	DSI_WRITE(SEQ_S6D7AA0_INIT_16, ARRAY_SIZE(SEQ_S6D7AA0_INIT_16));
	DSI_WRITE(SEQ_S6D7AA0_INIT_17, ARRAY_SIZE(SEQ_S6D7AA0_INIT_17));
	DSI_WRITE(SEQ_S6D7AA0_INIT_18, ARRAY_SIZE(SEQ_S6D7AA0_INIT_18));
	DSI_WRITE(SEQ_S6D7AA0_INIT_19, ARRAY_SIZE(SEQ_S6D7AA0_INIT_19));
	DSI_WRITE(SEQ_S6D7AA0_INIT_20, ARRAY_SIZE(SEQ_S6D7AA0_INIT_20));
	DSI_WRITE(SEQ_S6D7AA0_INIT_21, ARRAY_SIZE(SEQ_S6D7AA0_INIT_21));
	DSI_WRITE(SEQ_S6D7AA0_INIT_22, ARRAY_SIZE(SEQ_S6D7AA0_INIT_22));
	DSI_WRITE(SEQ_S6D7AA0_INIT_23, ARRAY_SIZE(SEQ_S6D7AA0_INIT_23));

	DSI_WRITE(SEQ_GAMMA_FA, ARRAY_SIZE(SEQ_GAMMA_FA));
	DSI_WRITE(SEQ_GAMMA_FB, ARRAY_SIZE(SEQ_GAMMA_FB));

	DSI_WRITE(SEQ_PASSWD1_LOCK, ARRAY_SIZE(SEQ_PASSWD1_LOCK));
	DSI_WRITE(SEQ_PASSWD2_LOCK, ARRAY_SIZE(SEQ_PASSWD2_LOCK));
	DSI_WRITE(SEQ_PASSWD3_LOCK, ARRAY_SIZE(SEQ_PASSWD3_LOCK));

	DSI_WRITE(SEQ_SLEEP_OUT, ARRAY_SIZE(SEQ_SLEEP_OUT));

	msleep(120);

	dev_info(&lcd->ld->dev, "%s: --\n", __func__);

exit:
	return ret;
}

static int isl98611_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dsim_err("need I2C_FUNC_I2C.\n");
		ret = -ENODEV;
		goto err_i2c;
	}

	backlight_client = client;

err_i2c:
	return ret;
}

static struct i2c_device_id isl98611_id[] = {
	{"isl98611", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, isl98611_id);

static struct of_device_id isl98611_i2c_dt_ids[] = {
	{ .compatible = "isl98611,i2c" },
	{ }
};

MODULE_DEVICE_TABLE(of, isl98611_i2c_dt_ids);

static struct i2c_driver isl98611_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "isl98611",
		.of_match_table	= of_match_ptr(isl98611_i2c_dt_ids),
	},
	.id_table = isl98611_id,
	.probe = isl98611_probe,
};

static int s6d7aa0_probe(struct dsim_device *dsim)
{
	int ret = 0;
	struct panel_private *priv = &dsim->priv;
	struct lcd_info *lcd = dsim->priv.par;

	dev_info(&lcd->ld->dev, "%s: was called\n", __func__);

	priv->lcdConnected = PANEL_CONNECTED;

	lcd->bd->props.max_brightness = EXTEND_BRIGHTNESS;
	lcd->bd->props.brightness = UI_DEFAULT_BRIGHTNESS;

	lcd->dsim = dsim;
	lcd->state = PANEL_STATE_RESUMED;
	lcd->lux = -1;

	s6d7aa0_read_init_info(lcd);
	if (priv->lcdConnected == PANEL_DISCONNEDTED) {
		dev_err(&lcd->ld->dev, "dsim : %s lcd was not connected\n", __func__);
		goto exit;
	}

	i2c_add_driver(&isl98611_i2c_driver);

	dev_info(&lcd->ld->dev, "%s: done\n", __func__);
exit:
	return ret;
}

static ssize_t lcd_type_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct lcd_info *lcd = dev_get_drvdata(dev);

	sprintf(buf, "BOE_%02X%02X%02X\n", lcd->id[0], lcd->id[1], lcd->id[2]);

	return strlen(buf);
}

static ssize_t window_type_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct lcd_info *lcd = dev_get_drvdata(dev);

	sprintf(buf, "%x %x %x\n", lcd->id[0], lcd->id[1], lcd->id[2]);

	return strlen(buf);
}

static ssize_t lux_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct lcd_info *lcd = dev_get_drvdata(dev);

	sprintf(buf, "%d\n", lcd->lux);

	return strlen(buf);
}

static ssize_t lux_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct lcd_info *lcd = dev_get_drvdata(dev);
	int value;
	int rc;

	rc = kstrtoint(buf, 0, &value);

	if (rc < 0)
		return rc;

	if (lcd->lux != value) {
		mutex_lock(&lcd->lock);
		lcd->lux = value;
		mutex_unlock(&lcd->lock);

#if defined(CONFIG_EXYNOS_DECON_MDNIE_LITE)
		attr_store_for_each(lcd->mdnie_class, attr->attr.name, buf, size);
#endif
	}

	return size;
}

static DEVICE_ATTR(lcd_type, 0444, lcd_type_show, NULL);
static DEVICE_ATTR(window_type, 0444, window_type_show, NULL);
static DEVICE_ATTR(lux, 0644, lux_show, lux_store);

static struct attribute *lcd_sysfs_attributes[] = {
	&dev_attr_lcd_type.attr,
	&dev_attr_window_type.attr,
	&dev_attr_lux.attr,
	NULL,
};

static const struct attribute_group lcd_sysfs_attr_group = {
	.attrs = lcd_sysfs_attributes,
};

static void lcd_init_sysfs(struct lcd_info *lcd)
{
	int ret = 0;

	ret = sysfs_create_group(&lcd->ld->dev.kobj, &lcd_sysfs_attr_group);
	if (ret < 0)
		dev_err(&lcd->ld->dev, "failed to add lcd sysfs\n");
}

#if defined(CONFIG_EXYNOS_DECON_MDNIE_LITE)
static int mdnie_lite_write_set(struct lcd_info *lcd, struct lcd_seq_info *seq, u32 num)
{
	int ret = 0, i;

	for (i = 0; i < num; i++) {
		if (seq[i].cmd) {
			ret = dsim_write_hl_data(lcd, seq[i].cmd, seq[i].len);
			if (ret != 0) {
				dev_info(&lcd->ld->dev, "%s: %dth fail\n", __func__, i);
				return ret;
			}
		}
		if (seq[i].sleep)
			usleep_range(seq[i].sleep * 1000, seq[i].sleep * 1000);
	}
	return ret;
}

static int mdnie_lite_send_seq(struct lcd_info *lcd, struct lcd_seq_info *seq, u32 num)
{
	int ret = 0;

	if (lcd->state != PANEL_STATE_RESUMED) {
		dev_info(&lcd->ld->dev, "%s: panel is not active\n", __func__);
		return -EIO;
	}

	mutex_lock(&lcd->lock);
	ret = mdnie_lite_write_set(lcd, seq, num);
	mutex_unlock(&lcd->lock);

	return ret;
}

static int mdnie_lite_read(struct lcd_info *lcd, u8 addr, u8 *buf, u32 size)
{
	int ret = 0;

	return ret;
}
#endif

static int dsim_panel_probe(struct dsim_device *dsim)
{
	int ret = 0;
	struct lcd_info *lcd;

	dsim->priv.par = lcd = kzalloc(sizeof(struct lcd_info), GFP_KERNEL);
	if (!lcd) {
		pr_err("%s: failed to allocate for lcd\n", __func__);
		ret = -ENOMEM;
		goto probe_err;
	}

	dsim->lcd = lcd->ld = lcd_device_register("panel", dsim->dev, lcd, NULL);
	if (IS_ERR(lcd->ld)) {
		pr_err("%s: failed to register lcd device\n", __func__);
		ret = PTR_ERR(lcd->ld);
		goto probe_err;
	}

	lcd->bd = backlight_device_register("panel", dsim->dev, lcd, &panel_backlight_ops, NULL);
	if (IS_ERR(lcd->bd)) {
		pr_err("%s: failed to register backlight device\n", __func__);
		ret = PTR_ERR(lcd->bd);
		goto probe_err;
	}

	mutex_init(&lcd->lock);

	ret = s6d7aa0_probe(dsim);
	if (ret) {
		dev_err(&lcd->ld->dev, "%s: failed to probe panel\n", __func__);
		goto probe_err;
	}

	lcd_init_sysfs(lcd);

#if defined(CONFIG_EXYNOS_DECON_MDNIE_LITE)
	mdnie_register(&lcd->ld->dev, lcd, (mdnie_w)mdnie_lite_send_seq, (mdnie_r)mdnie_lite_read, NULL, &tune_info);
	lcd->mdnie_class = get_mdnie_class();
#endif

	dev_info(&lcd->ld->dev, "%s: %s: done\n", kbasename(__FILE__), __func__);
probe_err:
	return ret;
}

static int dsim_panel_resume_early(struct dsim_device *dsim)
{
	struct lcd_info *lcd = dsim->priv.par;
	struct panel_private *priv = &lcd->dsim->priv;
	int ret = 0;

	dev_info(&lcd->ld->dev, "+%s\n", __func__);

	/* VSP VSN setting, So, It should be called before power enabling */

	ret = ISL98611_array_write(ISL98611_INIT, ARRAY_SIZE(ISL98611_INIT));
	if (ret < 0)
		dev_err(&lcd->ld->dev, "%s: error : BL DEVICE_CTRL setting fail\n", __func__);

	dev_info(&lcd->ld->dev, "-%s: %d\n", __func__, priv->lcdConnected);

	return ret;
}

static int dsim_panel_displayon(struct dsim_device *dsim)
{
	struct lcd_info *lcd = dsim->priv.par;
	struct panel_private *priv = &lcd->dsim->priv;
	int ret = 0;

	dev_info(&lcd->ld->dev, "+%s\n", __func__);

	if (lcd->state == PANEL_STATE_SUSPENED) {
		ret = s6d7aa0_init(lcd);
		if (ret) {
			dev_info(&lcd->ld->dev, "%s: failed to panel init\n", __func__);
			goto displayon_err;
		}
	}

	ret = s6d7aa0_displayon(lcd);
	if (ret) {
		dev_info(&lcd->ld->dev, "%s: failed to panel display on\n", __func__);
		goto displayon_err;
	}

displayon_err:
	mutex_lock(&lcd->lock);
	lcd->state = PANEL_STATE_RESUMED;
	mutex_unlock(&lcd->lock);

	dev_info(&lcd->ld->dev, "-%s: %d\n", __func__, priv->lcdConnected);

	return ret;
}

static int dsim_panel_suspend(struct dsim_device *dsim)
{
	struct lcd_info *lcd = dsim->priv.par;
	struct panel_private *priv = &lcd->dsim->priv;
	int ret = 0;

	dev_info(&lcd->ld->dev, "+%s\n", __func__);

	if (lcd->state == PANEL_STATE_SUSPENED)
		goto suspend_err;

	lcd->state = PANEL_STATE_SUSPENDING;

	ret = s6d7aa0_exit(lcd);
	if (ret) {
		dev_info(&lcd->ld->dev, "%s: failed to panel exit\n", __func__);
		goto suspend_err;
	}

suspend_err:
	mutex_lock(&lcd->lock);
	lcd->state = PANEL_STATE_SUSPENED;
	mutex_unlock(&lcd->lock);

	dev_info(&lcd->ld->dev, "-%s: %d\n", __func__, priv->lcdConnected);

	return ret;
}

struct mipi_dsim_lcd_driver s6d7aa0_mipi_lcd_driver = {
	.probe		= dsim_panel_probe,
	.resume_early	= dsim_panel_resume_early,
	.displayon	= dsim_panel_displayon,
	.suspend	= dsim_panel_suspend,
};

