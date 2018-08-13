/*  drivers/misc/sec_jack.c
 *
 *  Copyright (C) 2010 Samsung Electronics Co.Ltd
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
#include <linux/extcon-provider.h>
#else
#include <linux/extcon.h>
#endif
#include <linux/input.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/sec_jack.h>
#include <linux/iio/consumer.h>

#include <asm/system_info.h>

#define MAX_ZONE_LIMIT		10
#define SEND_KEY_CHECK_TIME_MS	30		/* 30ms */
#define DET_CHECK_TIME_MS	150		/* 150ms */
#define WAKE_LOCK_TIME		(5000)	/* 5 sec */
#define NUM_INPUT_DEVICE_ID	2

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
static struct class *jack_class;
static struct device *jack_dev;
#endif

struct sec_jack_info {
	struct sec_jack_platform_data *pdata;
	struct delayed_work jack_detect_work;
	struct work_struct buttons_work;
	struct workqueue_struct *queue;
	struct input_dev *input_dev;
	struct wakeup_source det_wakeup_source;
	struct sec_jack_zone *zone;
	struct input_handler handler;
	struct input_handle handle;
	struct input_device_id ids[NUM_INPUT_DEVICE_ID];
	int det_irq;
	int dev_id;
	int pressed;
	int pressed_code;
	unsigned int cur_jack_type;

	struct iio_channel *adc_channel;

	/* sysfs name HeadsetObserver.java looks for to track headset state */
	struct extcon_dev *switch_jack_detection;
};

/* with some modifications like moving all the gpio structs inside
 * the platform data and getting the name for the switch and
 * gpio_event from the platform data, the driver could support more than
 * one headset jack, but currently user space is looking only for
 * one key file and switch for a headset so it'd be overkill and
 * untestable so we limit to one instantiation for now.
 */
static atomic_t instantiated = ATOMIC_INIT(0);

/* sysfs name HeadsetObserver.java looks for to track headset state
 */
static const unsigned int jack_cables[] = {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
	EXTCON_JACK_HEADPHONE,
#else
	EXTCON_NONE,
#endif
};

static const unsigned int sendend_cables[] = {
	EXTCON_NONE,
};

static void sec_jack_set_micbias_state(
	struct sec_jack_platform_data *pdata, bool on);
static int sec_jack_get_adc_value(struct sec_jack_info *info);

/* gpio_input driver does not support to read adc value.
 * We use input filter to support 3-buttons of headset
 * without changing gpio_input driver.
 */
static bool sec_jack_buttons_filter(struct input_handle *handle,
				    unsigned int type, unsigned int code,
				    int value)
{
	struct sec_jack_info *hi = handle->handler->private;

	if (type != EV_KEY || code != KEY_UNKNOWN)
		return false;

	hi->pressed = value;

	/* This is called in timer handler of gpio_input driver.
	 * We use workqueue to read adc value.
	 */
	queue_work(hi->queue, &hi->buttons_work);

	return true;
}

static int sec_jack_buttons_connect(struct input_handler *handler,
				    struct input_dev *dev,
				    const struct input_device_id *id)
{
	struct sec_jack_info *hi;
	struct sec_jack_platform_data *pdata;
	struct sec_jack_buttons_zone *btn_zones;
	int err;
	int i;

	/* bind input_handler to input device related to only sec_jack */
	if (strncmp(dev->name, "gpio-keys", 8)) {
		pr_err("%s: not connecting: %s", __func__, dev->name);
		return -ENODEV;
	}

	pr_info("%s: connected input device: %s", __func__, dev->name);

	hi = handler->private;
	pdata = hi->pdata;
	btn_zones = pdata->buttons_zones;

	hi->input_dev = dev;
	hi->handle.dev = dev;
	hi->handle.handler = handler;
	hi->handle.open = 0;
	hi->handle.name = "sec_jack_buttons";

	err = input_register_handle(&hi->handle);
	if (err) {
		pr_err("%s: Failed to register sec_jack buttons handle, "
			"error %d\n", __func__, err);
		goto err_register_handle;
	}

	err = input_open_device(&hi->handle);
	if (err) {
		pr_err("%s: Failed to open input device, error %d\n",
			__func__, err);
		goto err_open_device;
	}

	for (i = 0; i < pdata->num_buttons_zones; i++)
		input_set_capability(dev, EV_KEY, btn_zones[i].code);

	input_set_capability(dev, EV_SW, SW_MICROPHONE_INSERT);
	input_set_capability(dev, EV_SW, SW_HEADPHONE_INSERT);
	input_sync(hi->input_dev);

	return 0;

err_open_device:
	input_unregister_handle(&hi->handle);
err_register_handle:

	return err;
}

