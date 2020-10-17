/*
 * ANX7688 USB-C HDMI bridge/PD driver
 *
 * How this works:
 * - this driver allows to program firmware into ANX7688 EEPROM, and
 *   initialize it
 * - it then communicates with the firmware running on the OCM (on-chip
 *   microcontroller)
 * - it detects whether there is cable plugged in or not and powers
 *   up or down the ANX7688 based on that
 * - when the cable is connected the firmware on the OCM will handle
 *   the detection of the nature of the device on the other end
 *   of the USB-C cable
 * - this driver then communicates with the USB phy to let it swap
 *   data roles accordingly
 * - it also enables VBUS and VCONN regulators as appropriate
 * - when the firmware detects SRC.1.5A or SRC.3.0A via CC pins
 *   or something else via PD, it notifies this driver via software
 *   interrupt and this driver notifies the PMIC that the input
 *   current limit can be increased
 * - USB phy driver (Allwinner) needs to know whether to switch to
 *   device or host mode, or whether to turn off
 */
#define DEBUG

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/power_supply.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/pd.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>

#define DISABLE_OCM 0

/* firmware regs */

#define ANX7688_REG_VBUS_OFF_DELAY_TIME 0x22
#define ANX7688_REG_FEATURE_CTRL        0x27
#define ANX7688_REG_EEPROM_LOAD_STATUS1 0x11
#define ANX7688_REG_EEPROM_LOAD_STATUS0 0x12
#define ANX7688_REG_FW_VERSION1         0x15
#define ANX7688_REG_FW_VERSION0         0x16

#define ANX7688_EEPROM_FW_LOADED	0x01

#define ANX7688_REG_STATUS_INT_MASK     0x17
#define ANX7688_REG_STATUS_INT          0x28
#define ANX7688_IRQS_RECEIVED_MSG       BIT(0)
#define ANX7688_IRQS_RECEIVED_ACK       BIT(1)
#define ANX7688_IRQS_VCONN_CHANGE       BIT(2)
#define ANX7688_IRQS_VBUS_CHANGE        BIT(3)
#define ANX7688_IRQS_CC_STATUS_CHANGE   BIT(4)
#define ANX7688_IRQS_DATA_ROLE_CHANGE   BIT(5)

#define ANX7688_REG_STATUS              0x29
#define ANX7688_VCONN_STATUS            BIT(2) /* 0 = off  1 = on */
#define ANX7688_VBUS_STATUS             BIT(3) /* 0 = off  1 = on */
#define ANX7688_DATA_ROLE_STATUS        BIT(5) /* 0 = device 1 = host */

#define ANX7688_REG_CC_STATUS           0x2a
#define ANX7688_REG_TRY_UFP_TIMER       0x23
#define ANX7688_REG_TIME_CTRL           0x24

#define ANX7688_REG_MAX_VOLTAGE         0x1b
#define ANX7688_REG_MAX_POWER           0x1c
#define ANX7688_REG_MIN_POWER           0x1d
#define ANX7688_REG_MAX_VOLTAGE_STATUS  0x1e
#define ANX7688_REG_MAX_POWER_STATUS    0x1f

#define ANX7688_SOFT_INT_MASK           0x7f

/* tcpc regs */

#define ANX7688_TCPC_REG_VENDOR_ID0     0x00
#define ANX7688_TCPC_REG_VENDOR_ID1     0x01
#define ANX7688_TCPC_REG_ALERT0         0x10
#define ANX7688_TCPC_REG_ALERT1         0x11
#define ANX7688_TCPC_REG_ALERT_MASK0    0x12
#define ANX7688_TCPC_REG_ALERT_MASK1    0x13
#define ANX7688_TCPC_REG_INTERFACE_SEND 0x30
#define ANX7688_TCPC_REG_INTERFACE_RECV 0x51

/* hw regs */

#define ANX7688_REG_IRQ_EXT_SOURCE0     0x3e
#define ANX7688_REG_IRQ_EXT_SOURCE1     0x4e
#define ANX7688_REG_IRQ_EXT_SOURCE2     0x4f
#define ANX7688_REG_IRQ_EXT_MASK0       0x3b
#define ANX7688_REG_IRQ_EXT_MASK1       0x3c
#define ANX7688_REG_IRQ_EXT_MASK2       0x3d
#define ANX7688_REG_IRQ_SOURCE0         0x54
#define ANX7688_REG_IRQ_SOURCE1         0x55
#define ANX7688_REG_IRQ_SOURCE2         0x56
#define ANX7688_REG_IRQ_MASK0           0x57
#define ANX7688_REG_IRQ_MASK1           0x58
#define ANX7688_REG_IRQ_MASK2           0x59

#define ANX7688_IRQ2_SOFT_INT           BIT(2)

#define ANX7688_REG_USBC_RESET_CTRL		0x05
#define ANX7688_USBC_RESET_CTRL_OCM_RESET	BIT(4)

//#define ANX7688_IRQ2_USB_PLUGIN         BIT(4)
//#define ANX7688_REG_IRQ_STATUS          0x53

/* ocm messages */

#define ANX7688_OCM_MSG_PWR_SRC_CAP     0x00
#define ANX7688_OCM_MSG_PWR_SNK_CAP     0x01
#define ANX7688_OCM_MSG_DP_SNK_IDENTITY 0x02
#define ANX7688_OCM_MSG_SVID            0x03
#define ANX7688_OCM_MSG_GET_DP_SNK_CAP  0x04
#define ANX7688_OCM_MSG_ACCEPT          0x05
#define ANX7688_OCM_MSG_REJECT          0x06
#define ANX7688_OCM_MSG_PSWAP_REQ       0x10
#define ANX7688_OCM_MSG_DSWAP_REQ       0x11
#define ANX7688_OCM_MSG_GOTO_MIN_REQ    0x12
#define ANX7688_OCM_MSG_VCONN_SWAP_REQ  0x13
#define ANX7688_OCM_MSG_VDM             0x14
#define ANX7688_OCM_MSG_DP_SNK_CFG      0x15
#define ANX7688_OCM_MSG_PWR_OBJ_REQ     0x16
#define ANX7688_OCM_MSG_PD_STATUS_REQ   0x17
#define ANX7688_OCM_MSG_DP_ALT_ENTER    0x19
#define ANX7688_OCM_MSG_DP_ALT_EXIT     0x1a
#define ANX7688_OCM_MSG_RESPONSE_TO_REQ 0xf0
#define ANX7688_OCM_MSG_SOFT_RST        0xf1
#define ANX7688_OCM_MSG_HARD_RST        0xf2
#define ANX7688_OCM_MSG_RESTART         0xf3

static const char * const anx7688_supply_names[] = {
        "avdd33",
        "avdd18",
        "dvdd18",
        "avdd10",
        "dvdd10",
        "hdmi_vt",

        "vconn", // power for VCONN1/VCONN2 switches
        "vbus", // vbus power
};

#define ANX7688_NUM_SUPPLIES ARRAY_SIZE(anx7688_supply_names)
#define ANX7688_NUM_ALWAYS_ON_SUPPLIES (ANX7688_NUM_SUPPLIES - 1)
#define ANX7688_VCONN_INDEX (ANX7688_NUM_SUPPLIES - 2)
#define ANX7688_VBUS_INDEX (ANX7688_NUM_SUPPLIES - 1)

enum {
	ANX7688_F_POWERED,
	ANX7688_F_CONNECTED,
	ANX7688_F_FW_FAILED,
	ANX7688_F_PWRSUPPLY_CHANGE,
};

struct anx7688 {
        struct device *dev;
        struct i2c_client *client;
        struct i2c_client *client_tcpc;
        struct regulator_bulk_data supplies[ANX7688_NUM_SUPPLIES];
	struct power_supply *vbus_in_supply;
	struct notifier_block vbus_in_nb;
	int current_limit;
        struct gpio_desc *gpio_enable;
        struct gpio_desc *gpio_reset;
        struct gpio_desc *gpio_cabledet;

