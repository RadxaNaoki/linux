// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the Cirrus EP93xx matrix keypad controller.
 *
 * Copyright (c) 2008 H Hartley Sweeten <hsweeten@visionengravers.com>
 *
 * Based on the pxa27x matrix keypad controller by Rodolfo Giometti.
 *
 */

#include <linux/bits.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/slab.h>
#include <linux/soc/cirrus/ep93xx.h>
#include <linux/pm_wakeirq.h>

/*
 * Keypad Interface Register offsets
 */
#define KEY_INIT		0x00	/* Key Scan Initialization register */
#define KEY_DIAG		0x04	/* Key Scan Diagnostic register */
#define KEY_REG			0x08	/* Key Value Capture register */

/* Key Scan Initialization Register bit defines */
#define KEY_INIT_DBNC_MASK	GENMASK(23, 16)
#define KEY_INIT_DBNC_SHIFT	16
#define KEY_INIT_DIS3KY		BIT(15)
#define KEY_INIT_DIAG		BIT(14)
#define KEY_INIT_BACK		BIT(13)
#define KEY_INIT_T2		BIT(12)
#define KEY_INIT_PRSCL_MASK	GENMASK(9, 0)
#define KEY_INIT_PRSCL_SHIFT	0

/* Key Scan Diagnostic Register bit defines */
#define KEY_DIAG_MASK		GENMASK(5, 0)
#define KEY_DIAG_SHIFT		0

/* Key Value Capture Register bit defines */
#define KEY_REG_K		BIT(15)
#define KEY_REG_INT		BIT(14)
#define KEY_REG_2KEYS		BIT(13)
#define KEY_REG_1KEY		BIT(12)
#define KEY_REG_KEY2_MASK	GENMASK(11, 6)
#define KEY_REG_KEY2_SHIFT	6
#define KEY_REG_KEY1_MASK	GENMASK(5, 0)
#define KEY_REG_KEY1_SHIFT	0

#define EP93XX_MATRIX_ROWS		(8)
#define EP93XX_MATRIX_COLS		(8)

#define EP93XX_MATRIX_SIZE	(EP93XX_MATRIX_ROWS * EP93XX_MATRIX_COLS)

struct ep93xx_keypad {
	struct input_dev *input_dev;
	struct clk *clk;
	unsigned int debounce;
	u16 prescale;

	void __iomem *mmio_base;

	unsigned short keycodes[EP93XX_MATRIX_SIZE];

	int key1;
	int key2;

	int irq;

	bool enabled;
};

static irqreturn_t ep93xx_keypad_irq_handler(int irq, void *dev_id)
{
	struct ep93xx_keypad *keypad = dev_id;
	struct input_dev *input_dev = keypad->input_dev;
	unsigned int status;
	int keycode, key1, key2;

	status = __raw_readl(keypad->mmio_base + KEY_REG);

	keycode = (status & KEY_REG_KEY1_MASK) >> KEY_REG_KEY1_SHIFT;
	key1 = keypad->keycodes[keycode];

	keycode = (status & KEY_REG_KEY2_MASK) >> KEY_REG_KEY2_SHIFT;
	key2 = keypad->keycodes[keycode];

	if (status & KEY_REG_2KEYS) {
		if (keypad->key1 && key1 != keypad->key1 && key2 != keypad->key1)
			input_report_key(input_dev, keypad->key1, 0);

		if (keypad->key2 && key1 != keypad->key2 && key2 != keypad->key2)
			input_report_key(input_dev, keypad->key2, 0);

		input_report_key(input_dev, key1, 1);
		input_report_key(input_dev, key2, 1);

		keypad->key1 = key1;
		keypad->key2 = key2;

	} else if (status & KEY_REG_1KEY) {
		if (keypad->key1 && key1 != keypad->key1)
			input_report_key(input_dev, keypad->key1, 0);

		if (keypad->key2 && key1 != keypad->key2)
			input_report_key(input_dev, keypad->key2, 0);

		input_report_key(input_dev, key1, 1);

		keypad->key1 = key1;
		keypad->key2 = 0;

	} else {
		input_report_key(input_dev, keypad->key1, 0);
		input_report_key(input_dev, keypad->key2, 0);

		keypad->key1 = keypad->key2 = 0;
	}
	input_sync(input_dev);

	return IRQ_HANDLED;
}

static void ep93xx_keypad_config(struct ep93xx_keypad *keypad)
{
	unsigned int val = 0;

	val |= (keypad->debounce << KEY_INIT_DBNC_SHIFT) & KEY_INIT_DBNC_MASK;

	val |= (keypad->prescale << KEY_INIT_PRSCL_SHIFT) & KEY_INIT_PRSCL_MASK;

	__raw_writel(val, keypad->mmio_base + KEY_INIT);
}