static void sec_jack_buttons_disconnect(struct input_handle *handle)
{
	struct sec_jack_info *hi = container_of(handle, struct sec_jack_info, handle);

	input_close_device(handle);
	input_unregister_handle(handle);

	hi->input_dev = NULL;
	memset(&hi->handle, 0, sizeof(struct input_handle));
}

static void sec_jack_set_type(struct sec_jack_info *hi, int jack_type)
{
	struct sec_jack_platform_data *pdata = hi->pdata;
	u32 state;

	/* this can happen during slow inserts where we think we identified
	 * the type but then we get another interrupt and do it again
	 */
	if (jack_type == hi->cur_jack_type) {
		if (jack_type != SEC_HEADSET_4POLE)
			sec_jack_set_micbias_state(pdata, false);
		return;
	}

	if (!hi->input_dev) {
		pr_err("%s: input_dev is NULL\n", __func__);
		return;
	}

	hi->cur_jack_type = jack_type;

	if (jack_type != SEC_JACK_NO_DEVICE) {
		/* micbias is left enabled for 4pole and disabled otherwise */
		if (jack_type != SEC_HEADSET_4POLE)
			sec_jack_set_micbias_state(pdata, false);

		input_report_switch(hi->input_dev, SW_HEADPHONE_INSERT, 1);
		if (hi->cur_jack_type & SEC_HEADSET_4POLE)
			input_report_switch(hi->input_dev, SW_MICROPHONE_INSERT, 1);
		input_sync(hi->input_dev);
	} else {
		/* wait for the work to finish if it is queued */
		flush_work(&hi->buttons_work);

		input_report_switch(hi->input_dev, SW_HEADPHONE_INSERT, 0);
		input_report_switch(hi->input_dev, SW_MICROPHONE_INSERT, 0);
		input_sync(hi->input_dev);
	}

	pr_info("%s : jack_type = %d\n", __func__, jack_type);
	state = jack_type & (SEC_HEADSET_4POLE | SEC_HEADSET_3POLE) ? 1 : 0;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0))
	extcon_set_state_sync(hi->switch_jack_detection, EXTCON_JACK_HEADPHONE, state);
#else
	extcon_set_state(hi->switch_jack_detection, state);
#endif
}

static void handle_jack_not_inserted(struct sec_jack_info *hi)
{
	sec_jack_set_type(hi, SEC_JACK_NO_DEVICE);
	/* hi->pdata->set_micbias_state(false); */
}

static void determine_jack_type(struct sec_jack_info *hi)
{
	struct sec_jack_platform_data *pdata = hi->pdata;
	struct sec_jack_zone *zones = pdata->zones;
	int size = pdata->num_zones;
	int count[MAX_ZONE_LIMIT] = {0};
	int adc;
	int i;
	unsigned npolarity = !pdata->det_active_high;

	/* set mic bias to enable adc */
	sec_jack_set_micbias_state(pdata, true);

	while (gpio_get_value(pdata->det_gpio) ^ npolarity) {
		adc = sec_jack_get_adc_value(hi);
		pr_debug("%s: adc = %d\n", __func__, adc);

		/* determine the type of headset based on the
		 * adc value.  An adc value can fall in various
		 * ranges or zones.  Within some ranges, the type
		 * can be returned immediately.  Within others, the
		 * value is considered unstable and we need to sample
		 * a few more types (up to the limit determined by
		 * the range) before we return the type for that range.
		 */
		for (i = 0; i < size; i++) {
			if (adc <= zones[i].adc_high) {
				if (++count[i] > zones[i].check_count) {
					sec_jack_set_type(hi,
							  zones[i].jack_type);
					return;
				}
				if (zones[i].delay_ms > 0)
					msleep(zones[i].delay_ms);
				break;
			}
		}
	}
	/* jack removed before detection complete */
	pr_debug("%s : jack removed before detection complete\n", __func__);
	handle_jack_not_inserted(hi);
}

/* thread run whenever the headset detect state changes (either insertion
 * or removal).
 */
