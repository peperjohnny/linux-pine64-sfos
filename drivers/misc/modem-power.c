/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Modem power control driver.
 *
 * Ondrej Jirman <megous@megous.com>
 *
 * How this works
 * --------------
 *
 * The driver:
 * - can be registered as a platform or serial device
 * - will use gpios, regulator and (optionally) serial port to control the modem
 * - exposes a character device to control the modem power and receive various
 *   events
 * - exposes sysfs interface to control modem power and wakeup
 * - supports multiple modem types and instances
 *
 * Power up/power down:
 * - may take a lot of time (eg. ~13-22s powerup, >22s powerdown)
 * - happens on a private workqueue under a lock
 * - may happen from shutdown hook
 * - prevents suspend when powerup/powerdown is in progress
 * - is serialized and there's no abort of in-progress operations
 * - for specific power sequence see comments in the section for each
 *   supported modem variant
 * - the driver monitors the power status of the modem (optionally)
 *   and tries to complete the powerdown initiated via AT command
 * - the driver tries to detect when the modem is killswitched off
 *   and updates the driver status to reflect that
 *
 * Suspend/resume:
 * - suspend is blocked if powerup/down is in progress
 * - modem can wakeup the host over gpio based IRQ (RI signal)
 * - the driver will assert ap_ready after resume finishes
 *
 * Rfkill:
 * - the driver implements a rfkill interface if rfkill gpio is available
 */

//#define DEBUG

#include <linux/wait.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/serdev.h>
#include <linux/rfkill.h>

#define DRIVER_NAME "modem-power"

enum {
	MPWR_REQ_NONE = 0,
	MPWR_REQ_RESET,
	MPWR_REQ_PWDN,
	MPWR_REQ_PWUP,
};

struct mpwr_dev;

struct mpwr_gpio {
	const char* name;
	unsigned desc_off;
	int flags;
	bool required;
	int irq_flags;
	unsigned irq_off;
};