	unsigned long flags[1];

        struct delayed_work work;
	struct timer_list work_timer;

        struct mutex lock;
        bool vbus_on, vconn_on;

        struct typec_port *port;
        struct typec_partner *partner;
        struct usb_pd_identity partner_identity;
	struct usb_role_switch *role_sw;

        struct dentry *debug_root;

	/* for debug */
	int last_status;
	int last_cc_status;
	int last_dp_state;
	int last_bc_result;
};

static int anx7688_reg_read(struct anx7688 *anx7688, u8 reg_addr)
{
        int ret;

        ret = i2c_smbus_read_byte_data(anx7688->client, reg_addr);
        if (ret < 0)
                dev_err(anx7688->dev, "i2c read failed at 0x%x (%d)\n",
                        reg_addr, ret);

        return ret;
}

static int anx7688_reg_write(struct anx7688 *anx7688, u8 reg_addr, u8 value)
{
        int ret;

        ret = i2c_smbus_write_byte_data(anx7688->client, reg_addr, value);
        if (ret < 0)
                dev_err(anx7688->dev, "i2c write failed at 0x%x (%d)\n",
                        reg_addr, ret);

        return ret;
}

static int anx7688_reg_update_bits(struct anx7688 *anx7688, u8 reg_addr,
                                   u8 mask, u8 value)
{
        int ret;

        ret = anx7688_reg_read(anx7688, reg_addr);
        if (ret < 0)
                return ret;

        ret &= ~mask;
        ret |= value;

        return anx7688_reg_write(anx7688, reg_addr, ret);
}

static int anx7688_tcpc_reg_read(struct anx7688 *anx7688, u8 reg_addr)
{
        int ret;

        ret = i2c_smbus_read_byte_data(anx7688->client_tcpc, reg_addr);
        if (ret < 0)
                dev_err(anx7688->dev, "tcpc i2c read failed at 0x%x (%d)\n",
                        reg_addr, ret);

        return ret;
}

static int anx7688_tcpc_reg_write(struct anx7688 *anx7688, u8 reg_addr, u8 value)
{
        int ret;

        ret = i2c_smbus_write_byte_data(anx7688->client_tcpc, reg_addr, value);
        if (ret < 0)
                dev_err(anx7688->dev, "tcpc i2c write failed at 0x%x (%d)\n",
                        reg_addr, ret);

        return ret;
}

static void anx7688_power_enable(struct anx7688 *anx7688)
{
        gpiod_set_value(anx7688->gpio_reset, 1);
        gpiod_set_value(anx7688->gpio_enable, 1);

        /* wait for power to stabilize and release reset */
        msleep(10);
        gpiod_set_value(anx7688->gpio_reset, 0);
        udelay(2);

        dev_dbg(anx7688->dev, "power enabled\n");

	set_bit(ANX7688_F_POWERED, anx7688->flags);
}

static void anx7688_power_disable(struct anx7688 *anx7688)
{
        gpiod_set_value(anx7688->gpio_reset, 1);
        msleep(5);
        gpiod_set_value(anx7688->gpio_enable, 0);

        dev_dbg(anx7688->dev, "power disabled\n");

	clear_bit(ANX7688_F_POWERED, anx7688->flags);
}

static int anx7688_send_ocm_message(struct anx7688 *anx7688, int cmd,
                                    const u8 *data, int data_len)
{
        int ret = 0, i;
        u8 pdo[32];

        if (data_len > sizeof(pdo) - 3 || data_len < 1) {
                dev_dbg(anx7688->dev,
                        "invalid ocm message length cmd=%d len=%d\n",
                        cmd, data_len);
                return -EINVAL;
        }

        // prepare pd packet
        pdo[0] = data_len + 1;
        pdo[1] = cmd;
        memcpy(pdo + 2, data, data_len);
        pdo[2 + data_len] = 0;
        for (i = 0; i < data_len + 2; i++)
                pdo[data_len + 2] -= pdo[i];

        dev_dbg(anx7688->dev, "send pd packet cmd=%d %*ph\n",
                cmd, data_len + 3, pdo);

        ret = anx7688_tcpc_reg_read(anx7688, ANX7688_TCPC_REG_INTERFACE_SEND);
        if (ret) {
                dev_err(anx7688->dev,
                        "failed to send pd packet (tx buffer full)\n");
                return -EBUSY;
        }

        ret = i2c_smbus_write_i2c_block_data(anx7688->client_tcpc,
                                             ANX7688_TCPC_REG_INTERFACE_SEND,
                                             data_len + 3, pdo);
        if (ret < 0)
                dev_err(anx7688->dev,
                        "failed to send pd packet (err=%d)\n", ret);

        // wait until the message is processed (10ms max)
        for (i = 0; i < 100; i++) {
                ret = anx7688_tcpc_reg_read(anx7688, ANX7688_TCPC_REG_INTERFACE_SEND);
                if (ret <= 0)
                        return ret;

                udelay(100);
        }

        dev_err(anx7688->dev, "timeout waiting for the message queue flush\n");
        return -ETIMEDOUT;
}

static int anx7688_connect(struct anx7688 *anx7688)
{
#if DISABLE_OCM
        int ret;

        dev_dbg(anx7688->dev, "cable inserted\n");

        msleep(10);
        anx7688_power_enable(anx7688);

	/* reset the OCM right away */
        ret = anx7688_reg_update_bits(anx7688, ANX7688_REG_USBC_RESET_CTRL,
                                      ANX7688_USBC_RESET_CTRL_OCM_RESET,
                                      ANX7688_USBC_RESET_CTRL_OCM_RESET);
	if (ret)
		goto err_poweroff;

	/* enable interrupts for VBUS, etc. */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2, 0xff);
	if (ret)
		goto err_poweroff;

        ret = anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_MASK2, ~(BIT(6) | BIT(5) | BIT(4)));
	if (ret)
		goto err_poweroff;

	set_bit(ANX7688_F_CONNECTED, anx7688->flags);
        return 0;

err_poweroff:
        anx7688_power_disable(anx7688);
        return ret;
#else
	struct typec_partner_desc desc = {};
        int ret, i;
        u8 fw[2];
        u32 pdo;
	const u8 dp_snk_identity[16] = {
		0x00, 0x00, 0x00, 0xec,	/* snk_id_hdr */
		0x00, 0x00, 0x00, 0x00,	/* snk_cert */
		0x00, 0x00, 0x00, 0x00,	/* snk_prd */
		0x39, 0x00, 0x00, 0x51	/* snk_ama */
	};
	const u8 svid[4] = {
		0x00, 0x00, 0x01, 0xff,
	};

        dev_dbg(anx7688->dev, "cable inserted\n");

	anx7688->last_status = -1;
	anx7688->last_cc_status = -1;
	anx7688->last_dp_state = -1;

        msleep(10);
        anx7688_power_enable(anx7688);

        ret = regulator_enable(anx7688->supplies[ANX7688_VCONN_INDEX].consumer);
        if (ret) {
                dev_err(anx7688->dev, "failed to enable vconn\n");
                goto err_poweroff;
        }
	anx7688->vconn_on = true;

        /* wait till the firmware is loaded (typically ~30ms) */
        for (i = 0; i < 100; i++) {
                ret = anx7688_reg_read(anx7688, ANX7688_REG_EEPROM_LOAD_STATUS0);

                if (ret >= 0 && (ret & ANX7688_EEPROM_FW_LOADED) == ANX7688_EEPROM_FW_LOADED) {
                        dev_dbg(anx7688->dev, "eeprom0 = 0x%02x\n", ret);
                        dev_info(anx7688->dev, "fw loaded after %d ms\n", i * 10);
                        goto fw_loaded;
                }

                msleep(5);
        }

	set_bit(ANX7688_F_FW_FAILED, anx7688->flags);
        dev_err(anx7688->dev, "boot firmware load failed (you may need to flash FW to anx7688 first)\n");
        ret = -ETIMEDOUT;
        goto err_vconoff;