static irqreturn_t sec_jack_detect_irq_thread(int irq, void *dev_id)
{
	struct sec_jack_info *hi = dev_id;
	struct sec_jack_platform_data *pdata = hi->pdata;
	int time_left_ms = DET_CHECK_TIME_MS;
	unsigned npolarity = !pdata->det_active_high;

	/* prevent suspend to allow user space to respond to switch */
	__pm_wakeup_event(&hi->det_wakeup_source, WAKE_LOCK_TIME);

	pr_info("[EarJack] detect_irq(%d)\n",
		gpio_get_value(pdata->det_gpio) ^ npolarity);

	/* debounce headset jack.  don't try to determine the type of
	 * headset until the detect state is true for a while.
	 */
	while (time_left_ms > 0) {
		if (!(gpio_get_value(pdata->det_gpio) ^ npolarity)) {
			/* jack not detected. */
			handle_jack_not_inserted(hi);
			return IRQ_HANDLED;
		}
		usleep_range(10000, 20000);
		time_left_ms -= 10;
	}
	/* jack presence was detected the whole time, figure out which type */
	determine_jack_type(hi);
	return IRQ_HANDLED;
}

/* thread run whenever the button of headset is pressed or released */
void sec_jack_buttons_work(struct work_struct *work)
{
	struct sec_jack_info *hi =
		container_of(work, struct sec_jack_info, buttons_work);
	struct sec_jack_platform_data *pdata = hi->pdata;
	struct sec_jack_buttons_zone *btn_zones = pdata->buttons_zones;
	int adc;
	int i;

	if (!hi->input_dev)
		return;

	if (hi->cur_jack_type != SEC_HEADSET_4POLE) {
		pr_debug("%s: skip work. cur_jack_type=%d\n", __func__,
			hi->cur_jack_type);
		return;
	}

	/* prevent suspend to allow user space to respond to switch */
	__pm_wakeup_event(&hi->det_wakeup_source, WAKE_LOCK_TIME);

	/* when button is released */
	if (hi->pressed == 0) {
		input_report_key(hi->input_dev, hi->pressed_code, 0);
		input_sync(hi->input_dev);
		pr_debug("%s: keycode=%d, is released\n", __func__,
			hi->pressed_code);
		return;
	}

	/* when button is pressed */
	adc = sec_jack_get_adc_value(hi);

	for (i = 0; i < pdata->num_buttons_zones; i++)
		if (adc >= btn_zones[i].adc_low &&
		    adc <= btn_zones[i].adc_high) {
			hi->pressed_code = btn_zones[i].code;
			input_report_key(hi->input_dev, btn_zones[i].code, 1);
			input_sync(hi->input_dev);
			pr_debug("%s: keycode=%d, is pressed\n", __func__,
				btn_zones[i].code);
			return;
		}

	pr_warn("%s: key is skipped. ADC value is %d\n", __func__, adc);
}

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
static ssize_t select_jack_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	pr_info("%s : operate nothing\n", __func__);

	return 0;
}

static ssize_t select_jack_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t size)
{
	struct sec_jack_info *hi = dev_get_drvdata(dev);
	struct sec_jack_platform_data *pdata = hi->pdata;
	int value = 0;


	sscanf(buf, "%d", &value);
	pr_err("%s: User  selection : 0X%x", __func__, value);
	if (value == SEC_HEADSET_4POLE) {
		sec_jack_set_micbias_state(pdata, true);
		msleep(100);
	}

	sec_jack_set_type(hi, value);

	return size;
}

static DEVICE_ATTR(select_jack, S_IRUGO | S_IWUSR | S_IWGRP,
	select_jack_show, select_jack_store);
#endif

static void sec_jack_set_micbias_state(
	struct sec_jack_platform_data *pdata, bool on)
{
	pr_info("%s: state=%s\n", __func__, (on?"on":"off"));
	if (system_rev < 0x3)
		gpio_set_value(pdata->ear_micbias_alt_enable_gpio, on);
	else
		gpio_set_value(pdata->ear_micbias_enable_gpio, on);
}

static int sec_jack_get_adc_value(struct sec_jack_info *info)
{
	int val, ret;
	if (system_rev < 0x2)
		val = 2000; /* temporary fix: adc_get_value(0); */
	else {
		if (!info->adc_channel)
			pr_err("%s: info->adc_channel is NULL\n", __func__);

		if (!info->adc_channel->indio_dev)
			pr_err("%s: info->adc_channel->indio_dev is NULL\n", __func__);

		ret = iio_read_channel_raw(info->adc_channel, &val);
		if (ret < 0) {
			pr_err("%s: iio read channel failed. (%d)\n", __func__, ret);
			val = 0;
		}
	}
	pr_info("%s: adc_value=%d\n", __func__, val);
	return  val;
}