#define MPWR_GPIO_DEF(_name, _flags, _req) \
	{ .name = #_name, \
	  .desc_off = offsetof(struct mpwr_dev, _name##_gpio), \
	  .flags = _flags, \
	  .required = _req, \
	}

#define MPWR_GPIO_DEF_IRQ(_name, _flags, _req, _irq_flags) \
	{ .name = #_name, \
	  .desc_off = offsetof(struct mpwr_dev, _name##_gpio), \
	  .flags = _flags, \
	  .required = _req, \
	  .irq_flags = _irq_flags, \
	  .irq_off = offsetof(struct mpwr_dev, _name##_irq), \
	}

struct mpwr_variant {
	int (*power_init)(struct mpwr_dev* mpwr);
	int (*power_up)(struct mpwr_dev* mpwr);
	int (*power_down)(struct mpwr_dev* mpwr);
	int (*reset)(struct mpwr_dev* mpwr);
	void (*recv_msg)(struct mpwr_dev *mpwr, const char *msg);
	int (*suspend)(struct mpwr_dev *mpwr);
	int (*resume)(struct mpwr_dev *mpwr);
	const struct mpwr_gpio* gpios;
	bool regulator_required;
	bool monitor_wakeup;
};

struct mpwr_dev {
	struct device *dev;
	const struct mpwr_variant* variant;

	wait_queue_head_t wait;

	/* serdev */
	struct serdev_device *serdev;
	char rcvbuf[4096];
	size_t rcvbuf_fill;
	char msg[4096];
        int msg_len;
        int msg_ok;
	//struct kfifo kfifo;
	DECLARE_KFIFO(kfifo, unsigned char, 4096);

	/* power */
	struct regulator *regulator;

	/* outputs */
	struct gpio_desc *enable_gpio;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *pwrkey_gpio;
	struct gpio_desc *sleep_gpio;
	struct gpio_desc *dtr_gpio;
	struct gpio_desc *host_ready_gpio;
	struct gpio_desc *cts_gpio;
	struct gpio_desc *rts_gpio;

	/* inputs */
	struct gpio_desc *status_gpio;
	struct gpio_desc *wakeup_gpio;
	int wakeup_irq;
	bool status_pwrkey_multiplexed;

	/* config */
	struct cdev cdev;
	dev_t major;

	/* rfkill */
	struct rfkill *rfkill;

	/* powerup/dn work queue */
	struct workqueue_struct *wq;
	struct work_struct power_work;
	struct work_struct finish_pdn_work;
        struct mutex modem_lock;

	// change
	spinlock_t lock; /* protects last_request */
	int last_request;
	ktime_t last_wakeup;

	struct timer_list wd_timer;
        struct delayed_work host_ready_work;

	unsigned long flags[1];
};

enum {
	/* modem is powered */
	MPWR_F_POWERED,
	MPWR_F_POWER_CHANGE_INPROGRESS,
	MPWR_F_KILLSWITCHED,
	/* we got a wakeup from the modem */
	MPWR_F_GOT_WAKEUP,
        /* serdev */
        MPWR_F_RECEIVING_MSG,
        /* eg25 */
        MPWR_F_GOT_PDN,
	/* config options */
        MPWR_F_DUMB_POWERUP,
        MPWR_F_FASTBOOT_POWERUP,
	/* file */
	MPWR_F_OPEN,
	MPWR_F_OVERFLOW,
};

static struct class* mpwr_class;

static int mpwr_serdev_at_cmd(struct mpwr_dev *mpwr, const char *msg, int timeout_ms);
static int mpwr_serdev_at_cmd_with_retry(struct mpwr_dev *mpwr, const char *msg,
					 int timeout_ms, int tries);
static int mpwr_serdev_at_cmd_with_retry_ignore_timeout(struct mpwr_dev *mpwr, const char *msg,
							int timeout_ms, int tries);

// {{{ mg2723 variant

static int mpwr_mg2723_power_init(struct mpwr_dev* mpwr)
{
	// if the device has power applied or doesn't have regulator
	// configured (we assume it's always powered) initialize GPIO
	// to shut it down initially
	if (!mpwr->regulator || regulator_is_enabled(mpwr->regulator)) {
		gpiod_set_value(mpwr->enable_gpio, 0);
		gpiod_set_value(mpwr->reset_gpio, 1);
	} else {
		// device is not powered, don't drive the gpios
		gpiod_direction_input(mpwr->enable_gpio);
		gpiod_direction_input(mpwr->reset_gpio);
	}

	return 0;
}

static int mpwr_mg2723_power_up(struct mpwr_dev* mpwr)
{
	int ret;

	// power up
	if (mpwr->regulator) {
		ret = regulator_enable(mpwr->regulator);
		if (ret < 0) {
			dev_err(mpwr->dev,
				"can't enable power supply err=%d", ret);
			return ret;
		}
	}

	gpiod_direction_output(mpwr->enable_gpio, 1);
	gpiod_direction_output(mpwr->reset_gpio, 1);
	msleep(300);
	gpiod_set_value(mpwr->reset_gpio, 0);

	return 0;
}

static int mpwr_mg2723_power_down(struct mpwr_dev* mpwr)
{
	gpiod_set_value(mpwr->enable_gpio, 0);
	msleep(50);

	if (mpwr->regulator) {
		regulator_disable(mpwr->regulator);

		gpiod_direction_input(mpwr->enable_gpio);
		gpiod_direction_input(mpwr->reset_gpio);
	} else {
		gpiod_set_value(mpwr->reset_gpio, 1);
	}

	return 0;
}

static int mpwr_mg2723_reset(struct mpwr_dev* mpwr)
{
	gpiod_set_value(mpwr->reset_gpio, 1);
	msleep(300);
	gpiod_set_value(mpwr->reset_gpio, 0);

	return 0;
}

static const struct mpwr_gpio mpwr_mg2723_gpios[] = {
	MPWR_GPIO_DEF(enable, GPIOD_IN, true),
	MPWR_GPIO_DEF(reset, GPIOD_IN, true),
	MPWR_GPIO_DEF_IRQ(wakeup, GPIOD_IN, true, IRQF_TRIGGER_FALLING),
	{ },
};

static const struct mpwr_variant mpwr_mg2723_variant = {
	.power_init = mpwr_mg2723_power_init,
	.power_up = mpwr_mg2723_power_up,
	.power_down = mpwr_mg2723_power_down,
	.reset = mpwr_mg2723_reset,
	.gpios = mpwr_mg2723_gpios,
};

// }}}
// {{{ eg25 variant

static bool mpwr_eg25_qcfg_airplanecontrol_is_ok(const char* v)
{
	return strstarts(v, "1,");
}

struct mpwr_eg25_qcfg {
	const char* name;
	const char* val;
	bool (*is_ok)(const char* val);
};

#define EG25G_LATEST_KNOWN_FIRMWARE "EG25GGBR07A08M2G_01.002.07"

static const struct mpwr_eg25_qcfg mpwr_eg25_qcfgs[] = {
	//{ "risignaltype",       "\"respective\"", },
	{ "risignaltype",       "\"physical\"", },
	{ "urc/ri/ring",        "\"pulse\",1,1000,5000,\"off\",1", },
	{ "urc/ri/smsincoming", "\"pulse\",1,1", },
	{ "urc/ri/other",       "\"off\",1,1", },
	{ "urc/ri/pin",         "uart_ri", },
	{ "urc/delay",          "0", },

	//{ "sleep/datactrl",     "0,300,1", },

	{ "sleepind/level",     "0", },
	{ "wakeupin/level",     "0", },

	{ "ApRstLevel",		"0", },
	{ "ModemRstLevel",	"0", },

	// in EG25-G this tries to modify file in /etc (read-only)
	// and fails
	//{ "dbgctl",		"0", },

	// we don't need AP_READY
	{ "apready",            "0,0,500", },

	{ "airplanecontrol",    "1",   mpwr_eg25_qcfg_airplanecontrol_is_ok },

	// available since firmware R07A08_01.002.01.002
	{ "fast/poweroff", 	"1" },
};

static char* mpwr_serdev_get_response_value(struct mpwr_dev *mpwr,
					    const char* prefix)
{
	int off;

	for (off = 0; off < mpwr->msg_len; off += strlen(mpwr->msg + off) + 1)
		if (strstarts(mpwr->msg + off, prefix))
			return mpwr->msg + off + strlen(prefix);

	return NULL;
}

static struct gpio_desc *mpwr_eg25_get_pwrkey_gpio(struct mpwr_dev *mpwr)
{
	if (mpwr->status_pwrkey_multiplexed)
		return mpwr->status_gpio;

	return mpwr->pwrkey_gpio;
}

/*
 * Gpio meanings
 * -------------
 *
 * enable_gpio - 1 = enables RF, 0 = disables RF
 * sleep_gpio  - 1 = puts modem to sleep, 0 = wakes up the modem (must be 0
 *               during poweron)
 * reset_gpio  - accepts 150-460ms reset pulse (high __|^|__)
 * pwrkey_gpio - accepts 100ms-650ms pulse for powerup (high __|^|__)
 *                       650ms+ pulse for powerdown
 *                       (initiated after pulse ends, pulse may have indefinite
 *                        duration)
 * status_gpio - modem power status 0 = powered  1 = unpowered
 * wakeup_gpio - "ring indicator" output from the modem
 * host_ready_gpio - AP_READY pin - host is ready to receive URCs
 *
 * (pwrkey may be multiplexed with status_gpio)
 *
 * Modem behavior
 * --------------
 *
 * wakeup_gpio (RI):
 * - goes high shortly after power is applied (~15ms)
 * - goes low when RDY is sent
 *
 * dtr_gpio
 * - when high, modem can sleep if requested
 * - H->L will wake up a sleeping modem
 * - internal pull-up
 *
 * ri
 * - pulled low when there's URC
 * - modem wakes up on URC automatically
 *
 * - AT+QURCCFG
 * - AT+QINDCFG="csq",1
 * - AT+QINDCFG="ring",1
 * - AT+QINDCFG="smsincoming",1
 * - AT+CGREG=0
 * - AT+CREG=0
 *
 * - AT+QURCCFG="urcport","uart1"
 */
static int mpwr_eg25_power_up(struct mpwr_dev* mpwr)
{
	struct gpio_desc *pwrkey_gpio = mpwr_eg25_get_pwrkey_gpio(mpwr);
	bool wakeup_ok, status_ok;
	bool needs_restart = false, fastboot;
	u32 speed = 115200;
	int ret, i, off;
	ktime_t start;

	fastboot = test_and_clear_bit(MPWR_F_FASTBOOT_POWERUP, mpwr->flags);

	if (regulator_is_enabled(mpwr->regulator))
		dev_warn(mpwr->dev,
			 "regulator was already enabled during powerup");

	/* Enable the modem power. */
	ret = regulator_enable(mpwr->regulator);
	if (ret < 0) {
		dev_err(mpwr->dev,
			"can't enable power supply err=%d", ret);
		return ret;
	}

	/* Drive default gpio signals during powerup */
	gpiod_direction_output(mpwr->host_ready_gpio, 1);
	/* #W_DISABLE must be left pulled up during modem power up
	 * early on, because opensource bootloader uses this signal to enter
	 * fastboot mode when it's pulled down.
	 *
	 * This should be 1 for normal powerup and 0 for fastboot mode with
	 * special Biktor's firmware.
	 */
	gpiod_direction_output(mpwr->enable_gpio, !fastboot);
	gpiod_direction_output(mpwr->sleep_gpio, 0);
	gpiod_direction_output(mpwr->reset_gpio, 0);
	gpiod_direction_output(pwrkey_gpio, 0);
	gpiod_direction_output(mpwr->dtr_gpio, 0);

	/* Wait for powerup. (30ms min. according to datasheet) */
	msleep(50);

	/* Send 200ms pwrkey pulse to initiate poweron */
	gpiod_set_value(pwrkey_gpio, 1);
	msleep(200);
	gpiod_set_value(pwrkey_gpio, 0);

	/* skip modem killswitch status checks in fastboot bootloader entry mode */
	if (fastboot)
		goto open_serdev;

	/* Switch status key to input, in case it's multiplexed with pwrkey. */
	gpiod_direction_input(mpwr->status_gpio);

	/*
	 * Wait for status/wakeup change, assume good values, if CTS/status
	 * signals, are not configured.
	 */
	status_ok = mpwr->status_gpio ? false : true;
	wakeup_ok = mpwr->wakeup_gpio ? false : true;

	/* wait up to 10s for status */
	start = ktime_get();
	while (ktime_ms_delta(ktime_get(), start) < 10000) {
		if (!wakeup_ok && mpwr->wakeup_gpio && gpiod_get_value(mpwr->wakeup_gpio)) {
			dev_info(mpwr->dev, "wakeup ok\n");
			wakeup_ok = true;
		}

		if (!status_ok && mpwr->status_gpio && !gpiod_get_value(mpwr->status_gpio)) {
			dev_info(mpwr->dev, "status ok\n");
			status_ok = true;
		}

		/* modem is ready */
		if (wakeup_ok && status_ok)
			break;

		msleep(50);
	}

	if (!wakeup_ok) {
		dev_err(mpwr->dev, "The modem looks kill-switched\n");
		if (!test_and_set_bit(MPWR_F_KILLSWITCHED, mpwr->flags))
			sysfs_notify(&mpwr->dev->kobj, NULL, "killswitched");
		goto err_shutdown_noclose;
	}

	if (!status_ok) {
		dev_err(mpwr->dev, "The modem didn't report powerup success in time\n");
		goto err_shutdown_noclose;
	}

	if (test_and_clear_bit(MPWR_F_KILLSWITCHED, mpwr->flags))
		sysfs_notify(&mpwr->dev->kobj, NULL, "killswitched");

open_serdev:
	/* open serial console */
	ret = serdev_device_open(mpwr->serdev);
	if (ret) {
		dev_err(mpwr->dev, "error opening serdev (%d)\n", ret);
		goto err_shutdown_noclose;
	}

	of_property_read_u32(mpwr->dev->of_node, "current-speed", &speed);
	serdev_device_set_baudrate(mpwr->serdev, speed);
	serdev_device_set_flow_control(mpwr->serdev, false);
	ret = serdev_device_set_parity(mpwr->serdev, SERDEV_PARITY_NONE);
	if (ret) {
		dev_err(mpwr->dev, "error setting serdev parity (%d)\n", ret);
		goto err_shutdown;
	}

	if (test_bit(MPWR_F_DUMB_POWERUP, mpwr->flags) || fastboot)
		goto powered_up;

	ret = mpwr_serdev_at_cmd_with_retry_ignore_timeout(mpwr, "AT&FE0", 1000, 30);
	if (ret)
		goto err_shutdown;

	/* print firmware version */
        ret = mpwr_serdev_at_cmd_with_retry(mpwr, "AT+QVERSION;+QSUBSYSVER", 1000, 15);
        if (ret == 0 && mpwr->msg_len > 0) {
		bool outdated = false;

		dev_info(mpwr->dev, "===================================================\n");
		for (off = 0; off < mpwr->msg_len; off += strlen(mpwr->msg + off) + 1) {
			if (strstr(mpwr->msg + off, "Project Rev") && !strstr(mpwr->msg + off, EG25G_LATEST_KNOWN_FIRMWARE))
				outdated = true;

			dev_info(mpwr->dev, "%s\n", mpwr->msg + off);
		}
		dev_info(mpwr->dev, "===================================================\n");

		if (outdated)
			dev_warn(mpwr->dev, "Your modem has an outdated firmware. Latest know version is %s. Consider updating.\n", EG25G_LATEST_KNOWN_FIRMWARE);
	}

	/* print ADB key to dmesg */
        ret = mpwr_serdev_at_cmd_with_retry(mpwr, "AT+QADBKEY?", 1000, 15);
        if (ret == 0) {
		const char *val = mpwr_serdev_get_response_value(mpwr, "+QADBKEY: ");
		if (val)
			dev_info(mpwr->dev, "ADB KEY is '%s' (you can use it to unlock ADB access to the modem, see https://xnux.eu/devices/feature/modem-pp.html)\n", val);
	}

        // check DAI config
        ret = mpwr_serdev_at_cmd_with_retry(mpwr, "AT+QDAI?", 1000, 15);
        if (ret == 0) {
		const char *val = mpwr_serdev_get_response_value(mpwr, "+QDAI: ");
		const char *needed_val = NULL;
		char buf[128];

		if (val) {
			of_property_read_string(mpwr->dev->of_node, "quectel,qdai", &needed_val);

			if (needed_val && strcmp(needed_val, val)) {
				dev_warn(mpwr->dev, "QDAI is '%s' (changing to '%s')\n", val, needed_val);

				/* update qdai */
				snprintf(buf, sizeof buf, "AT+QDAI=%s", needed_val);
				ret = mpwr_serdev_at_cmd(mpwr, buf, 5000);
				if (ret == 0)
					needs_restart = true;
			} else {
				dev_info(mpwr->dev, "QDAI is '%s'\n", val);
			}
		}
	}

	/* reset the modem, to apply QDAI config if necessary */
	if (needs_restart) {
		dev_info(mpwr->dev, "Restarting modem\n");
        
		/* reboot is broken with fastboot enabled */
		mpwr_serdev_at_cmd(mpwr, "AT+QCFG=\"fast/poweroff\",0", 5000);

		ret = mpwr_serdev_at_cmd(mpwr, "AT+CFUN=1,1", 5000);
		if (ret)
			goto err_shutdown;

		/* wait a bit before starting to probe the modem again */
		msleep(6000);

		ret = mpwr_serdev_at_cmd_with_retry_ignore_timeout(mpwr, "AT&FE0", 1000, 30);
		if (ret)
			goto err_shutdown;

		// wait until QDAI starts succeeding (then the modem is ready
		// to accept the following QCFGs)
		ret = mpwr_serdev_at_cmd_with_retry(mpwr, "AT+QDAI?", 1000, 15);
		if (ret)
			goto err_shutdown;
	}

        /* check and update important QCFGs */
        for (i = 0; i < ARRAY_SIZE(mpwr_eg25_qcfgs); i++) {
                const char* name = mpwr_eg25_qcfgs[i].name;
                const char* needed_val = mpwr_eg25_qcfgs[i].val;
		bool (*is_ok)(const char* val) = mpwr_eg25_qcfgs[i].is_ok;
		const char *val;
                char buf[128];

                snprintf(buf, sizeof buf, "AT+QCFG=\"%s\"", name);
                ret = mpwr_serdev_at_cmd(mpwr, buf, 1000);
                if (ret)
			continue;

                snprintf(buf, sizeof buf, "+QCFG: \"%s\",", name);
		val = mpwr_serdev_get_response_value(mpwr, buf);
		if (val) {
			if (needed_val && (is_ok ? !is_ok(val) : strcmp(needed_val, val))) {
				dev_info(mpwr->dev, "QCFG '%s' is '%s' (changing to '%s')\n", name, val, needed_val);

				/* update qcfg */
				snprintf(buf, sizeof buf, "AT+QCFG=\"%s\",%s", name, needed_val);
				ret = mpwr_serdev_at_cmd(mpwr, buf, 1000);
				if (ret)
					break; /* go to next QCFG */
			} else {
				dev_info(mpwr->dev, "QCFG '%s' is '%s'\n", name, val);
			}
		}
        }

	/* setup URC port */
	ret = mpwr_serdev_at_cmd(mpwr, "AT+QURCCFG=\"urcport\",\"all\"", 2000);
        if (ret) {
		dev_info(mpwr->dev, "Your modem doesn't support AT+QURCCFG=\"urcport\",\"all\", consider upgrading the firmware.\n");

		ret = mpwr_serdev_at_cmd(mpwr, "AT+QURCCFG=\"urcport\",\"usbat\"", 2000);
		if (ret)
			dev_err(mpwr->dev, "Modem may not report URCs to the right port!\n");
	}

	/* enable the modem to go to sleep when DTR is low */
	ret = mpwr_serdev_at_cmd(mpwr, "AT+QSCLK=1", 2000);
        if (ret)
		dev_err(mpwr->dev, "Modem will probably not sleep!\n");

powered_up:
	gpiod_direction_output(mpwr->dtr_gpio, 1);

	return 0;

err_shutdown:
	serdev_device_close(mpwr->serdev);
err_shutdown_noclose:
	dev_warn(mpwr->dev,
		 "Forcibly cutting off power, data loss may occur.\n");
	gpiod_direction_input(mpwr->enable_gpio);
	gpiod_direction_input(mpwr->reset_gpio);
	gpiod_direction_input(mpwr->sleep_gpio);
	gpiod_direction_input(pwrkey_gpio);
	gpiod_direction_input(mpwr->host_ready_gpio);
	gpiod_direction_input(mpwr->dtr_gpio);

	regulator_disable(mpwr->regulator);
	return -ENODEV;
}

static int mpwr_eg25_power_down_finish(struct mpwr_dev* mpwr)
{
	struct gpio_desc *pwrkey_gpio = mpwr_eg25_get_pwrkey_gpio(mpwr);
	ktime_t start = ktime_get();
	int ret;

	serdev_device_close(mpwr->serdev);

	/*
	 * This function is called right after POWERED DOWN message is received.
	 *
	 * In case of fast/poweroff == 1, no POWERED DOWN message is sent.
	 * Fast power off times are around 1s since the end of 800ms
	 * POK pulse.
	 *
	 * When the modem powers down RI (wakeup) goes low and STATUS goes
	 * high at the same time. Status is not connected on some boards.
	 * RI should be inactive during poweroff, but we don't know for sure.
	 *
	 * Therfore:
	 * - wait for STATUS going low
	 * - in case that's not available wait for RI going low
	 * - in case timings seem off, warn the user
	 *
	 * In addition, some boards have PWRKEY multiplexed with STATUS signal.
	 * In that case we need to switch STATUS to output high level, as soon
	 * as it goes low in order to prevent a power-up signal being registered
	 * by the modem.
	 */

	if (mpwr->status_gpio) {
		/* wait up to 30s for status going high */
		while (ktime_ms_delta(ktime_get(), start) < 30000) {
			if (gpiod_get_value(mpwr->status_gpio)) {
				if (ktime_ms_delta(ktime_get(), start) < 500)
					dev_warn(mpwr->dev,
						 "STATUS signal is high too soon during powerdown. Modem is already off?\n");
				goto powerdown;
			}

			msleep(20);
		}

		dev_warn(mpwr->dev,
			 "STATUS signal didn't go high during shutdown. Modem is still on?\n");
		goto force_powerdown;
	} else {
		clear_bit(MPWR_F_GOT_WAKEUP, mpwr->flags);

		if (!gpiod_get_value(mpwr->wakeup_gpio)) {
			dev_warn(mpwr->dev,
				 "RI signal is low too soon during powerdown. Modem is already off, or spurious wakeup?\n");
			msleep(2000);
			goto powerdown;
		}

		ret = wait_event_timeout(mpwr->wait,
					 test_bit(MPWR_F_GOT_WAKEUP, mpwr->flags),
					 msecs_to_jiffies(30000));
		if (ret <= 0) {
			dev_warn(mpwr->dev,
				 "RI signal didn't go low during shutdown, is modem really powering down?\n");
			goto force_powerdown;
		}

		if (ktime_ms_delta(ktime_get(), start) < 500) {
			dev_warn(mpwr->dev,
				 "RI signal is low too soon during powerdown. Modem is already off, or spurious wakeup?\n");
			msleep(2000);
			goto powerdown;
		}
	}

powerdown:
	gpiod_direction_input(mpwr->enable_gpio);
	gpiod_direction_input(mpwr->reset_gpio);
	gpiod_direction_input(mpwr->sleep_gpio);
	gpiod_direction_input(pwrkey_gpio);
	gpiod_direction_input(mpwr->host_ready_gpio);
	gpiod_direction_input(mpwr->dtr_gpio);

	regulator_disable(mpwr->regulator);

	return 0;

force_powerdown:
	dev_warn(mpwr->dev,
		 "Forcibly cutting off power, data loss may occur.\n");
	goto powerdown;
}

static int mpwr_eg25_power_down(struct mpwr_dev* mpwr)
{
	struct gpio_desc *pwrkey_gpio = mpwr_eg25_get_pwrkey_gpio(mpwr);
	int ret;

	/* Send 800ms pwrkey pulse to initiate powerdown. */
	gpiod_direction_output(pwrkey_gpio, 1);
	msleep(800);
	gpiod_set_value(pwrkey_gpio, 0);

	/* Switch status key to input, in case it's multiplexed with pwrkey. */
	gpiod_direction_input(mpwr->status_gpio);

	msleep(20);

#if 0
	// wait for POWERED DOWN message
	clear_bit(MPWR_F_GOT_PDN, mpwr->flags);
	ret = wait_event_timeout(mpwr->wait,
				 test_bit(MPWR_F_GOT_PDN, mpwr->flags),
				 msecs_to_jiffies(7000));
	if (ret <= 0)
		dev_warn(mpwr->dev,
			 "POWERED DOWN message not received, is modem really powering down?\n");
#endif

	return mpwr_eg25_power_down_finish(mpwr);
}

static void mpwr_finish_pdn_work(struct work_struct *work)
{
	/*
	struct mpwr_dev *mpwr = container_of(work, struct mpwr_dev, power_work);
	unsigned long flags;

	spin_lock_irqsave(&mpwr->lock, flags);
	spin_unlock_irqrestore(&mpwr->lock, flags);

	pm_stay_awake(mpwr->dev);

	mutex_lock(&mpwr->modem_lock);

	mutex_unlock(&mpwr->modem_lock);

	pm_relax(mpwr->dev);
	*/
}

static void mpwr_eg25_receive_msg(struct mpwr_dev *mpwr, const char *msg)
{
	unsigned int msg_len;

	if (!strcmp(msg, "POWERED DOWN")) {
		// system is powering down
                set_bit(MPWR_F_GOT_PDN, mpwr->flags);
		wake_up(&mpwr->wait);

		/*
		if (mutex_trylock(&mpwr->modem_lock)) {
			// if no power op is in progress, this means userspace
			// tried to shut the modem down via AT command, finish up
			// the job

			pm_stay_awake(mpwr->dev);

			queue_work(mpwr->wq, &mpwr->power_work);
			dev_warn(mpwr->dev, "userspace shut down the modem via AT command, finishing the job\n");
			mpwr_eg25_power_down_finish(mpwr);
			mutex_unlock(&mpwr->modem_lock);

			pm_relax(mpwr->dev);
		}
                  */
                return;
	}

	if (!strcmp(msg, "RDY")) {
		// system is ready after powerup
                return;
	}

	if (!test_bit(MPWR_F_OPEN, mpwr->flags))
		return;

	msg_len = strlen(msg);

	if (msg_len + 1 > kfifo_avail(&mpwr->kfifo)) {
		if (!test_and_set_bit(MPWR_F_OVERFLOW, mpwr->flags))
			wake_up(&mpwr->wait);
		return;
	}

	kfifo_in(&mpwr->kfifo, msg, msg_len);
	kfifo_in(&mpwr->kfifo, "\n", 1);
	wake_up(&mpwr->wait);
}

static void mpwr_host_ready_work(struct work_struct *work)
{
        struct mpwr_dev *mpwr = container_of(work, struct mpwr_dev, host_ready_work.work);
	int ret;

	mutex_lock(&mpwr->modem_lock);
	gpiod_direction_output(mpwr->dtr_gpio, 0);

	/*
	 * We need to give the modem some time to wake up.
	 */
	msleep(5);

	ret = mpwr_serdev_at_cmd(mpwr, "AT+QCFG=\"urc/cache\",0", 500);
	if (ret)
		dev_warn(mpwr->dev,
			 "Failed to disable urc/cache, you may not be able to see URCs\n");

	gpiod_direction_output(mpwr->dtr_gpio, 1);
	mutex_unlock(&mpwr->modem_lock);

	gpiod_direction_output(mpwr->host_ready_gpio, 1);
}

static int mpwr_eg25_suspend(struct mpwr_dev *mpwr)
{
	int ret;

	cancel_delayed_work_sync(&mpwr->host_ready_work);

	gpiod_direction_output(mpwr->host_ready_gpio, 0);

	mutex_lock(&mpwr->modem_lock);
	gpiod_direction_output(mpwr->dtr_gpio, 0);

	msleep(5);

	ret = mpwr_serdev_at_cmd(mpwr, "AT+QCFG=\"urc/cache\",1", 500);
	if (ret)
		dev_warn(mpwr->dev,
			 "Failed to enable urc/cache, you may lose URCs during suspend\n");

	gpiod_direction_output(mpwr->dtr_gpio, 1);
	mutex_unlock(&mpwr->modem_lock);

	return 0;
}

static int mpwr_eg25_resume(struct mpwr_dev *mpwr)
{
	//gpiod_direction_output(mpwr->dtr_gpio, 0);

	// delay disabling URC cache until the whole system is hopefully resumed...
	schedule_delayed_work(&mpwr->host_ready_work, msecs_to_jiffies(1000));

	return 0;
}

static const struct mpwr_gpio mpwr_eg25_gpios[] = {
	MPWR_GPIO_DEF(enable, GPIOD_OUT_HIGH, true),
	MPWR_GPIO_DEF(reset, GPIOD_OUT_LOW, true),
	MPWR_GPIO_DEF(pwrkey, GPIOD_OUT_LOW, false),
	MPWR_GPIO_DEF(dtr, GPIOD_OUT_LOW, true),
	MPWR_GPIO_DEF(status, GPIOD_IN, false),
	MPWR_GPIO_DEF_IRQ(wakeup, GPIOD_IN, true,
			  IRQF_TRIGGER_FALLING),

	// XXX: not really needed...
	MPWR_GPIO_DEF(sleep, GPIOD_OUT_LOW, false),
	MPWR_GPIO_DEF(host_ready, GPIOD_OUT_HIGH, false),
	MPWR_GPIO_DEF(cts, GPIOD_IN, false),
	MPWR_GPIO_DEF(rts, GPIOD_OUT_LOW, false),
	{ },
};

static const struct mpwr_variant mpwr_eg25_variant = {
	.power_up = mpwr_eg25_power_up,
	.power_down = mpwr_eg25_power_down,
	.recv_msg = mpwr_eg25_receive_msg,
	.suspend = mpwr_eg25_suspend,
	.resume = mpwr_eg25_resume,
	.gpios = mpwr_eg25_gpios,
	.regulator_required = true,
	.monitor_wakeup = true,
};

// }}}
// {{{ generic helpers

static void mpwr_reset(struct mpwr_dev* mpwr)
{
	struct device *dev = mpwr->dev;
	int ret;

	if (!test_bit(MPWR_F_POWERED, mpwr->flags)) {
		dev_err(dev, "reset requested but device is not enabled");
		return;
	}

	if (!mpwr->reset_gpio) {
		dev_err(dev, "reset is not configured for this device");
		return;
	}

	if (!mpwr->variant->reset) {
		dev_err(dev, "reset requested but not implemented");
		return;
	}

	dev_info(dev, "resetting");
	ret = mpwr->variant->reset(mpwr);
	if (ret) {
		dev_err(dev, "reset failed");
	}
}

static void mpwr_power_down(struct mpwr_dev* mpwr)
{
	struct device *dev = mpwr->dev;
	ktime_t start = ktime_get();
	int ret;

	if (!test_bit(MPWR_F_POWERED, mpwr->flags))
		return;

	if (!mpwr->variant->power_down) {
		dev_err(dev, "power down requested but not implemented");
		return;
	}

	dev_info(dev, "powering down");

	ret = mpwr->variant->power_down(mpwr);
	if (ret) {
		dev_err(dev, "power down failed");
	} else {
		clear_bit(MPWR_F_POWERED, mpwr->flags);
		sysfs_notify(&mpwr->dev->kobj, NULL, "powered");
		dev_info(mpwr->dev, "powered down in %lld ms\n",
			 ktime_ms_delta(ktime_get(), start));
	}
}

static void mpwr_power_up(struct mpwr_dev* mpwr)
{
	struct device *dev = mpwr->dev;
	ktime_t start = ktime_get();
	int ret;

	if (test_bit(MPWR_F_POWERED, mpwr->flags))
		return;

	if (!mpwr->variant->power_up) {
		dev_err(dev, "power up requested but not implemented");
		return;
	}

	dev_info(dev, "powering up");

	ret = mpwr->variant->power_up(mpwr);
	if (ret) {
		dev_err(dev, "power up failed");
	} else {
		set_bit(MPWR_F_POWERED, mpwr->flags);
		sysfs_notify(&mpwr->dev->kobj, NULL, "powered");
		dev_info(mpwr->dev, "powered up in %lld ms\n",
			 ktime_ms_delta(ktime_get(), start));
	}
}

// }}}
// {{{ chardev

static int mpwr_release(struct inode *ip, struct file *fp)
{
	struct mpwr_dev* mpwr = fp->private_data;

	clear_bit(MPWR_F_OPEN, mpwr->flags);

	return 0;
}

static int mpwr_open(struct inode *ip, struct file *fp)
{
	struct mpwr_dev* mpwr = container_of(ip->i_cdev, struct mpwr_dev, cdev);

	fp->private_data = mpwr;

	if (test_and_set_bit(MPWR_F_OPEN, mpwr->flags))
		return -EBUSY;

	nonseekable_open(ip, fp);
	return 0;
}

static ssize_t mpwr_read(struct file *fp, char __user *buf, size_t len,
			 loff_t *off)
{
	struct mpwr_dev* mpwr = fp->private_data;
	int non_blocking = fp->f_flags & O_NONBLOCK;
	unsigned int copied;
	int ret;

	if (non_blocking && kfifo_is_empty(&mpwr->kfifo))
		return -EWOULDBLOCK;

	ret = wait_event_interruptible(mpwr->wait,
				       !kfifo_is_empty(&mpwr->kfifo)
				       || test_bit(MPWR_F_OVERFLOW, mpwr->flags));
	if (ret)
		return ret;

	if (test_and_clear_bit(MPWR_F_OVERFLOW, mpwr->flags)) {
		if (len < 9)
			return -E2BIG;
		if (copy_to_user(buf, "OVERFLOW\n", 9))
			return -EFAULT;
		return 9;
	}

	ret = kfifo_to_user(&mpwr->kfifo, buf, len, &copied);

	return ret ? ret : copied;
}

static unsigned int mpwr_poll(struct file *fp, poll_table *wait)
{
	struct mpwr_dev* mpwr = fp->private_data;

	poll_wait(fp, &mpwr->wait, wait);

	if (!kfifo_is_empty(&mpwr->kfifo))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations mpwr_fops = {
	.owner		= THIS_MODULE,
	.open		= mpwr_open,
	.release	= mpwr_release,
	.llseek		= noop_llseek,
	.read		= mpwr_read,
	.poll		= mpwr_poll,
};

// }}}

static void mpwr_work_handler(struct work_struct *work)
{
	struct mpwr_dev *mpwr = container_of(work, struct mpwr_dev, power_work);
	unsigned long flags;
	int last_request;

	spin_lock_irqsave(&mpwr->lock, flags);
	last_request = mpwr->last_request;
	mpwr->last_request = 0;
	spin_unlock_irqrestore(&mpwr->lock, flags);

	pm_stay_awake(mpwr->dev);

	mutex_lock(&mpwr->modem_lock);

	if (last_request == MPWR_REQ_RESET) {
		mpwr_reset(mpwr);
	} else if (last_request == MPWR_REQ_PWDN) {
		mpwr_power_down(mpwr);
	} else if (last_request == MPWR_REQ_PWUP) {
		mpwr_power_up(mpwr);
	}

	mutex_unlock(&mpwr->modem_lock);

	clear_bit(MPWR_F_POWER_CHANGE_INPROGRESS, mpwr->flags);
	sysfs_notify(&mpwr->dev->kobj, NULL, "is_busy");
	wake_up(&mpwr->wait);

	pm_relax(mpwr->dev);
}

static void mpwr_request_power_change(struct mpwr_dev* mpwr, int request)
{
	unsigned long flags;

	set_bit(MPWR_F_POWER_CHANGE_INPROGRESS, mpwr->flags);
	sysfs_notify(&mpwr->dev->kobj, NULL, "is_busy");

	spin_lock_irqsave(&mpwr->lock, flags);
	mpwr->last_request = request;
	spin_unlock_irqrestore(&mpwr->lock, flags);

	queue_work(mpwr->wq, &mpwr->power_work);
}

static irqreturn_t mpwr_gpio_isr(int irq, void *dev_id)
{
	struct mpwr_dev *mpwr = dev_id;

	if (irq == mpwr->wakeup_irq) {
		dev_dbg(mpwr->dev, "wakeup irq\n");

		set_bit(MPWR_F_GOT_WAKEUP, mpwr->flags);
		spin_lock(&mpwr->lock);
		mpwr->last_wakeup = ktime_get();
		spin_unlock(&mpwr->lock);
		wake_up(&mpwr->wait);
	}

	return IRQ_HANDLED;
}

static void mpwr_wd_timer_fn(struct timer_list *t)
{
	struct mpwr_dev *mpwr = from_timer(mpwr, t, wd_timer);

	if (!mpwr->variant->monitor_wakeup || !test_bit(MPWR_F_POWERED, mpwr->flags))
		return;

	/*
	 * Monitor wakeup status:
	 *
	 * If RI signal is low for too long we assume the user killswitched
	 * the modem at runtime.
	 */
	spin_lock(&mpwr->lock);
	if (!gpiod_get_value(mpwr->wakeup_gpio)) {
		if (ktime_ms_delta(ktime_get(), mpwr->last_wakeup) > 5000) {
			if (!test_and_set_bit(MPWR_F_KILLSWITCHED, mpwr->flags))
				sysfs_notify(&mpwr->dev->kobj, NULL, "killswitched");
			wake_up(&mpwr->wait);
			dev_warn(mpwr->dev, "modem looks killswitched at runtime!\n");
		}
	}
	spin_unlock(&mpwr->lock);

	mod_timer(t, jiffies + msecs_to_jiffies(1000));
}

// {{{ sysfs

static ssize_t powered_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!test_bit(MPWR_F_POWERED, mpwr->flags));
}

static ssize_t powered_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t len)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));
	bool status;
	int ret;

	ret = kstrtobool(buf, &status);
	if (ret)
		return ret;

	mpwr_request_power_change(mpwr, status ? MPWR_REQ_PWUP : MPWR_REQ_PWDN);

	return len;
}