fw_loaded:
        ret = i2c_smbus_read_i2c_block_data(anx7688->client,
                                            ANX7688_REG_FW_VERSION1, 2, fw);
        if (ret < 0) {
                dev_err(anx7688->dev, "failed to read firmware version\n");
                goto err_vconoff;
        }

        dev_info(anx7688->dev, "OCM firmware loaded (version 0x%04x)\n",
                 fw[1] | fw[0] << 8);

        /* Unmask interrupts */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_STATUS_INT, 0);
	if (ret)
		goto err_vconoff;

        ret = anx7688_reg_write(anx7688, ANX7688_REG_STATUS_INT_MASK, ~ANX7688_SOFT_INT_MASK);
	if (ret)
		goto err_vconoff;

        ret = anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2, 0xff);
	if (ret)
		goto err_vconoff;

        ret = anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_MASK2, (u8)~ANX7688_IRQ2_SOFT_INT);
	if (ret)
		goto err_vconoff;


        /* time to turn off vbus after cc disconnect (unit is 4 ms) */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_VBUS_OFF_DELAY_TIME, 100 / 4);
	if (ret)
		goto err_vconoff;

        //anx7688_reg_write(anx7688, ANX7688_REG_TIME_CTRL, 0x00);

        /* 300ms (unit is 2 ms) */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_TRY_UFP_TIMER, 300 / 2);
	if (ret)
		goto err_vconoff;


        /* maximum voltage in 100 mV units */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_MAX_VOLTAGE, 50); /* 5 V */
	if (ret)
		goto err_vconoff;


        /* min/max power in 500 mW units */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_MAX_POWER, 15 * 2); /* 15 W */
	if (ret)
		goto err_vconoff;

        ret = anx7688_reg_write(anx7688, ANX7688_REG_MIN_POWER, 1);  /* 0.5 W */
	if (ret)
		goto err_vconoff;

        /* auto_pd, try.src, try.sink, goto safe 5V */
        ret = anx7688_reg_write(anx7688, ANX7688_REG_FEATURE_CTRL, 0x1e);
	if (ret)
		goto err_vconoff;


	pdo = cpu_to_le32(PDO_FIXED(5000, 500,
				    PDO_FIXED_DATA_SWAP |
				    PDO_FIXED_USB_COMM |
				    PDO_FIXED_DUAL_ROLE));
	ret = anx7688_send_ocm_message(anx7688, ANX7688_OCM_MSG_PWR_SRC_CAP,
				       (u8*)&pdo, sizeof pdo);
	if (ret)
		goto err_vconoff;


	pdo = cpu_to_le32(PDO_FIXED(5000, 3000,
				    PDO_FIXED_DATA_SWAP |
				    PDO_FIXED_USB_COMM |
				    PDO_FIXED_DUAL_ROLE));
	ret = anx7688_send_ocm_message(anx7688, ANX7688_OCM_MSG_PWR_SNK_CAP,
				       (u8*)&pdo, sizeof pdo);
	if (ret)
		goto err_vconoff;

	/* Send DP SNK identity */
	ret = anx7688_send_ocm_message(anx7688, ANX7688_OCM_MSG_DP_SNK_IDENTITY,
				       dp_snk_identity, sizeof dp_snk_identity);
	if (ret)
		goto err_vconoff;

	ret = anx7688_send_ocm_message(anx7688, ANX7688_OCM_MSG_SVID,
				       svid, sizeof svid);
	if (ret)
		goto err_vconoff;

        dev_dbg(anx7688->dev, "OCM configuration completed\n");

        desc.accessory = TYPEC_ACCESSORY_NONE;

	typec_unregister_partner(anx7688->partner);

        anx7688->partner = typec_register_partner(anx7688->port, &desc);
        if (IS_ERR(anx7688->partner)) {
                ret = PTR_ERR(anx7688->partner);
		goto err_vconoff;
	}

	set_bit(ANX7688_F_CONNECTED, anx7688->flags);
        return 0;

err_vconoff:
        regulator_disable(anx7688->supplies[ANX7688_VCONN_INDEX].consumer);
	anx7688->vconn_on = false;
err_poweroff:
        anx7688_power_disable(anx7688);
        return ret;
#endif
}

static void anx7688_disconnect(struct anx7688 *anx7688)
{
	union power_supply_propval val = {0,};
	struct device *dev = anx7688->dev;
	int ret;

        dev_dbg(dev, "cable removed\n");

	if (anx7688->vconn_on) {
		regulator_disable(anx7688->supplies[ANX7688_VCONN_INDEX].consumer);
		anx7688->vconn_on = false;
	}

	if (anx7688->vbus_on) {
		regulator_disable(anx7688->supplies[ANX7688_VBUS_INDEX].consumer);
		anx7688->vbus_on = false;
	}

        anx7688_power_disable(anx7688);

	typec_unregister_partner(anx7688->partner);
        anx7688->partner = NULL;

        typec_set_pwr_opmode(anx7688->port, TYPEC_PWR_MODE_USB);
        typec_set_pwr_role(anx7688->port, TYPEC_SINK);
        typec_set_vconn_role(anx7688->port, TYPEC_SINK);
        typec_set_data_role(anx7688->port, TYPEC_DEVICE);

	usb_role_switch_set_role(anx7688->role_sw, USB_ROLE_NONE);

	val.intval = 500 * 1000;
	dev_dbg(dev, "setting vbus_in current limit to %d mA\n", val.intval);
	ret = power_supply_set_property(anx7688->vbus_in_supply,
					POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
					&val);
	if (ret)
		dev_err(dev, "failed to set vbus_in current to %d mA\n",
			val.intval / 1000);

	val.intval = 0;
	dev_dbg(dev, "disabling vbus_in power path\n");
	ret = power_supply_set_property(anx7688->vbus_in_supply,
					POWER_SUPPLY_PROP_ONLINE,
					&val);
	if (ret)
		dev_err(dev, "failed to offline vbus_in\n");

	val.intval = 1;
	dev_dbg(dev, "enabling USB BC 1.2 detection\n");
	ret = power_supply_set_property(anx7688->vbus_in_supply,
					POWER_SUPPLY_PROP_USB_BC_ENABLED,
					&val);
	if (ret)
		dev_err(dev, "failed to enabled USB BC1.2 detection\n");

	clear_bit(ANX7688_F_CONNECTED, anx7688->flags);
}

static void anx7688_handle_cable_change(struct anx7688* anx7688)
{
        int cabledet;
	bool connected;

        mutex_lock(&anx7688->lock);

	connected = test_bit(ANX7688_F_CONNECTED, anx7688->flags);
        cabledet = gpiod_get_value(anx7688->gpio_cabledet);

        if (cabledet && !connected)
                anx7688_connect(anx7688);
        else if (!cabledet && connected)
                anx7688_disconnect(anx7688);

        mutex_unlock(&anx7688->lock);
}

static irqreturn_t anx7688_irq_plug_handler(int irq, void *data)
{
        struct anx7688 *anx7688 = data;

        dev_dbg(anx7688->dev, "plug irq (cd=%d)\n",
                gpiod_get_value(anx7688->gpio_cabledet));

        /*
         * After each cabledet change the scheduled work timer is reset
         * to fire in ~10ms. So the work is done only after the cabledet
         * is stable for ~10ms.
         */
        schedule_delayed_work(&anx7688->work, msecs_to_jiffies(10));

        return IRQ_HANDLED;
}