static int ep93xx_keypad_open(struct input_dev *pdev)
{
	struct ep93xx_keypad *keypad = input_get_drvdata(pdev);

	if (!keypad->enabled) {
		ep93xx_keypad_config(keypad);
		clk_prepare_enable(keypad->clk);
		keypad->enabled = true;
	}

	return 0;
}

static void ep93xx_keypad_close(struct input_dev *pdev)
{
	struct ep93xx_keypad *keypad = input_get_drvdata(pdev);

	if (keypad->enabled) {
		clk_disable_unprepare(keypad->clk);
		keypad->enabled = false;
	}
}


static int ep93xx_keypad_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ep93xx_keypad *keypad = platform_get_drvdata(pdev);
	struct input_dev *input_dev = keypad->input_dev;

	guard(mutex)(&input_dev->mutex);

	if (keypad->enabled) {
		clk_disable(keypad->clk);
		keypad->enabled = false;
	}

	return 0;
}

static int ep93xx_keypad_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct ep93xx_keypad *keypad = platform_get_drvdata(pdev);
	struct input_dev *input_dev = keypad->input_dev;

	guard(mutex)(&input_dev->mutex);

	if (input_device_enabled(input_dev)) {
		if (!keypad->enabled) {
			ep93xx_keypad_config(keypad);
			clk_enable(keypad->clk);
			keypad->enabled = true;
		}
	}

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ep93xx_keypad_pm_ops,
				ep93xx_keypad_suspend, ep93xx_keypad_resume);

static int ep93xx_keypad_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ep93xx_keypad *keypad;
	struct input_dev *input_dev;
	int err;

	keypad = devm_kzalloc(&pdev->dev, sizeof(*keypad), GFP_KERNEL);
	if (!keypad)
		return -ENOMEM;

	keypad->irq = platform_get_irq(pdev, 0);
	if (keypad->irq < 0)
		return keypad->irq;

	keypad->mmio_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(keypad->mmio_base))
		return PTR_ERR(keypad->mmio_base);

	keypad->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(keypad->clk))
		return PTR_ERR(keypad->clk);

	device_property_read_u32(dev, "debounce-delay-ms", &keypad->debounce);
	device_property_read_u16(dev, "cirrus,prescale", &keypad->prescale);

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	keypad->input_dev = input_dev;

	input_dev->name = pdev->name;
	input_dev->id.bustype = BUS_HOST;
	input_dev->open = ep93xx_keypad_open;
	input_dev->close = ep93xx_keypad_close;

	err = matrix_keypad_build_keymap(NULL, NULL,
					 EP93XX_MATRIX_ROWS, EP93XX_MATRIX_COLS,
					 keypad->keycodes, input_dev);
	if (err)
		return err;

	if (device_property_read_bool(&pdev->dev, "autorepeat"))
		__set_bit(EV_REP, input_dev->evbit);
	input_set_drvdata(input_dev, keypad);

	err = devm_request_irq(&pdev->dev, keypad->irq,
			       ep93xx_keypad_irq_handler,
			       0, pdev->name, keypad);
	if (err)
		return err;

	err = input_register_device(input_dev);
	if (err)
		return err;

	platform_set_drvdata(pdev, keypad);

	device_init_wakeup(&pdev->dev, 1);
	err = dev_pm_set_wake_irq(&pdev->dev, keypad->irq);
	if (err)
		dev_warn(&pdev->dev, "failed to set up wakeup irq: %d\n", err);

	return 0;
}

static void ep93xx_keypad_remove(struct platform_device *pdev)
{
	dev_pm_clear_wake_irq(&pdev->dev);
}

static const struct of_device_id ep93xx_keypad_of_ids[] = {
	{ .compatible = "cirrus,ep9307-keypad" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ep93xx_keypad_of_ids);

static struct platform_driver ep93xx_keypad_driver = {
	.driver		= {
		.name	= "ep93xx-keypad",
		.pm	= pm_sleep_ptr(&ep93xx_keypad_pm_ops),
		.of_match_table = ep93xx_keypad_of_ids,
	},
	.probe		= ep93xx_keypad_probe,
	.remove		= ep93xx_keypad_remove,
};
module_platform_driver(ep93xx_keypad_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("H Hartley Sweeten <hsweeten@visionengravers.com>");
MODULE_DESCRIPTION("EP93xx Matrix Keypad Controller");
MODULE_ALIAS("platform:ep93xx-keypad");