static ssize_t powered_blocking_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));
	bool status;
	int ret;

	ret = kstrtobool(buf, &status);
	if (ret)
		return ret;

	mpwr_request_power_change(mpwr, status ? MPWR_REQ_PWUP : MPWR_REQ_PWDN);

	ret = wait_event_interruptible_timeout(mpwr->wait,
					       !test_bit(MPWR_F_POWER_CHANGE_INPROGRESS, mpwr->flags),
					       msecs_to_jiffies(60000));
	if (ret <= 0) {
		dev_err(mpwr->dev, "Power state change timeout\n");
		return -EIO;
	}

	if (!!status != !!test_bit(MPWR_F_POWERED, mpwr->flags))
		return -EIO;

	return len;
}

static ssize_t dumb_powerup_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!test_bit(MPWR_F_DUMB_POWERUP, mpwr->flags));
}

static ssize_t dumb_powerup_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	if (val) {
		dev_err(mpwr->dev, "Don't use dumb_powerup, it's just a debug function!\n");
		set_bit(MPWR_F_DUMB_POWERUP, mpwr->flags);
	} else
		clear_bit(MPWR_F_DUMB_POWERUP, mpwr->flags);

	return len;
}

static ssize_t fastboot_powerup_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!test_bit(MPWR_F_FASTBOOT_POWERUP, mpwr->flags));
}