static int anx7688_handle_pd_message(struct anx7688* anx7688,
				     u8 cmd, u8* msg, unsigned len)
{
	switch (cmd) {
	case ANX7688_OCM_MSG_PWR_SRC_CAP:
	case ANX7688_OCM_MSG_PWR_SNK_CAP:
	case ANX7688_OCM_MSG_DP_SNK_IDENTITY:
	case ANX7688_OCM_MSG_SVID:
	case ANX7688_OCM_MSG_GET_DP_SNK_CAP:
	case ANX7688_OCM_MSG_ACCEPT:
	case ANX7688_OCM_MSG_REJECT:
	case ANX7688_OCM_MSG_PSWAP_REQ:
	case ANX7688_OCM_MSG_DSWAP_REQ:
	case ANX7688_OCM_MSG_GOTO_MIN_REQ:
	case ANX7688_OCM_MSG_VCONN_SWAP_REQ:
	case ANX7688_OCM_MSG_VDM:
	case ANX7688_OCM_MSG_DP_SNK_CFG:
	case ANX7688_OCM_MSG_PWR_OBJ_REQ:
	case ANX7688_OCM_MSG_PD_STATUS_REQ:
	case ANX7688_OCM_MSG_DP_ALT_ENTER:
	case ANX7688_OCM_MSG_DP_ALT_EXIT:
	case ANX7688_OCM_MSG_RESPONSE_TO_REQ:
	case ANX7688_OCM_MSG_SOFT_RST:
	case ANX7688_OCM_MSG_HARD_RST:
	case ANX7688_OCM_MSG_RESTART:
		break;
	}

	return 0;
}

static int anx7688_receive_msg(struct anx7688* anx7688)
{
        u8 pdo[32], checksum = 0;
        int i, ret;

        ret = i2c_smbus_read_i2c_block_data(anx7688->client_tcpc,
                                            ANX7688_TCPC_REG_INTERFACE_RECV,
                                            32, pdo);
        if (ret < 0) {
                dev_err(anx7688->dev, "failed to read pd msg\n");
                return ret;
        }

        ret = anx7688_tcpc_reg_write(anx7688, ANX7688_TCPC_REG_INTERFACE_RECV, 0);
        if (ret) {
                dev_warn(anx7688->dev, "failed to clear recv fifo\n");
        }

        if (pdo[0] == 0 || pdo[0] > sizeof(pdo) - 2) {
                dev_err(anx7688->dev, "received invalid pd message\n");
                return -EINVAL;
        }

        dev_dbg(anx7688->dev, "recv ocm message cmd=%d %*ph\n",
                pdo[1], pdo[0] + 2, pdo);

        for (i = 0; i < pdo[0] + 2; i++)
                checksum += pdo[i];

        if (checksum != 0) {
                dev_err(anx7688->dev, "bad checksum on received message\n");
                return -EINVAL;
        }

        anx7688_handle_pd_message(anx7688, pdo[1], pdo + 2, pdo[0] - 1);
        return 0;
}

static const char* anx7688_cc_status_string(unsigned v)
{
	switch (v) {
	case 0: return "SRC.Open";
	case 1: return "SRC.Rd";
	case 2: return "SRC.Ra";
	case 4: return "SNK.Default";
	case 8: return "SNK.Power1.5";
	case 12: return "SNK.Power3.0";
	default: return "UNK";
	}
}

static int anx7688_cc_status(unsigned v)
{
	switch (v) {
	case 0: return -1;
	case 1: return -1;
	case 2: return -1;
	case 4: return TYPEC_PWR_MODE_USB;
	case 8: return TYPEC_PWR_MODE_1_5A;
	case 12: return TYPEC_PWR_MODE_3_0A;
	default: return -1;
	}
}

static int anx7688_update_cc_status(struct anx7688 *anx7688, int cc_status)
{
        struct device *dev = anx7688->dev;
	union power_supply_propval val = {0,};
	int cc1, cc2, cc = -1, ret;

	cc1 = anx7688_cc_status(cc_status & 0xf);
	cc2 = anx7688_cc_status((cc_status >> 4) & 0xf);
	if (cc1 >= 0) {
		cc = cc1;
	} else if (cc2 >= 0) {
		cc = cc2;
	}

	if (cc < 0)
		return 0;

	anx7688->current_limit = 0;
	if (cc == TYPEC_PWR_MODE_1_5A)
		anx7688->current_limit = 1500 * 1000;
	else if (cc == TYPEC_PWR_MODE_3_0A)
		anx7688->current_limit = 3000 * 1000;

	if (anx7688->current_limit) {
		/*
		 * Disable BC1.2 detection, because we'll be setting
		 * a current limit determined by USB-PD
		 */
		val.intval = 0;
		dev_dbg(dev, "disabling USB BC 1.2 detection\n");
		ret = power_supply_set_property(anx7688->vbus_in_supply,
						POWER_SUPPLY_PROP_USB_BC_ENABLED,
						&val);
		if (ret)
			dev_err(dev, "failed to disable USB BC1.2 detection\n");

		val.intval = anx7688->current_limit;
		dev_dbg(dev, "setting vbus_in current limit to %d mA\n", val.intval);
		ret = power_supply_set_property(anx7688->vbus_in_supply,
						POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
						&val);
		if (ret)
			dev_err(dev, "failed to set vbus_in current to %d mA\n",
				val.intval / 1000);
	} else {
		/*
		 * Use the result of BC1.2 detection performed by PMIC.
		 */
		ret = power_supply_get_property(anx7688->vbus_in_supply,
						POWER_SUPPLY_PROP_USB_BC_ENABLED,
						&val);
		if (ret)
			dev_err(dev, "failed to get USB BC1.2 detection status\n");

		if (ret != 0 || val.intval == 0) {
			/*
			 * If BC is disabled or we can't get its status,
			 * set conservative 500mA limit. Otherwise leave
			 * the limit to BC1.2.
			 */
			val.intval = 500 * 1000;
			dev_dbg(dev, "setting vbus_in current limit to %d mA\n", val.intval);
			ret = power_supply_set_property(anx7688->vbus_in_supply,
							POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
							&val);
			if (ret)
				dev_err(dev, "failed to set vbus_in current to %d mA\n",
					val.intval / 1000);
		}
	}

	/* Turn on VBUS power path inside PMIC. */
	val.intval = 1;
	dev_dbg(dev, "enabling vbus_in power path\n");
	ret = power_supply_set_property(anx7688->vbus_in_supply,
					POWER_SUPPLY_PROP_ONLINE,
					&val);
	if (ret)
		dev_err(anx7688->dev, "failed to offline vbus_in\n");

	typec_set_pwr_opmode(anx7688->port, cc);
	return 0;
}