static int sec_jack_init_gpio(struct platform_device *pdev,
	struct sec_jack_platform_data *pdata)
{
	int ret = 0;
	int ear_micbias_gpio = 0;
	int micbias_gpio = pdata->micbias_enable_gpio;
	int det_gpio = pdata->det_gpio;

	if (system_rev < 0x3)
		ear_micbias_gpio = pdata->ear_micbias_alt_enable_gpio;
	else
		ear_micbias_gpio = pdata->ear_micbias_enable_gpio;

	ret = devm_gpio_request(&pdev->dev, micbias_gpio, "micbias_enable");
	if (ret < 0)
		return ret;

	ret = devm_gpio_request(&pdev->dev, ear_micbias_gpio, "ear_micbias_enable");
	if (ret < 0)
		goto err_micbias;

	ret = devm_gpio_request(&pdev->dev, det_gpio, "ear_jack_detect");
	if (ret) {
		pr_err("%s : gpio_request failed for %d\n",
		       __func__, pdata->det_gpio);
		goto err_ear_micbias;
	}

	ret = gpio_direction_output(micbias_gpio, 0);
	if (ret < 0)
		goto cleanup;

	ret = gpio_direction_output(ear_micbias_gpio, 0);
	if (ret < 0)
		goto cleanup;

	return ret;

cleanup:
	devm_gpio_free(&pdev->dev, det_gpio);
err_ear_micbias:
	devm_gpio_free(&pdev->dev, ear_micbias_gpio);
err_micbias:
	devm_gpio_free(&pdev->dev, micbias_gpio);

	return ret;
}

#ifdef CONFIG_OF
static struct sec_jack_platform_data* sec_jack_parse_dt(struct platform_device *pdev)
{
	struct sec_jack_platform_data *pdata;
	struct device_node *jack_zones_np, *button_zones_np;
	struct device_node *entry;
	struct device_node *np = pdev->dev.of_node;
	int i;

	pdata = kzalloc(sizeof(struct sec_jack_platform_data), GFP_KERNEL);

	if (!pdata) {
		pr_err("%s: could not allocate platform data.\n", __func__);
		return NULL;
	}

	pdata->det_gpio = of_get_named_gpio(np, "det-gpio", 0);
	pdata->micbias_enable_gpio = of_get_named_gpio(np, "micbias-enable", 0);
	pdata->ear_micbias_enable_gpio =
		of_get_named_gpio(np, "ear-micbias-enable", 0);
	pdata->ear_micbias_alt_enable_gpio =
		of_get_named_gpio(np, "ear-micbias-enable-alt", 0);

	// jack zones
	jack_zones_np = of_get_child_by_name(np, "jack-zones");
	if (!jack_zones_np) {
		pr_err("%s: could not find jack-zones node\n",
			of_node_full_name(np));
		return NULL;
	}

	pdata->num_zones = of_get_child_count(jack_zones_np);
	if (pdata->num_zones == 0) {
		pr_err("%s: no jack zones specified\n", of_node_full_name(np));
		goto err;
	}

	pdata->zones = kzalloc(
		sizeof(struct sec_jack_zone)*pdata->num_zones, GFP_KERNEL);

	i = 0;
	for_each_child_of_node(jack_zones_np, entry) {
		struct sec_jack_zone *zone;
		u32 val = 0;

		zone = &pdata->zones[i];
		if (!of_property_read_u32(entry, "adc-high", &val))
			zone->adc_high = val;
		if (!of_property_read_u32(entry, "delay-ms", &val))
			zone->delay_ms = val;
		if (!of_property_read_u32(entry, "check-count", &val))
			zone->check_count = val;
		if (!of_property_read_u32(entry, "jack-type", &val))
			zone->jack_type = val;

		i++;
	}

	// button zones
	button_zones_np = of_get_child_by_name(np, "jack-button-zones");
	if (!button_zones_np) {
		pr_err("%s: could not find jack-button-zones node\n",
			of_node_full_name(np));
		goto err;
	}

	pdata->num_buttons_zones = of_get_child_count(button_zones_np);
	if (pdata->num_buttons_zones == 0) {
		pr_err("%s: no jack button zones specified\n", of_node_full_name(np));
	}

	pdata->buttons_zones = kzalloc(
		sizeof(struct sec_jack_buttons_zone)*pdata->num_buttons_zones, GFP_KERNEL);