static ssize_t fastboot_powerup_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t len)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	if (val) {
		dev_warn(mpwr->dev, "Fastboot powerup needs a special bootloader!\n");
		set_bit(MPWR_F_FASTBOOT_POWERUP, mpwr->flags);
	} else
		clear_bit(MPWR_F_FASTBOOT_POWERUP, mpwr->flags);

	return len;
}

static ssize_t killswitched_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!test_bit(MPWR_F_KILLSWITCHED, mpwr->flags));
}

static ssize_t is_busy_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));

	return scnprintf(buf, PAGE_SIZE, "%u\n",
			 !!test_bit(MPWR_F_POWER_CHANGE_INPROGRESS, mpwr->flags));
}

static ssize_t hard_reset_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(to_platform_device(dev));
	bool val;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;
	if (val)
		mpwr_request_power_change(mpwr, MPWR_REQ_RESET);

	return len;
}

static DEVICE_ATTR_RW(powered);
static DEVICE_ATTR_WO(powered_blocking);
static DEVICE_ATTR_RW(dumb_powerup);
static DEVICE_ATTR_RW(fastboot_powerup);
static DEVICE_ATTR_RO(killswitched);
static DEVICE_ATTR_RO(is_busy);
static DEVICE_ATTR_WO(hard_reset);