static int anx7688_update_status(struct anx7688 *anx7688)
{
        struct device *dev = anx7688->dev;
	bool vbus_on, vconn_on, dr_dfp;
	int status, cc_status, dp_state, ret;

	status = anx7688_reg_read(anx7688, ANX7688_REG_STATUS);
	if (status < 0)
		return status;

	cc_status = anx7688_reg_read(anx7688, ANX7688_REG_CC_STATUS);
	if (cc_status < 0)
		return cc_status;

	dp_state = anx7688_tcpc_reg_read(anx7688, 0x87);
	if (dp_state < 0)
		return dp_state;

	if (anx7688->last_status == -1 || anx7688->last_status != status) {
		anx7688->last_status = status;
		dev_dbg(dev, "status changed to 0x%02x\n", status);
	}

	if (anx7688->last_cc_status == -1 || anx7688->last_cc_status != cc_status) {
		anx7688->last_cc_status = cc_status;
		dev_dbg(dev, "cc_status changed to CC1 = %s CC2 = %s\n",
			anx7688_cc_status_string(cc_status & 0xf),
			anx7688_cc_status_string((cc_status >> 4) & 0xf));

		anx7688_update_cc_status(anx7688, cc_status);
	}

	if (anx7688->last_dp_state == -1 || anx7688->last_dp_state != dp_state) {
		anx7688->last_dp_state = dp_state;
		dev_dbg(dev, "dp state changed to 0x%02x\n", dp_state);
	}

	vbus_on = !!(status & ANX7688_VBUS_STATUS);
	vconn_on = !!(status & ANX7688_VCONN_STATUS);
	dr_dfp = !!(status & ANX7688_DATA_ROLE_STATUS);

	if (anx7688->vbus_on != vbus_on) {
		if (vbus_on) {
			ret = regulator_enable(anx7688->supplies[ANX7688_VBUS_INDEX].consumer);
			if (ret) {
				dev_err(anx7688->dev, "failed to enable vbus\n");
				return ret;
			}
		} else {
			ret = regulator_disable(anx7688->supplies[ANX7688_VBUS_INDEX].consumer);
			if (ret) {
				dev_err(anx7688->dev, "failed to disable vbus\n");
				return ret;
			}
		}

		typec_set_pwr_role(anx7688->port, vbus_on ? TYPEC_SOURCE : TYPEC_SINK);
		anx7688->vbus_on = vbus_on;
	}

	if (anx7688->vconn_on != vconn_on) {
		if (vconn_on) {
			ret = regulator_enable(anx7688->supplies[ANX7688_VCONN_INDEX].consumer);
			if (ret) {
				dev_err(anx7688->dev, "failed to enable vconn\n");
				return ret;
			}
		} else {
			ret = regulator_disable(anx7688->supplies[ANX7688_VCONN_INDEX].consumer);
			if (ret) {
				dev_err(anx7688->dev, "failed to disable vconn\n");
				return ret;
			}
		}

		typec_set_vconn_role(anx7688->port, vconn_on ? TYPEC_SOURCE : TYPEC_SINK);
		anx7688->vconn_on = vconn_on;
	}

	typec_set_data_role(anx7688->port, dr_dfp ? TYPEC_HOST : TYPEC_DEVICE);

	if (usb_role_switch_get_role(anx7688->role_sw) !=
	    (dr_dfp ? USB_ROLE_HOST : USB_ROLE_DEVICE)) {
		dev_dbg(anx7688->dev, "data role change requested to %s\n",
			dr_dfp ? "dfp" : "ufp");

		ret = usb_role_switch_set_role(anx7688->role_sw,
					       dr_dfp ? USB_ROLE_HOST : USB_ROLE_DEVICE);
		if (ret)
			return ret;
	}

	return 0;
}

static irqreturn_t anx7688_irq_status_handler(int irq, void *data)
{
#if DISABLE_OCM
        struct anx7688 *anx7688 = data;
        struct device *dev = anx7688->dev;
        int ext2_status;

        mutex_lock(&anx7688->lock);

        if (!test_bit(ANX7688_F_CONNECTED, anx7688->flags)) {
                /*
		 * The chip should be disabled and powered off, nothing
                 * more to do.
		 */
                dev_dbg(dev, "spurious status irq\n");
                goto out_unlock;
        }

        //dev_dbg(dev, "status irq\n");

        ext2_status = anx7688_reg_read(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2);
	dev_dbg(dev, "  ext2 = 0x%02x\n", ext2_status);
	anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2, ext2_status);

out_unlock:
        mutex_unlock(&anx7688->lock);

        return IRQ_HANDLED;
#else
        struct anx7688 *anx7688 = data;
        struct device *dev = anx7688->dev;
        int tcpc_status, ext2_status, soft_status;

        mutex_lock(&anx7688->lock);

        if (!test_bit(ANX7688_F_CONNECTED, anx7688->flags)) {
                dev_dbg(dev, "spurious status irq\n");
                /* anx chip should be disabled and power off, nothing
                 * more to do */
                goto out_unlock;
        }

        //dev_dbg(dev, "status irq\n");

        // clear tcpc interrupt
        tcpc_status = anx7688_tcpc_reg_read(anx7688, ANX7688_TCPC_REG_ALERT0);
        if (tcpc_status > 0) {
                //dev_dbg(dev, "  tcpc = 0x%02x\n", tcpc_status);
                anx7688_tcpc_reg_write(anx7688, ANX7688_TCPC_REG_ALERT0, tcpc_status);
        }

        ext2_status = anx7688_reg_read(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2);
        if (ext2_status & ANX7688_IRQ2_SOFT_INT) {
                soft_status = anx7688_reg_read(anx7688, ANX7688_REG_STATUS_INT);
                anx7688_reg_write(anx7688, ANX7688_REG_STATUS_INT, 0);

                //dev_dbg(dev, "  soft = 0x%02x\n", soft_status);

		if (soft_status > 0) {
			soft_status &= ANX7688_SOFT_INT_MASK;

			if (soft_status & ANX7688_IRQS_RECEIVED_MSG)
				anx7688_receive_msg(anx7688);

			if (soft_status & (ANX7688_IRQS_CC_STATUS_CHANGE |
					   ANX7688_IRQS_VBUS_CHANGE |
					   ANX7688_IRQS_VCONN_CHANGE |
					   ANX7688_IRQS_DATA_ROLE_CHANGE)) {
				anx7688_update_status(anx7688);
			}
		}

                anx7688_reg_write(anx7688, ANX7688_REG_IRQ_EXT_SOURCE2, ANX7688_IRQ2_SOFT_INT);
        }

out_unlock:
        mutex_unlock(&anx7688->lock);

        return IRQ_HANDLED;
#endif
}

static int anx7688_dr_set(struct typec_port *port, enum typec_data_role role)
{
        struct anx7688 *anx7688 = typec_get_drvdata(port);

        dev_info(anx7688->dev, "data role set %d\n", role);

        return -ENOTSUPP;
}

static int anx7688_pr_set(struct typec_port *port, enum typec_role role)
{
        struct anx7688 *anx7688 = typec_get_drvdata(port);

        dev_info(anx7688->dev, "power role set %d\n", role);

        return -ENOTSUPP;
}

/*
 * Calls to the eerpom functions need to be taken under the anx7688 lock.
 */

static int anx7688_eeprom_set_address(struct anx7688 *anx7688, u16 addr)
{
        int ret;

        ret = anx7688_reg_write(anx7688, 0xe0, (addr >> 8) & 0xff);
        if (ret < 0)
                return ret;

        return anx7688_reg_write(anx7688, 0xe1, addr & 0xff);
}

static int anx7688_eeprom_wait_done(struct anx7688 *anx7688)
{
        ktime_t timeout;
        int ret;

        // wait for read to be done
        timeout = ktime_add_us(ktime_get(), 10000);
        while (true) {
                ret = anx7688_reg_read(anx7688, 0xe2);
                if (ret < 0)
                        return ret;

                if (ret & BIT(3))
                        return 0;

                if (ktime_after(ktime_get(), timeout)) {
                        dev_err(anx7688->dev, "timeout waiting for eeprom\n");
                        return -ETIMEDOUT;
                }
        }
}

/* wait for internal FSM of EEPROM to be in a state ready for
 * programming/reading
 */
static int anx7688_eeprom_wait_ready(struct anx7688 *anx7688)
{
        ktime_t timeout;
        int ret;

        // wait until eeprom is ready
        timeout = ktime_add_us(ktime_get(), 1000000);
        while (true) {
                ret = anx7688_reg_read(anx7688, 0x7f);
                if (ret < 0)
                        return ret;

                if ((ret & 0x0f) == 7)
                        return 0;

                if (ktime_after(ktime_get(), timeout)) {
                        dev_err(anx7688->dev, "timeout waiting for eeprom to initialize\n");
                        return -ETIMEDOUT;
                }

                msleep(5);
        }
}