	i = 0;
	for_each_child_of_node(button_zones_np, entry) {
		struct sec_jack_buttons_zone *button_zone;
		u32 val = 0;

		button_zone = &pdata->buttons_zones[i];
		if (!of_property_read_u32(entry, "code", &val))
			button_zone->code = val;
		if (!of_property_read_u32(entry, "adc-low", &val))
			button_zone->adc_low = val;
		if (!of_property_read_u32(entry, "adc-high", &val))
			button_zone->adc_high = val;

		i++;
	}

	pr_info("%s: det_gpio=%d\n", __func__, pdata->det_gpio);
	pr_info("%s: micbias_enable_gpio=%d\n", __func__, pdata->micbias_enable_gpio);
	pr_info("%s: ear_micbias_enable_gpio=%d\n", __func__, pdata->ear_micbias_enable_gpio);
	pr_info("%s: ear_micbias_alt_enable_gpio=%d\n", __func__, pdata->ear_micbias_alt_enable_gpio);

	return pdata;

err:
	kfree(pdata);
	return NULL;
}
#else
static struct sec_jack_platform_data* sec_jack_parse_dt(struct platform_device *pdev)
{
	return -EINVAL;
}
#endif

static int sec_jack_setup_adc_channel(
	struct sec_jack_info *info,
	struct platform_device *pdev)
{
	struct iio_channel *chan;
	int ret = 0;

	if (!info)
		return -EINVAL;

	chan = iio_channel_get(NULL, "stmpe-adc-ch4");
	if (IS_ERR(chan)) {
		ret = PTR_ERR(chan);
		dev_err(&pdev->dev, "failed to get stmpe811-adc-ch4 (%d)\n",
			ret);
		return ret;
	}
	info->adc_channel = chan;

	return ret;
}

static int sec_jack_probe(struct platform_device *pdev)
{
	struct sec_jack_info *hi;
	struct sec_jack_platform_data *pdata = NULL;
	int ret;

	if (pdev->dev.platform_data) {
		pdata = pdev->dev.platform_data;
	} else if (pdev->dev.of_node) {
		pdata = sec_jack_parse_dt(pdev);
	}

	pr_info("%s : Registering jack driver\n", __func__);
	if (!pdata) {
		pr_err("%s : pdata is NULL.\n", __func__);
		return -ENODEV;
	}

	if (!pdata->zones || pdata->num_zones > MAX_ZONE_LIMIT) {
		pr_err("%s : need to check pdata\n", __func__);
		return -ENODEV;
	}

	ret = sec_jack_init_gpio(pdev, pdata);
	if (ret) {
		pr_err("%s: error initializing gpios %d.\n", __func__, ret);
		return -ENODEV;
	}

	if (atomic_xchg(&instantiated, 1)) {
		pr_err("%s : already instantiated, can only have one\n",
			__func__);
		return -ENODEV;
	}

	hi = kzalloc(sizeof(struct sec_jack_info), GFP_KERNEL);
	if (hi == NULL) {
		pr_err("%s : Failed to allocate memory.\n", __func__);
		ret = -ENOMEM;
		goto err_kzalloc;
	}

	hi->pdata = pdata;

	ret = sec_jack_setup_adc_channel(hi, pdev);
	if (ret)
		goto err_extcon_dev_register;

	/* make the id of our gpi_event device the same as our platform device,
	 * which makes it the responsiblity of the board file to make sure
	 * it is unique relative to other gpio_event devices
	 */
	hi->dev_id = pdev->id;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
	hi->switch_jack_detection = devm_extcon_dev_allocate(&pdev->dev, jack_cables);
#else
	hi->switch_jack_detection->name = "h2w";
#endif
	pr_info("%s: extcon dev name %s\n", __func__, dev_name(pdev->dev.parent));
	ret = devm_extcon_dev_register(&pdev->dev, hi->switch_jack_detection);
	if (ret < 0) {
		pr_err("%s : Failed to register switch device\n", __func__);
		goto err_extcon_dev_register;
	}

	wakeup_source_init(&hi->det_wakeup_source, "sec_jack_det");

	INIT_WORK(&hi->buttons_work, sec_jack_buttons_work);
	hi->queue = create_singlethread_workqueue("sec_jack_wq");
	if (hi->queue == NULL) {
		ret = -ENOMEM;
		pr_err("%s: Failed to create workqueue\n", __func__);
		goto err_create_wq_failed;
	}

	hi->det_irq = gpio_to_irq(pdata->det_gpio);

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	jack_class = class_create(THIS_MODULE, "jack");
	if (IS_ERR(jack_class))
		pr_err("Failed to create class(sec_jack)\n");

	/* support PBA function test */
	jack_dev = device_create(jack_class, NULL, 0, hi, "jack_selector");
	if (IS_ERR(jack_dev))
		pr_err("Failed to create device(sec_jack)!= %d\n",
			IS_ERR(jack_dev));

	if (device_create_file(jack_dev, &dev_attr_select_jack) < 0)
		pr_err("Failed to create device file(%s)!\n",
			dev_attr_select_jack.attr.name);
#endif
	set_bit(EV_KEY, hi->ids[0].evbit);
	hi->ids[0].flags = INPUT_DEVICE_ID_MATCH_EVBIT;
	hi->handler.filter = sec_jack_buttons_filter;
	hi->handler.connect = sec_jack_buttons_connect;
	hi->handler.disconnect = sec_jack_buttons_disconnect;
	hi->handler.name = "sec_jack_buttons";
	hi->handler.id_table = hi->ids;
	hi->handler.private = hi;

	ret = input_register_handler(&hi->handler);
	if (ret) {
		pr_err("%s : Failed to register_handler\n", __func__);
		goto err_register_input_handler;
	}
	ret = request_threaded_irq(hi->det_irq, NULL,
				   sec_jack_detect_irq_thread,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				   IRQF_ONESHOT, "sec_headset_detect", hi);
	if (ret) {
		pr_err("%s : Failed to request_irq.\n", __func__);
		goto err_request_detect_irq;
	}

	/* to handle insert/removal when we're sleeping in a call */
	ret = enable_irq_wake(hi->det_irq);
	if (ret) {
		pr_err("%s : Failed to enable_irq_wake.\n", __func__);
		goto err_regsister_data;
	}

	dev_set_drvdata(&pdev->dev, hi);

	sec_jack_set_micbias_state(pdata, true);
	determine_jack_type(hi);

	return 0;

err_regsister_data:
	free_irq(hi->det_irq, hi);
err_request_detect_irq:
	input_unregister_handler(&hi->handler);
err_register_input_handler:
	destroy_workqueue(hi->queue);
err_create_wq_failed:
	wakeup_source_trash(&hi->det_wakeup_source);
	devm_extcon_dev_unregister(&pdev->dev, hi->switch_jack_detection);
err_extcon_dev_register:
	gpio_free(pdata->det_gpio);
	kfree(hi);
	kfree(pdata);
err_kzalloc:
	atomic_set(&instantiated, 0);

	return ret;
}