static struct attribute *mpwr_attrs[] = {
	&dev_attr_powered.attr,
	&dev_attr_powered_blocking.attr,
	&dev_attr_dumb_powerup.attr,
	&dev_attr_fastboot_powerup.attr,
	&dev_attr_killswitched.attr,
	&dev_attr_is_busy.attr,
	&dev_attr_hard_reset.attr,
	NULL,
};

static const struct attribute_group mpwr_group = {
	.attrs = mpwr_attrs,
};

// }}}
// {{{ rfkill

static int mpwr_rfkill_set(void *data, bool blocked)
{
	struct mpwr_dev *mpwr = data;

	gpiod_set_value(mpwr->enable_gpio, !blocked);
	return 0;
}

static void mpwr_rfkill_query(struct rfkill *rfkill, void *data)
{
	struct mpwr_dev *mpwr = data;

	rfkill_set_sw_state(rfkill, !gpiod_get_value(mpwr->enable_gpio));
}

static const struct rfkill_ops mpwr_rfkill_ops = {
	.set_block = mpwr_rfkill_set,
	.query = mpwr_rfkill_query,
};

// }}}
// {{{ probe

static int mpwr_probe_generic(struct device *dev, struct mpwr_dev **mpwr_out)
{
	struct mpwr_dev *mpwr;
	struct device_node *np = dev->of_node;
	struct device *sdev;
	const char* cdev_name = NULL;
	int ret, i;

	mpwr = devm_kzalloc(dev, sizeof(*mpwr), GFP_KERNEL);
	if (!mpwr)
		return -ENOMEM;

	mpwr->variant = of_device_get_match_data(dev);
	if (!mpwr->variant)
		return -EINVAL;

	mpwr->dev = dev;
	init_waitqueue_head(&mpwr->wait);
        mutex_init(&mpwr->modem_lock);
        spin_lock_init(&mpwr->lock);
	INIT_WORK(&mpwr->power_work, &mpwr_work_handler);
	INIT_WORK(&mpwr->finish_pdn_work, &mpwr_finish_pdn_work);
        INIT_DELAYED_WORK(&mpwr->host_ready_work, mpwr_host_ready_work);
	INIT_KFIFO(mpwr->kfifo);

	ret = of_property_read_string(np, "char-device-name", &cdev_name);
	if (ret) {
		dev_err(dev, "char-device-name is not configured");
		return -EINVAL;
	}

	mpwr->status_pwrkey_multiplexed =
		of_property_read_bool(np, "status-pwrkey-multiplexed");

	mpwr->regulator = devm_regulator_get_optional(dev, "power");
	if (IS_ERR(mpwr->regulator)) {
		ret = PTR_ERR(mpwr->regulator);
                if (ret != -ENODEV) {
			dev_err(dev, "can't get power supply err=%d", ret);
			return ret;
		}

		mpwr->regulator = NULL;
	}

	if (!mpwr->regulator && mpwr->variant->regulator_required) {
		dev_err(dev, "can't get power supply err=%d", -ENODEV);
		return -ENODEV;
	}

	for (i = 0; mpwr->variant->gpios[i].name; i++) {
		const struct mpwr_gpio *io = &mpwr->variant->gpios[i];
		struct gpio_desc **desc = (struct gpio_desc **)((u8*)mpwr +
								io->desc_off);
		int *irq = (int*)((u8*)mpwr + io->irq_off);
		char buf[64];

		if (io->required)
			*desc = devm_gpiod_get(dev, io->name, io->flags);
		else
			*desc = devm_gpiod_get_optional(dev, io->name, io->flags);

		if (IS_ERR(*desc)) {
			dev_err(dev, "can't get %s gpio err=%ld", io->name,
				PTR_ERR(*desc));
			return PTR_ERR(*desc);
		}

		if (!*desc)
			continue;

		if (io->irq_flags == 0 || io->irq_off == 0)
			continue;

		*irq = gpiod_to_irq(*desc);
		if (*irq <= 0) {
			dev_err(dev, "error converting %s gpio to irq: %d",
				io->name, ret);
			return *irq;
		}

		snprintf(buf, sizeof buf, "modem-%s-gpio", io->name);
		ret = devm_request_irq(dev, *irq, mpwr_gpio_isr, io->irq_flags,
				       devm_kstrdup(dev, buf, GFP_KERNEL), mpwr);
		if (ret) {
			dev_err(dev, "error requesting %s irq: %d",
				io->name, ret);
			return ret;
		}
	}

	if (mpwr->status_pwrkey_multiplexed && mpwr->pwrkey_gpio) {
		dev_err(dev, "status and pwrkey are multiplexed, but pwrkey defined\n");
		return -EINVAL;
	}

	if (mpwr->status_pwrkey_multiplexed && !mpwr->status_gpio) {
		dev_err(dev, "status and pwrkey are multiplexed, but status is not defined\n");
		return -EINVAL;
	}

	ret = devm_device_add_group(dev, &mpwr_group);
	if (ret)
		return ret;

	// create char device
	ret = alloc_chrdev_region(&mpwr->major, 0, 1, "modem-power");
	if (ret) {
		dev_err(dev, "can't allocate chrdev region");
		goto err_disable_regulator;
	}

	cdev_init(&mpwr->cdev, &mpwr_fops);
	mpwr->cdev.owner = THIS_MODULE;
	ret = cdev_add(&mpwr->cdev, mpwr->major, 1);
	if (ret) {
		dev_err(dev, "can't add cdev");
		goto err_unreg_chrev_region;
	}

	sdev = device_create(mpwr_class, dev, mpwr->major, mpwr, cdev_name);
	if (IS_ERR(sdev)) {
		ret = PTR_ERR(sdev);
		goto err_del_cdev;
	}

	if (mpwr->wakeup_irq > 0) {
		ret = device_init_wakeup(dev, true);
		if (ret) {
			dev_err(dev, "failed to init wakeup (%d)\n", ret);
			goto err_free_dev;
		}
	}

	if (mpwr->enable_gpio) {
		mpwr->rfkill = rfkill_alloc("modem", dev, RFKILL_TYPE_WWAN,
					    &mpwr_rfkill_ops, mpwr);
		if (!mpwr->rfkill) {
			dev_err(dev, "failed to alloc rfkill\n");
			ret = -ENOMEM;
			goto err_deinit_wakeup;
		}

                rfkill_init_sw_state(mpwr->rfkill, false);

		ret = rfkill_register(mpwr->rfkill);
		if (ret) {
			dev_err(dev, "failed to register rfkill (%d)\n", ret);
			goto err_free_rfkill;
		}
	}

	mpwr->wq = alloc_ordered_workqueue("modem-power", 0);
	if (!mpwr->wq) {
		ret = -ENOMEM;
		dev_err(dev, "failed to allocate workqueue\n");
		goto err_unreg_rfkill;
	}

	if (mpwr->variant->power_init)
		mpwr->variant->power_init(mpwr);

	timer_setup(&mpwr->wd_timer, mpwr_wd_timer_fn, 0);
	mod_timer(&mpwr->wd_timer, jiffies + msecs_to_jiffies(50));

	dev_info(dev, "modem power manager ready");
	*mpwr_out = mpwr;

	return 0;

err_unreg_rfkill:
	if (mpwr->rfkill)
		rfkill_unregister(mpwr->rfkill);
err_free_rfkill:
	if (mpwr->rfkill)
		rfkill_destroy(mpwr->rfkill);
err_deinit_wakeup:
	if (mpwr->wakeup_irq > 0)
		device_init_wakeup(dev, false);
err_free_dev:
	device_destroy(mpwr_class, mpwr->major);
err_del_cdev:
	cdev_del(&mpwr->cdev);
err_unreg_chrev_region:
	unregister_chrdev(mpwr->major, "modem-power");
err_disable_regulator:
	cancel_work_sync(&mpwr->power_work);
	return ret;
}