static int anx7688_eeprom_read(struct anx7688 *anx7688, unsigned addr, u8 buf[16])
{
        int ret;

        ret = anx7688_eeprom_set_address(anx7688, addr);
        if (ret)
                return ret;

        // initiate read
        ret = anx7688_reg_write(anx7688, 0xe2, 0x06);
        if (ret < 0)
                return ret;

        ret = anx7688_eeprom_wait_done(anx7688);
        if (ret)
                return ret;

        ret = i2c_smbus_read_i2c_block_data(anx7688->client, 0xd0, 16, buf);
        if (ret < 0) {
                dev_err(anx7688->dev,
                        "failed to read eeprom data (err=%d)\n", ret);
                return ret;
        }

        return 0;
}

static int anx7688_eeprom_write(struct anx7688 *anx7688, unsigned addr,
                                const u8 buf[16])
{
        int ret;

        ret = anx7688_eeprom_set_address(anx7688, addr);
        if (ret)
                return ret;

        ret = i2c_smbus_write_i2c_block_data(anx7688->client, 0xd0, 16, buf);
        if (ret < 0) {
                dev_err(anx7688->dev,
                        "failed to write eeprom data (err=%d)\n", ret);
                return ret;
        }

        dev_info(anx7688->dev, "eeprom wr %x %*ph\n", addr, 16, buf);

        // initiate write
        ret = anx7688_reg_write(anx7688, 0xe2, 0x01);
        if (ret < 0)
                return ret;

        ret = anx7688_eeprom_wait_done(anx7688);
        if (ret)
                return ret;

        return 0;
}

/*
 * The firmware is flashed as is from address 0x10 up to 0xffff.
 */
static int anx7688_flash_firmware(struct anx7688 *anx7688)
{
        const char* fw_name = "anx7688-fw.bin";
        const struct firmware *fw;
        unsigned addr;
        u8 buf[16];
        int ret;

        ret = request_firmware(&fw, fw_name, anx7688->dev);
        if (ret < 0)
                return ret;

        if (fw->size > 0x10000 - 16) {
                dev_err(anx7688->dev, "Firmware is too big %s\n", fw_name);
                ret = -E2BIG;
                goto err_release;
        }

        mutex_lock(&anx7688->lock);

	/*
	 * Cabledet changes will not be processed, since we're holding the
	 * lock, so the firmware flashing is safe from interruptions.
	 */

	if (test_bit(ANX7688_F_CONNECTED, anx7688->flags))
                anx7688_disconnect(anx7688);

	msleep(20);

	anx7688_power_enable(anx7688);

        // reset OCM
        ret = anx7688_reg_update_bits(anx7688, ANX7688_REG_USBC_RESET_CTRL,
                                      ANX7688_USBC_RESET_CTRL_OCM_RESET,
                                      ANX7688_USBC_RESET_CTRL_OCM_RESET);
        if (ret < 0)
                goto err_unlock;

        ret = anx7688_eeprom_wait_ready(anx7688);
        if (ret)
                goto err_unlock;

        msleep(10);

        ret = anx7688_reg_update_bits(anx7688, 0x3f, BIT(5), BIT(5));
        if (ret < 0)
                goto err_unlock;

        ret = anx7688_reg_update_bits(anx7688, 0x44,
                                      BIT(0) | BIT(7),
                                      BIT(0) | BIT(7));
        if (ret < 0)
                goto err_unlock;

        ret = anx7688_reg_update_bits(anx7688, 0x66, BIT(3), BIT(3));
        if (ret < 0)
                goto err_unlock;

        for (addr = 0; addr < fw->size; addr += sizeof buf) {
                memset(buf, 0, sizeof buf);
                memcpy(buf, fw->data + addr, min_t(unsigned, fw->size - addr, sizeof buf));

                ret = anx7688_eeprom_write(anx7688, addr + 0x10, buf);
                if (ret < 0)
                        goto err_unlock;
        }

	clear_bit(ANX7688_F_FW_FAILED, anx7688->flags);

err_unlock:
	anx7688_power_disable(anx7688);
	schedule_delayed_work(&anx7688->work, msecs_to_jiffies(20));
        mutex_unlock(&anx7688->lock);

err_release:
        release_firmware(fw);
        return ret;
}

static const struct typec_operations anx7688_typec_ops = {
        .dr_set = anx7688_dr_set,
        .pr_set = anx7688_pr_set,
};

static ssize_t flash_eeprom_show(struct device *dev,
                           struct device_attribute *attr, char *buf)
{
        return scnprintf(buf, PAGE_SIZE, "Write 1 to this file to initiate firmware flashing. Consult dmesg for results.\n");
}

static ssize_t flash_eeprom_store(struct device *dev,
                            struct device_attribute *attr,
                            const char *buf, size_t len)
{
        struct anx7688 *anx7688 = i2c_get_clientdata(to_i2c_client(dev));
        unsigned val;
        int ret;

        ret = kstrtouint(buf, 0, &val);
        if (ret)
                return ret;

        if (val == 1) {
                ret = anx7688_flash_firmware(anx7688);
                if (ret)
                        return ret;
        }

        return len;
}
static DEVICE_ATTR_RW(flash_eeprom);

static ssize_t reg_update_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
        struct anx7688 *anx7688 = i2c_get_clientdata(to_i2c_client(dev));
        unsigned val, addr, devaddr, data;
        int ret;

        ret = kstrtouint(buf, 16, &val);
        if (ret)
                return ret;

	data = val & 0xff;
	addr = (val >> 8) & 0xff;
	devaddr = (val >> 16) & 0xff;

	mutex_lock(&anx7688->lock);

	if (!test_bit(ANX7688_F_POWERED, anx7688->flags)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	if (devaddr == 0x50)
		ret = anx7688_reg_write(anx7688, addr, val);
	else if (devaddr == 0x58)
		ret = anx7688_tcpc_reg_write(anx7688, addr, val);
	else
		ret = -EINVAL;

out_unlock:
	mutex_unlock(&anx7688->lock);

        return ret ? ret : len;
}
static DEVICE_ATTR_WO(reg_update);

static ssize_t hwreset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
        struct anx7688 *anx7688 = i2c_get_clientdata(to_i2c_client(dev));

	mutex_lock(&anx7688->lock);

	if (test_bit(ANX7688_F_CONNECTED, anx7688->flags))
                anx7688_disconnect(anx7688);

	schedule_delayed_work(&anx7688->work, msecs_to_jiffies(20));
	mutex_unlock(&anx7688->lock);

        return len;
}
static DEVICE_ATTR_WO(hwreset);

static struct attribute *anx7688_attrs[] = {
        &dev_attr_flash_eeprom.attr,
        &dev_attr_reg_update.attr,
        &dev_attr_hwreset.attr,
        NULL,
};

ATTRIBUTE_GROUPS(anx7688);

/*
 * This function has to work when the ANX7688 is active, and when
 * it is powered down. It power cycles the chip and asserts the OCM
 * reset, to prevent OCM FW interfering with EEPROM reading.
 *
 * After reading EEPROM, the reconnection is scheduled.
 */
static int anx7688_firmware_show(struct seq_file *s, void *data)
{
        struct anx7688 *anx7688 = s->private;
        unsigned addr;
        u8 buf[16];
        int ret;

        mutex_lock(&anx7688->lock);

	if (test_bit(ANX7688_F_CONNECTED, anx7688->flags))
		anx7688_disconnect(anx7688);

	msleep(20);

	anx7688_power_enable(anx7688);

	ret = anx7688_reg_update_bits(anx7688, ANX7688_REG_USBC_RESET_CTRL,
				      ANX7688_USBC_RESET_CTRL_OCM_RESET,
				      ANX7688_USBC_RESET_CTRL_OCM_RESET);
	if (ret < 0)
		goto out_powerdown;

        ret = anx7688_eeprom_wait_ready(anx7688);
        if (ret)
                goto out_powerdown;

	msleep(10);

        for (addr = 0x10; addr < 0x10000; addr += 16) {
                // set address
                ret = anx7688_eeprom_read(anx7688, addr, buf);
                if (ret < 0)
                        goto out_powerdown;

                seq_write(s, buf, sizeof buf);
        }

out_powerdown:
	anx7688_power_disable(anx7688);
        schedule_delayed_work(&anx7688->work, 0);
        mutex_unlock(&anx7688->lock);

        return ret;
}
DEFINE_SHOW_ATTRIBUTE(anx7688_firmware);