static int sec_jack_remove(struct platform_device *pdev)
{

	struct sec_jack_info *hi = dev_get_drvdata(&pdev->dev);
	struct sec_jack_platform_data *pdata = hi->pdata;

	pr_info("%s :\n", __func__);
	disable_irq_wake(hi->det_irq);
	free_irq(hi->det_irq, hi);
	destroy_workqueue(hi->queue);
	input_unregister_handler(&hi->handler);
	wakeup_source_trash(&hi->det_wakeup_source);
	devm_extcon_dev_unregister(&pdev->dev, hi->switch_jack_detection);
	devm_gpio_free(&pdev->dev, hi->pdata->det_gpio);
	devm_gpio_free(&pdev->dev, pdata->micbias_enable_gpio);

	if (system_rev < 0x3)
		devm_gpio_free(&pdev->dev, pdata->ear_micbias_alt_enable_gpio);
	else
		devm_gpio_free(&pdev->dev, pdata->ear_micbias_enable_gpio);

	kfree(hi);
	atomic_set(&instantiated, 0);

	return 0;
}

static const struct of_device_id sec_jack_of_ids[] = {
	{ .compatible = "samsung,sec_jack" },
	{ }
};

static struct platform_driver sec_jack_driver = {
	.probe = sec_jack_probe,
	.remove = sec_jack_remove,
	.driver = {
			.name = "sec_jack",
			.of_match_table = sec_jack_of_ids,
			.owner = THIS_MODULE,
		   },
};
static int __init sec_jack_init(void)
{
	return platform_driver_register(&sec_jack_driver);
}

static void __exit sec_jack_exit(void)
{
	platform_driver_unregister(&sec_jack_driver);
}

late_initcall(sec_jack_init);
module_exit(sec_jack_exit);

MODULE_AUTHOR("ms17.kim@samsung.com");
MODULE_DESCRIPTION("Samsung Electronics Corp Ear-Jack detection driver");
MODULE_LICENSE("GPL");