static int mpwr_remove_generic(struct mpwr_dev *mpwr)
{
	if (mpwr->rfkill) {
		rfkill_unregister(mpwr->rfkill);
		rfkill_destroy(mpwr->rfkill);
	}

	if (mpwr->wakeup_irq > 0)
		device_init_wakeup(mpwr->dev, false);

	del_timer_sync(&mpwr->wd_timer);
        cancel_delayed_work_sync(&mpwr->host_ready_work);

	cancel_work_sync(&mpwr->power_work);
	destroy_workqueue(mpwr->wq);

	mutex_lock(&mpwr->modem_lock);
	mpwr_power_down(mpwr);
	mutex_unlock(&mpwr->modem_lock);

	device_destroy(mpwr_class, mpwr->major);
	cdev_del(&mpwr->cdev);
	unregister_chrdev(mpwr->major, "modem-power");

	return 0;
}

static void mpwr_shutdown_generic(struct mpwr_dev *mpwr)
{
	cancel_work_sync(&mpwr->power_work);
        cancel_delayed_work_sync(&mpwr->host_ready_work);

	mutex_lock(&mpwr->modem_lock);
	mpwr_power_down(mpwr);
	mutex_unlock(&mpwr->modem_lock);
}

// }}}
// {{{ suspend/resume