static int anx7688_regs_show(struct seq_file *s, void *data)
{
        struct anx7688 *anx7688 = s->private;
        u8 buf[16];
        unsigned i, addr;
        int ret = -ENODEV;

        mutex_lock(&anx7688->lock);

	if (!test_bit(ANX7688_F_POWERED, anx7688->flags))
		goto out_unlock;

        for (addr = 0; addr < 256; addr += 16) {
                ret = i2c_smbus_read_i2c_block_data(anx7688->client, addr,
                                                    sizeof buf, buf);
                if (ret < 0) {
                        dev_err(anx7688->dev,
                                "failed to read registers (err=%d)\n", ret);
                        goto out_unlock;
                }

                for (i = 0; i < 16; i++)
                        seq_printf(s, "50%02x: %02x\n", addr + i, buf[i]);
        }

        for (addr = 0; addr < 256; addr += 16) {
                ret = i2c_smbus_read_i2c_block_data(anx7688->client_tcpc, addr,
                                                    sizeof buf, buf);
                if (ret < 0) {
                        dev_err(anx7688->dev,
                                "failed to read registers (err=%d)\n", ret);
                        goto out_unlock;
                }

                for (i = 0; i < 16; i++)
                        seq_printf(s, "58%02x: %02x\n", addr + i, buf[i]);
        }

out_unlock:
        mutex_unlock(&anx7688->lock);

        return ret;
}
DEFINE_SHOW_ATTRIBUTE(anx7688_regs);

static int anx7688_status_show(struct seq_file *s, void *data)
{
        struct anx7688 *anx7688 = s->private;

        mutex_lock(&anx7688->lock);

	seq_printf(s, "not much\n");

        mutex_unlock(&anx7688->lock);

        return 0;
}
DEFINE_SHOW_ATTRIBUTE(anx7688_status);

static void anx7688_cabledet_timer_fn(struct timer_list *t)
{
	struct anx7688 *anx7688 = from_timer(anx7688, t, work_timer);

	schedule_delayed_work(&anx7688->work, 0);
	mod_timer(t, jiffies + msecs_to_jiffies(1000));
}

static void anx7688_handle_vbus_in_notify(struct anx7688* anx7688)
{
	union power_supply_propval psy_val = {0,};
	struct device *dev = anx7688->dev;
	int ret;

	/* PD charger doesn't like this for some reason, so it's disabled for now */
	if (anx7688->current_limit && false) {
		ret = power_supply_get_property(anx7688->vbus_in_supply,
						POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
						&psy_val);
		if (ret) {
			dev_err(dev, "failed to get vbus_in current\n");
			return;
		}

		if (psy_val.intval == anx7688->current_limit)
			goto check_type;

		psy_val.intval = anx7688->current_limit;
		dev_dbg(dev, "setting vbus_in current limit to %d mA\n", psy_val.intval);
		ret = power_supply_set_property(anx7688->vbus_in_supply,
						POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
						&psy_val);
		if (ret)
			dev_err(dev, "failed to set vbus_in current to %d mA\n",
				psy_val.intval / 1000);
	}

check_type:
	ret = power_supply_get_property(anx7688->vbus_in_supply,
					POWER_SUPPLY_PROP_USB_TYPE,
					&psy_val);
	if (ret) {
		dev_err(dev, "failed to get USB BC1.2 result\n");
		return;
	}

	if (anx7688->last_bc_result == psy_val.intval)
		return;

	anx7688->last_bc_result = psy_val.intval;

	switch (psy_val.intval) {
	case POWER_SUPPLY_USB_TYPE_DCP:
	case POWER_SUPPLY_USB_TYPE_CDP:
		dev_dbg(dev, "BC 1.2 result: DCP or CDP\n");
		break;
	case POWER_SUPPLY_USB_TYPE_SDP:
	default:
		dev_dbg(dev, "BC 1.2 result: SDP\n");
		break;
	}
}

static int anx7688_vbus_in_notify(struct notifier_block *nb,
				  unsigned long val, void *v)
{
	struct anx7688 *anx7688 = container_of(nb, struct anx7688, vbus_in_nb);
	struct power_supply *psy = v;

	/* atomic context */
	if (val == PSY_EVENT_PROP_CHANGED && psy == anx7688->vbus_in_supply) {
		set_bit(ANX7688_F_PWRSUPPLY_CHANGE, anx7688->flags);
		schedule_delayed_work(&anx7688->work, 0);
	}

	return NOTIFY_OK;
}

static void anx7688_work(struct work_struct *work)
{
        struct anx7688 *anx7688 = container_of(work, struct anx7688, work.work);

	if (test_bit(ANX7688_F_FW_FAILED, anx7688->flags))
		return;

	if (test_and_clear_bit(ANX7688_F_PWRSUPPLY_CHANGE, anx7688->flags))
		anx7688_handle_vbus_in_notify(anx7688);

        anx7688_handle_cable_change(anx7688);

	if (test_bit(ANX7688_F_CONNECTED, anx7688->flags)) {
		/*
		 * We check status periodically outside of interrupt, just to
		 * be sure we didn't miss any status interrupts
		 */
		mutex_lock(&anx7688->lock);
		anx7688_update_status(anx7688);
		mutex_unlock(&anx7688->lock);
	}
}