static int __maybe_unused mpwr_suspend(struct device *dev)
{
	struct mpwr_dev *mpwr = dev_get_drvdata(dev);
	int ret = 0;

	if (!test_bit(MPWR_F_POWERED, mpwr->flags))
		return 0;

	//if (mpwr->sleep_gpio)
		//gpiod_direction_output(mpwr->sleep_gpio, 1);

	if (mpwr->variant->suspend)
		mpwr->variant->suspend(mpwr);

	if (mpwr->wakeup_irq && device_may_wakeup(mpwr->dev))
		enable_irq_wake(mpwr->wakeup_irq);

	return ret;
}

static int __maybe_unused mpwr_resume(struct device *dev)
{
	struct mpwr_dev *mpwr = dev_get_drvdata(dev);
	int ret = 0;

	if (!test_bit(MPWR_F_POWERED, mpwr->flags))
		return 0;

	//if (mpwr->sleep_gpio)
		//gpiod_direction_output(mpwr->sleep_gpio, 0);

	if (mpwr->variant->resume)
		mpwr->variant->resume(mpwr);

	if (mpwr->wakeup_irq && device_may_wakeup(mpwr->dev))
		disable_irq_wake(mpwr->wakeup_irq);

	return ret;
}

static const struct dev_pm_ops mpwr_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mpwr_suspend, mpwr_resume)
};

// }}}
// {{{ serdev

static int mpwr_serdev_send_msg(struct mpwr_dev *mpwr, const char *msg)
{
	int ret, len;
	char buf[128];

	if (!mpwr->serdev)
		return -ENODEV;

	len = snprintf(buf, sizeof buf, "%s\r\n", msg);
	if (len >= sizeof buf)
		return -E2BIG;

	ret = serdev_device_write(mpwr->serdev, buf, len, msecs_to_jiffies(3000));
	if (ret < len)
		return -EIO;

	serdev_device_wait_until_sent(mpwr->serdev, msecs_to_jiffies(3000));

	return 0;
}

static int __mpwr_serdev_at_cmd(struct mpwr_dev *mpwr, const char *msg,
				int timeout_ms, bool report_error, bool report_timeout)
{
        int ret;

        if (test_and_set_bit(MPWR_F_RECEIVING_MSG, mpwr->flags))
		return -EBUSY;

	mpwr->msg_len = 0;

	dev_dbg(mpwr->dev, "SEND: %s\n", msg);

	ret = mpwr_serdev_send_msg(mpwr, msg);
        if (ret) {
		clear_bit(MPWR_F_RECEIVING_MSG, mpwr->flags);
		dev_err(mpwr->dev, "AT command '%s' can't be sent (%d)\n", msg, ret);
                return ret;
        }

	ret = wait_event_interruptible_timeout(mpwr->wait,
					       !test_bit(MPWR_F_RECEIVING_MSG, mpwr->flags),
					       msecs_to_jiffies(timeout_ms));
	if (ret <= 0) {
		clear_bit(MPWR_F_RECEIVING_MSG, mpwr->flags);
		if (report_timeout)
			dev_err(mpwr->dev, "AT command '%s' timed out\n", msg);
                return ret ? ret : -ETIMEDOUT;
	}

        if (!mpwr->msg_ok) {
		if (report_error)
			dev_err(mpwr->dev, "AT command '%s' returned ERROR\n", msg);
                return -EINVAL;
	}

        return 0;
}

static int mpwr_serdev_at_cmd(struct mpwr_dev *mpwr, const char *msg, int timeout_ms)
{
	return __mpwr_serdev_at_cmd(mpwr, msg, timeout_ms, true, true);
}

static int __mpwr_serdev_at_cmd_with_retry(struct mpwr_dev *mpwr, const char *msg,
					   int timeout_ms, int tries, bool ignore_timeout)
{
	int ret = 0;

	if (tries < 1)
		tries = 1;

	while (tries-- > 0) {
		ret = __mpwr_serdev_at_cmd(mpwr, msg, timeout_ms, false, !ignore_timeout);
		if (ret != -EINVAL && (!ignore_timeout || ret != -ETIMEDOUT))
			return ret;

		if (ret != -ETIMEDOUT)
			msleep(1000);
	}

	dev_err(mpwr->dev, "AT command '%s' returned ERROR\n", msg);
	return ret;
}

static int mpwr_serdev_at_cmd_with_retry(struct mpwr_dev *mpwr, const char *msg,
					 int timeout_ms, int tries)
{
	return __mpwr_serdev_at_cmd_with_retry(mpwr, msg, timeout_ms, tries, false);
}

static int mpwr_serdev_at_cmd_with_retry_ignore_timeout(struct mpwr_dev *mpwr, const char *msg,
							int timeout_ms, int tries)
{
	return __mpwr_serdev_at_cmd_with_retry(mpwr, msg, timeout_ms, tries, true);
}

static void mpwr_serdev_receive_msg(struct mpwr_dev *mpwr, const char *msg)
{
	dev_dbg(mpwr->dev, "RECV: %s\n", msg);

	if (mpwr->variant->recv_msg)
		mpwr->variant->recv_msg(mpwr, msg);

        if (!test_bit(MPWR_F_RECEIVING_MSG, mpwr->flags))
                return;

	if (!strcmp(msg, "OK")) {
                clear_bit(MPWR_F_RECEIVING_MSG, mpwr->flags);
                mpwr->msg_ok = true;
		wake_up(&mpwr->wait);
                return;
	} else if (!strcmp(msg, "ERROR")) {
                clear_bit(MPWR_F_RECEIVING_MSG, mpwr->flags);
                mpwr->msg_ok = false;
		wake_up(&mpwr->wait);
                return;
	} else {
                int len = strlen(msg);

                if (mpwr->msg_len + len + 1 > sizeof(mpwr->msg)) {
                        dev_warn(mpwr->dev, "message buffer overflow, ignoring message\n");
                        return;
                }

                memcpy(mpwr->msg + mpwr->msg_len, msg, len + 1);
                mpwr->msg_len += len + 1;
        }
}

static int mpwr_serdev_receive_buf(struct serdev_device *serdev,
				   const unsigned char *buf, size_t count)
{
	struct mpwr_dev *mpwr = serdev_device_get_drvdata(serdev);
	size_t avail = sizeof(mpwr->rcvbuf) - mpwr->rcvbuf_fill;
	char* p;

	if (avail < count)
		count = avail;

	if (avail > 0) {
		memcpy(mpwr->rcvbuf + mpwr->rcvbuf_fill, buf, count);
		mpwr->rcvbuf_fill += count;
	}

	while (true) {
		p = strnstr(mpwr->rcvbuf, "\r\n", mpwr->rcvbuf_fill);
		if (p) {
			if (p > mpwr->rcvbuf) {
				*p = 0;
				mpwr_serdev_receive_msg(mpwr, mpwr->rcvbuf);
			}

			mpwr->rcvbuf_fill -= (p - mpwr->rcvbuf) + 2;
			memmove(mpwr->rcvbuf, p + 2, mpwr->rcvbuf_fill);
		} else {
			if (sizeof(mpwr->rcvbuf) - mpwr->rcvbuf_fill == 0) {
				mpwr->rcvbuf_fill = 0;
				dev_warn(mpwr->dev, "rcvbuf overflow\n");
			}

			break;
		}
	}

        return count;
}

static const struct serdev_device_ops mpwr_serdev_ops = {
	.receive_buf = mpwr_serdev_receive_buf,
	.write_wakeup = serdev_device_write_wakeup,
};

static int mpwr_serdev_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
        struct mpwr_dev* mpwr;
        int ret;

	ret = mpwr_probe_generic(dev, &mpwr);
	if (ret)
		return ret;

	serdev_device_set_drvdata(serdev, mpwr);
	serdev_device_set_client_ops(serdev, &mpwr_serdev_ops);
	mpwr->serdev = serdev;

        return 0;
}

static void mpwr_serdev_remove(struct serdev_device *serdev)
{
	struct mpwr_dev *mpwr = serdev_device_get_drvdata(serdev);

	mpwr_remove_generic(mpwr);
}

static const struct of_device_id mpwr_of_match_serdev[] = {
	{ .compatible = "quectel,eg25",
	  .data = &mpwr_eg25_variant },
	{},
};
MODULE_DEVICE_TABLE(of, mpwr_of_match_serdev);

static void mpwr_serdev_shutdown(struct device *dev)
{
	struct mpwr_dev *mpwr = dev_get_drvdata(dev);

	mpwr_shutdown_generic(mpwr);
}

static struct serdev_device_driver mpwr_serdev_driver = {
        .probe  = mpwr_serdev_probe,
        .remove = mpwr_serdev_remove,
        .driver = {
                .name = DRIVER_NAME,
                .of_match_table = mpwr_of_match_serdev,
		.pm = &mpwr_pm_ops,
		.shutdown = mpwr_serdev_shutdown,
        },
};

// }}}
// {{{ platdev

static int mpwr_pdev_probe(struct platform_device *pdev)
{
        struct mpwr_dev* mpwr;
        int ret;

	ret = mpwr_probe_generic(&pdev->dev, &mpwr);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, mpwr);
        return 0;
}

static int mpwr_pdev_remove(struct platform_device *pdev)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(pdev);

	return mpwr_remove_generic(mpwr);
}

static void mpwr_pdev_shutdown(struct platform_device *pdev)
{
	struct mpwr_dev *mpwr = platform_get_drvdata(pdev);

	mpwr_shutdown_generic(mpwr);
}

static const struct of_device_id mpwr_of_match_plat[] = {
	{ .compatible = "zte,mg3732",
	  .data = &mpwr_mg2723_variant },
	{},
};
MODULE_DEVICE_TABLE(of, mpwr_of_match_plat);

static struct platform_driver mpwr_platform_driver = {
	.probe = mpwr_pdev_probe,
	.remove = mpwr_pdev_remove,
	.shutdown = mpwr_pdev_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = mpwr_of_match_plat,
		.pm = &mpwr_pm_ops,
	},
};

// }}}
// {{{ driver init

static int __init mpwr_driver_init(void)
{
	int ret;

	mpwr_class = class_create(THIS_MODULE, "modem-power");
	if (IS_ERR(mpwr_class))
		return PTR_ERR(mpwr_class);

	ret = serdev_device_driver_register(&mpwr_serdev_driver);
	if (ret)
		goto err_class;

	ret = platform_driver_register(&mpwr_platform_driver);
	if (ret)
		goto err_serdev;

	return ret;

err_serdev:
	serdev_device_driver_unregister(&mpwr_serdev_driver);
err_class:
	class_destroy(mpwr_class);
	return ret;
}

static void __exit mpwr_driver_exit(void)
{
	serdev_device_driver_unregister(&mpwr_serdev_driver);
	platform_driver_unregister(&mpwr_platform_driver);
	class_destroy(mpwr_class);
}

module_init(mpwr_driver_init);
module_exit(mpwr_driver_exit);

MODULE_DESCRIPTION("Modem power manager");
MODULE_AUTHOR("Ondrej Jirman <megous@megous.com>");
MODULE_LICENSE("GPL v2");

// }}}