static int anx7688_i2c_probe(struct i2c_client *client,
                             const struct i2c_device_id *id)
{
        struct anx7688 *anx7688;
        struct device *dev = &client->dev;
        struct typec_capability typec_cap = { };
	union power_supply_propval psy_val;
        int i, vid_h, vid_l;
        int irq_cabledet;
        int ret = 0;

        anx7688 = devm_kzalloc(dev, sizeof(*anx7688), GFP_KERNEL);
        if (!anx7688)
                return -ENOMEM;

        i2c_set_clientdata(client, anx7688);
        anx7688->client = client;
        anx7688->dev = &client->dev;
        mutex_init(&anx7688->lock);
        INIT_DELAYED_WORK(&anx7688->work, anx7688_work);

        for (i = 0; i < ANX7688_NUM_SUPPLIES; i++)
                anx7688->supplies[i].supply = anx7688_supply_names[i];
        ret = devm_regulator_bulk_get(dev, ANX7688_NUM_SUPPLIES,
                                      anx7688->supplies);
        if (ret)
                return ret;

	anx7688->vbus_in_supply =
		devm_power_supply_get_by_phandle(dev, "vbus_in-supply");
	if (IS_ERR(anx7688->vbus_in_supply)) {
		dev_err(dev, "Couldn't get the VBUS power supply\n");
		return PTR_ERR(anx7688->vbus_in_supply);
	}

	if (!anx7688->vbus_in_supply)
		return -EPROBE_DEFER;

        anx7688->gpio_enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
        if (IS_ERR(anx7688->gpio_enable)) {
                dev_err(dev, "Could not get enable gpio\n");
                return PTR_ERR(anx7688->gpio_enable);
        }

        anx7688->gpio_reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
        if (IS_ERR(anx7688->gpio_reset)) {
                dev_err(dev, "Could not get reset gpio\n");
                return PTR_ERR(anx7688->gpio_reset);
        }

        anx7688->gpio_cabledet = devm_gpiod_get(dev, "cabledet", GPIOD_IN);
        if (IS_ERR(anx7688->gpio_cabledet)) {
                dev_err(dev, "Could not get cabledet gpio\n");
                return PTR_ERR(anx7688->gpio_cabledet);
        }

        irq_cabledet = gpiod_to_irq(anx7688->gpio_cabledet);
        if (irq_cabledet < 0) {
                dev_err(dev, "Could not get cabledet irq\n");
                return irq_cabledet;
        }

        ret = devm_device_add_groups(&client->dev, anx7688_groups);
        if (ret)
                return ret;

        // Register the TCPC i2c interface as second interface (0x58)
        anx7688->client_tcpc = i2c_new_dummy_device(client->adapter, 0x2c);
        if (IS_ERR(anx7688->client_tcpc)) {
                dev_err(dev, "Could not register tcpc i2c client\n");
                return PTR_ERR(anx7688->client_tcpc);
        }
        i2c_set_clientdata(anx7688->client_tcpc, anx7688);

        // powerup and probe the ANX chip

        ret = regulator_bulk_enable(ANX7688_NUM_ALWAYS_ON_SUPPLIES,
                                    anx7688->supplies);
        if (ret) {
                dev_err(dev, "Could not enable regulators\n");
                goto err_dummy_dev;
        }

        msleep(10);

        anx7688_power_enable(anx7688);

        vid_l = anx7688_tcpc_reg_read(anx7688, ANX7688_TCPC_REG_VENDOR_ID0);
        vid_h = anx7688_tcpc_reg_read(anx7688, ANX7688_TCPC_REG_VENDOR_ID1);
	if (vid_l < 0 || vid_h < 0) {
		anx7688_power_disable(anx7688);
		goto err_disable_reg;
	}

        dev_info(dev, "Vendor id 0x%04x\n", vid_l | vid_h << 8);

        anx7688_power_disable(anx7688);

	anx7688->role_sw = usb_role_switch_get(dev);
	if (IS_ERR(anx7688->role_sw)) {
                dev_err(dev, "Could not get role switch\n");
		ret = PTR_ERR(anx7688->role_sw);
		goto err_disable_reg;
	}

	// setup a typec port device
        typec_cap.revision = USB_TYPEC_REV_1_2;
        typec_cap.pd_revision = 0x200;
        typec_cap.prefer_role = TYPEC_NO_PREFERRED_ROLE;
        typec_cap.type = TYPEC_PORT_DRP;
        typec_cap.data = TYPEC_PORT_DRD;
        typec_cap.driver_data = anx7688;
        typec_cap.ops = &anx7688_typec_ops;

        anx7688->port = typec_register_port(dev, &typec_cap);
        if (IS_ERR(anx7688->port)) {
                dev_err(dev, "Could not register type-c port\n");
                ret = PTR_ERR(anx7688->port);
                goto err_role_sw;
        }

        typec_set_pwr_opmode(anx7688->port, TYPEC_PWR_MODE_USB);
        typec_set_pwr_role(anx7688->port, TYPEC_SINK);
        typec_set_vconn_role(anx7688->port, TYPEC_SINK);
        typec_set_data_role(anx7688->port, TYPEC_DEVICE);

        ret = devm_request_irq(dev, irq_cabledet, anx7688_irq_plug_handler,
                               IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                               "anx7688-cabledet", anx7688);
        if (ret < 0) {
                dev_err(dev, "Could not request cabledet irq (%d)\n", ret);
                goto err_cport;
        }

        ret = devm_request_threaded_irq(dev, client->irq,
                                        NULL, anx7688_irq_status_handler,
                                        IRQF_ONESHOT, NULL, anx7688);
        if (ret < 0) {
                dev_err(dev, "Could not request irq (%d)\n", ret);
                goto err_cport;
        }

	// enable BC1.2 detection in PMIC and set current limit to 500mA until
	// proper limit is established via BC1.2 or USB-PD
	psy_val.intval = 1;
	dev_dbg(dev, "enabling USB BC 1.2 detection\n");
	ret = power_supply_set_property(anx7688->vbus_in_supply,
			POWER_SUPPLY_PROP_USB_BC_ENABLED,
			&psy_val);
	if (ret) {
		dev_err(anx7688->dev, "failed to disable BC1.2 detection\n");
		goto err_cport;
	}

	psy_val.intval = 500000;
	dev_dbg(dev, "setting vbus_in current limit to %d mA\n", psy_val.intval);
	ret = power_supply_set_property(anx7688->vbus_in_supply,
			POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
			&psy_val);
	if (ret) {
		dev_err(anx7688->dev, "failed to set vbus_in current to %d mA\n",
			psy_val.intval / 1000);
		goto err_cport;
	}

	anx7688->last_bc_result = -1;
	anx7688->vbus_in_nb.notifier_call = anx7688_vbus_in_notify;
	anx7688->vbus_in_nb.priority = 0;
	ret = power_supply_reg_notifier(&anx7688->vbus_in_nb);
	if (ret)
		goto err_cport;

        anx7688->debug_root = debugfs_create_dir("anx7688", NULL);
        debugfs_create_file("firmware", 0444, anx7688->debug_root, anx7688,
                            &anx7688_firmware_fops);
        debugfs_create_file("regs", 0444, anx7688->debug_root, anx7688,
                            &anx7688_regs_fops);
        debugfs_create_file("status", 0444, anx7688->debug_root, anx7688,
                            &anx7688_status_fops);

        schedule_delayed_work(&anx7688->work, msecs_to_jiffies(10));

	timer_setup(&anx7688->work_timer, anx7688_cabledet_timer_fn, 0);
	mod_timer(&anx7688->work_timer, jiffies + msecs_to_jiffies(1000));

        return 0;

err_cport:
        typec_unregister_port(anx7688->port);
err_role_sw:
	usb_role_switch_put(anx7688->role_sw);
err_disable_reg:
        regulator_bulk_disable(ANX7688_NUM_ALWAYS_ON_SUPPLIES, anx7688->supplies);
err_dummy_dev:
        i2c_unregister_device(anx7688->client_tcpc);
        return ret;
}

static int anx7688_i2c_remove(struct i2c_client *client)
{
        struct anx7688 *anx7688 = i2c_get_clientdata(client);

	mutex_lock(&anx7688->lock);

	power_supply_unreg_notifier(&anx7688->vbus_in_nb);

	del_timer_sync(&anx7688->work_timer);
        cancel_delayed_work_sync(&anx7688->work);

	if (test_bit(ANX7688_F_CONNECTED, anx7688->flags))
		anx7688_disconnect(anx7688);

	typec_unregister_partner(anx7688->partner);
        typec_unregister_port(anx7688->port);
	usb_role_switch_put(anx7688->role_sw);

        regulator_bulk_disable(ANX7688_NUM_ALWAYS_ON_SUPPLIES, anx7688->supplies);
        i2c_unregister_device(anx7688->client_tcpc);

        debugfs_remove(anx7688->debug_root);

	mutex_unlock(&anx7688->lock);

        return 0;
}

static const struct i2c_device_id anx7688_ids[] = {
        { "anx7688", 0 },
        { }
};
MODULE_DEVICE_TABLE(i2c, anx7688_ids);

#ifdef CONFIG_OF
static struct of_device_id anx7688_of_match_table[] = {
        { .compatible = "analogix,anx7688" },
        { },
};
MODULE_DEVICE_TABLE(of, anx7688_of_match_table);
#endif

static struct i2c_driver anx7688_driver = {
        .driver = {
                .name = "anx7688",
#ifdef CONFIG_OF
                .of_match_table = anx7688_of_match_table,
#endif
        },
        .probe = anx7688_i2c_probe,
        .remove = anx7688_i2c_remove,
        .id_table = anx7688_ids,
};

module_i2c_driver(anx7688_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Martijn Braam <martijn@brixit.nl>");
MODULE_AUTHOR("Ondrej Jirman <megous@megous.com>");
MODULE_DESCRIPTION("Analogix ANX7688 USB-C DisplayPort bridge");