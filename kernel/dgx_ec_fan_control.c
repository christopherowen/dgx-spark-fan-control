// SPDX-License-Identifier: GPL-2.0-only
/*
 * NVIDIA DGX Spark EC additive fan-floor cooling device.
 *
 * The embedded controller remains the fan-policy authority.  This driver can
 * only raise its common lower RPM clamp through the validated packet service;
 * it has no upper-clamp, PWM, raw-packet, or arbitrary-memory interface.
 * Cooling state zero restores the firmware's automatic/unset value.
 */

#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

#define DGX_EC_PACKET_SUBMIT_COMMAND	0x01
#define DGX_EC_PACKET_POLL_COMMAND	0x02

#define DGX_EC_FAN_GET_CAPABILITIES	0x01
#define DGX_EC_FAN_GET_LOWER_LIMIT	0x04
#define DGX_EC_FAN_SET_LOWER_LIMIT	0x05
#define DGX_EC_FAN_GET_TELEMETRY		0x07

#define DGX_EC_FAN_CAPABILITIES_LENGTH	10U
#define DGX_EC_FAN_LIMIT_LENGTH		2U
#define DGX_EC_FAN_TELEMETRY_LENGTH	64U
#define DGX_EC_PACKET_POLL_ATTEMPTS	100U
#define DGX_EC_PACKET_POLL_DELAY_MS	10U
#define DGX_EC_CACHE_INTERVAL_MS		1000U
#define DGX_EC_PACKET_COMPLETE		0x00
#define DGX_EC_PACKET_EC_ERROR		0x01
#define DGX_EC_PACKET_PENDING		0x02
#define DGX_EC_PACKET_SP_ESPI_READ_FAILED	0x05
#define DGX_EC_PACKET_SP_MAILBOX_BUSY	0x0a
#define DGX_EC_CAPABILITIES_REVISION	1U
#define DGX_EC_EXPECTED_FAN0_MIN_RPM	1260U
#define DGX_EC_EXPECTED_FAN0_MAX_RPM	9000U
#define DGX_EC_EXPECTED_FAN1_MIN_RPM	1890U
#define DGX_EC_EXPECTED_FAN1_MAX_RPM	13500U
#define DGX_EC_LIMIT_UNSET		U16_MAX
#define DGX_EC_MAX_PLAUSIBLE_RPM	30000U
#define DGX_EC_RESTORE_RETRY_MS		100U
#define DGX_EC_RESTORE_ATTEMPTS		3U
#define DGX_EC_SHARED_PARTITION_ID	0x8003U
#define DGX_EC_SHARED_PARTITION_PROPS	0x0109U

static const uuid_t dgx_ec_packet_uuid =
	UUID_INIT(0x78b04d80, 0xd21d, 0x4986,
		  0x8a, 0xcb, 0x46, 0x7b, 0x60, 0x24, 0x7a, 0xc5);

/* State zero is automatic.  All other states are additive common RPM floors. */
static const u16 dgx_ec_floor_states[] = {
	DGX_EC_LIMIT_UNSET,
	2700U,
	3600U,
	4500U,
	5400U,
	6300U,
	7200U,
	8100U,
	9000U,
	10125U,
	11250U,
	12375U,
	13500U,
};

struct dgx_ec_fan_capabilities {
	u8 revision;
	u8 unit_mode;
	__le16 fan0_min;
	__le16 fan0_max;
	__le16 fan1_min;
	__le16 fan1_max;
} __packed;

struct dgx_ec_fan_control_data {
	struct ffa_device *ffa_dev;
	struct thermal_cooling_device *cooling_dev;
	struct notifier_block reboot_notifier;
	struct mutex lock; /* Serializes every packet transaction and state change. */
	unsigned long last_updated;
	unsigned long current_state;
	u16 rpm[2];
	u16 attempted_floor;
	bool floor_uncertain;
	bool telemetry_valid;
};

static bool dgx_ec_is_supported_platform(void)
{
	return dmi_match(DMI_SYS_VENDOR, "NVIDIA") &&
	       dmi_match(DMI_PRODUCT_NAME, "NVIDIA_DGX_Spark") &&
	       dmi_match(DMI_BOARD_NAME, "P4242");
}

static int dgx_ec_validate_transport(struct ffa_device *ffa_dev)
{
	u32 ffa_version;

	if (!uuid_equal(&ffa_dev->uuid, &dgx_ec_packet_uuid))
		return -ENODEV;

	if (!ffa_dev->ops || !ffa_dev->ops->info_ops ||
	    !ffa_dev->ops->info_ops->api_version_get ||
	    !ffa_dev->ops->msg_ops ||
	    !ffa_dev->ops->msg_ops->sync_send_receive2)
		return -EOPNOTSUPP;

	ffa_version = ffa_dev->ops->info_ops->api_version_get();
	dev_info(&ffa_dev->dev,
		 "packet service partition=%#x properties=%#x FF-A=%u.%u\n",
		 ffa_dev->vm_id, ffa_dev->properties,
		 FFA_MAJOR_VERSION(ffa_version), FFA_MINOR_VERSION(ffa_version));

	if (ffa_version != FFA_VERSION_1_2 || ffa_dev->mode_32bit ||
	    ffa_dev->vm_id != DGX_EC_SHARED_PARTITION_ID ||
	    ffa_dev->properties != DGX_EC_SHARED_PARTITION_PROPS)
		return -EOPNOTSUPP;

	return 0;
}

static int dgx_ec_packet_poll(struct ffa_device *ffa_dev, u8 *state,
			      void *output, size_t output_length)
{
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	int ret;

	raw[0] = DGX_EC_PACKET_POLL_COMMAND;
	ret = ffa_dev->ops->msg_ops->sync_send_receive2(ffa_dev, &message);
	if (ret)
		return ret;

	*state = raw[0];
	if (*state == DGX_EC_PACKET_COMPLETE && output_length)
		memcpy(output, raw + 1, output_length);

	return 0;
}

static int dgx_ec_submit_status(struct ffa_device *ffa_dev,
				struct ffa_send_direct_data2 *message)
{
	u8 *raw = (u8 *)message->data;
	u8 operation = raw[1];
	int ret;

	ret = ffa_dev->ops->msg_ops->sync_send_receive2(ffa_dev, message);
	if (ret) {
		dev_warn_ratelimited(&ffa_dev->dev,
			"FF-A submit operation=%#x failed: %d\n",
			operation, ret);
		return ret;
	}

	ret = get_unaligned_le32(raw);
	if (ret)
		dev_warn_ratelimited(&ffa_dev->dev,
			"packet submit operation=%#x status=%#x\n",
			operation, ret);
	if (ret == DGX_EC_PACKET_SP_ESPI_READ_FAILED)
		return -EIO;
	if (ret == DGX_EC_PACKET_SP_MAILBOX_BUSY)
		return -EBUSY;
	if (ret)
		return -EREMOTEIO;

	return 0;
}

static int dgx_ec_wait_for_completion(struct ffa_device *ffa_dev,
				      void *output, size_t output_length)
{
	unsigned int attempt;
	u8 state;
	int ret;

	for (attempt = 0; attempt < DGX_EC_PACKET_POLL_ATTEMPTS; attempt++) {
		ret = dgx_ec_packet_poll(ffa_dev, &state, output, output_length);
		if (ret)
			return ret;
		if (state == DGX_EC_PACKET_COMPLETE)
			return 0;
		if (state == DGX_EC_PACKET_EC_ERROR)
			return -EREMOTEIO;
		if (state != DGX_EC_PACKET_PENDING)
			return -EBADMSG;
		msleep(DGX_EC_PACKET_POLL_DELAY_MS);
	}

	dev_warn_ratelimited(&ffa_dev->dev,
		"packet poll timed out: state=%#x after %u polls\n",
		state, DGX_EC_PACKET_POLL_ATTEMPTS);
	return -ETIMEDOUT;
}

static int dgx_ec_preflight(struct ffa_device *ffa_dev, u8 operation)
{
	int ret;

	/* Drain a late completion before another request can reuse the relay. */
	ret = dgx_ec_wait_for_completion(ffa_dev, NULL, 0);
	if (ret)
		dev_warn_ratelimited(&ffa_dev->dev,
			"operation=%#x preflight failed: %d; no request submitted\n",
			operation, ret);
	return ret;
}

static int dgx_ec_read_operation(struct ffa_device *ffa_dev, u8 operation,
				 void *output)
{
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	size_t output_length;
	int ret;

	switch (operation) {
	case DGX_EC_FAN_GET_CAPABILITIES:
		output_length = DGX_EC_FAN_CAPABILITIES_LENGTH;
		break;
	case DGX_EC_FAN_GET_LOWER_LIMIT:
		output_length = DGX_EC_FAN_LIMIT_LENGTH;
		break;
	case DGX_EC_FAN_GET_TELEMETRY:
		output_length = DGX_EC_FAN_TELEMETRY_LENGTH;
		break;
	default:
		return -EPERM;
	}

	ret = dgx_ec_preflight(ffa_dev, operation);
	if (ret)
		return ret;

	raw[0] = DGX_EC_PACKET_SUBMIT_COMMAND;
	raw[1] = operation;
	raw[2] = 0;
	raw[3] = output_length;
	ret = dgx_ec_submit_status(ffa_dev, &message);
	if (ret)
		return ret;

	return dgx_ec_wait_for_completion(ffa_dev, output, output_length);
}

static int dgx_ec_write_lower_floor(struct dgx_ec_fan_control_data *data,
				  u16 value)
{
	struct ffa_device *ffa_dev = data->ffa_dev;
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	int ret;

	ret = dgx_ec_preflight(ffa_dev, DGX_EC_FAN_SET_LOWER_LIMIT);
	if (ret)
		return ret;

	raw[0] = DGX_EC_PACKET_SUBMIT_COMMAND;
	raw[1] = DGX_EC_FAN_SET_LOWER_LIMIT;
	raw[2] = DGX_EC_FAN_LIMIT_LENGTH;
	raw[3] = 0;
	put_unaligned_le16(value, raw + 4);
	/* A transport error does not prove that the EC rejected this write. */
	data->attempted_floor = value;
	data->floor_uncertain = true;
	ret = dgx_ec_submit_status(ffa_dev, &message);
	if (ret)
		return ret;

	return dgx_ec_wait_for_completion(ffa_dev, NULL, 0);
}

static int dgx_ec_read_lower_floor(struct dgx_ec_fan_control_data *data,
				   u16 *floor)
{
	u8 limit_data[DGX_EC_FAN_LIMIT_LENGTH];
	int ret;

	ret = dgx_ec_read_operation(data->ffa_dev,
				    DGX_EC_FAN_GET_LOWER_LIMIT, limit_data);
	if (!ret)
		*floor = get_unaligned_le16(limit_data);

	return ret;
}

static int dgx_ec_read_capabilities(struct dgx_ec_fan_control_data *data)
{
	struct dgx_ec_fan_capabilities capabilities;
	int ret;

	ret = dgx_ec_read_operation(data->ffa_dev,
				    DGX_EC_FAN_GET_CAPABILITIES,
				    &capabilities);
	if (ret)
		return ret;

	if (capabilities.revision != DGX_EC_CAPABILITIES_REVISION ||
	    capabilities.unit_mode != 0 ||
	    le16_to_cpu(capabilities.fan0_min) !=
		DGX_EC_EXPECTED_FAN0_MIN_RPM ||
	    le16_to_cpu(capabilities.fan0_max) !=
		DGX_EC_EXPECTED_FAN0_MAX_RPM ||
	    le16_to_cpu(capabilities.fan1_min) !=
		DGX_EC_EXPECTED_FAN1_MIN_RPM ||
	    le16_to_cpu(capabilities.fan1_max) !=
		DGX_EC_EXPECTED_FAN1_MAX_RPM)
		return -EBADMSG;

	dev_info(&data->ffa_dev->dev,
		 "pinned RPM capabilities fan0=%u..%u fan1=%u..%u\n",
		 DGX_EC_EXPECTED_FAN0_MIN_RPM,
		 DGX_EC_EXPECTED_FAN0_MAX_RPM,
		 DGX_EC_EXPECTED_FAN1_MIN_RPM,
		 DGX_EC_EXPECTED_FAN1_MAX_RPM);
	return 0;
}

static int dgx_ec_refresh(struct dgx_ec_fan_control_data *data)
{
	u8 telemetry[DGX_EC_FAN_TELEMETRY_LENGTH];
	u16 fan0_rpm;
	u16 fan1_rpm;
	int ret;

	if (data->telemetry_valid &&
	    time_before(jiffies, data->last_updated +
			msecs_to_jiffies(DGX_EC_CACHE_INTERVAL_MS)))
		return 0;

	ret = dgx_ec_read_operation(data->ffa_dev,
				    DGX_EC_FAN_GET_TELEMETRY, telemetry);
	if (ret)
		return ret;

	fan0_rpm = get_unaligned_le16(telemetry + 4);
	fan1_rpm = get_unaligned_le16(telemetry + 6);
	if (fan0_rpm > DGX_EC_MAX_PLAUSIBLE_RPM ||
	    fan1_rpm > DGX_EC_MAX_PLAUSIBLE_RPM)
		return -EBADMSG;

	data->rpm[0] = fan0_rpm;
	data->rpm[1] = fan1_rpm;
	data->last_updated = jiffies;
	data->telemetry_valid = true;
	return 0;
}

/* Called under lock, only after a complete, authenticated floor read. */
static int dgx_ec_reconcile_floor(struct dgx_ec_fan_control_data *data, u16 floor)
{
	unsigned int state;

	if (floor == DGX_EC_LIMIT_UNSET) {
		data->current_state = 0;
	} else if (floor == dgx_ec_floor_states[data->current_state]) {
		/* A completed request left the last confirmed floor intact. */
	} else if (data->floor_uncertain && floor == data->attempted_floor) {
		for (state = 1; state < ARRAY_SIZE(dgx_ec_floor_states); state++) {
			if (floor == dgx_ec_floor_states[state])
				break;
		}
		if (state == ARRAY_SIZE(dgx_ec_floor_states))
			return -ESTALE;
		data->current_state = state;
	} else {
		dev_warn_ratelimited(&data->ffa_dev->dev,
			"floor ownership mismatch: observed=%#x confirmed=%#x\n",
			floor, dgx_ec_floor_states[data->current_state]);
		return -ESTALE;
	}

	data->floor_uncertain = false;
	data->telemetry_valid = false;
	return 0;
}

static int dgx_ec_restore_automatic_locked(struct dgx_ec_fan_control_data *data)
{
	u16 floor;
	int ret;

	ret = dgx_ec_read_lower_floor(data, &floor);
	if (ret)
		return ret;
	ret = dgx_ec_reconcile_floor(data, floor);
	if (ret || floor == DGX_EC_LIMIT_UNSET)
		return ret;

	ret = dgx_ec_write_lower_floor(data, DGX_EC_LIMIT_UNSET);
	if (ret)
		return ret;
	ret = dgx_ec_read_lower_floor(data, &floor);
	if (ret)
		return ret;
	if (floor != DGX_EC_LIMIT_UNSET)
		return -EIO;

	return dgx_ec_reconcile_floor(data, floor);
}

static int dgx_ec_restore_automatic(struct dgx_ec_fan_control_data *data,
				    const char *reason)
{
	unsigned int attempt;
	int ret = 0;

	mutex_lock(&data->lock);
	for (attempt = 0; attempt < DGX_EC_RESTORE_ATTEMPTS; attempt++) {
		ret = dgx_ec_restore_automatic_locked(data);
		if (!ret || ret == -ESTALE)
			break;
		if (attempt + 1 < DGX_EC_RESTORE_ATTEMPTS)
			msleep(DGX_EC_RESTORE_RETRY_MS);
	}
	if (ret)
		dev_emerg(&data->ffa_dev->dev,
			  "failed to restore automatic fan policy for %s: %d\n",
			  reason, ret);
	else
		dev_info(&data->ffa_dev->dev,
			 "automatic fan policy active for %s\n", reason);
	mutex_unlock(&data->lock);

	return ret;
}

static int dgx_ec_hwmon_read(struct device *dev,
			     enum hwmon_sensor_types type, u32 attr,
			     int channel, long *val)
{
	struct dgx_ec_fan_control_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_fan || attr != hwmon_fan_input ||
	    channel < 0 || channel >= ARRAY_SIZE(data->rpm))
		return -EOPNOTSUPP;

	ret = mutex_lock_interruptible(&data->lock);
	if (ret)
		return ret;
	ret = dgx_ec_refresh(data);
	if (!ret)
		*val = data->rpm[channel];
	mutex_unlock(&data->lock);

	return ret;
}

static const struct hwmon_ops dgx_ec_hwmon_ops = {
	.visible = 0444,
	.read = dgx_ec_hwmon_read,
};

static const struct hwmon_channel_info * const dgx_ec_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	NULL
};

static const struct hwmon_chip_info dgx_ec_chip_info = {
	.ops = &dgx_ec_hwmon_ops,
	.info = dgx_ec_hwmon_info,
};

static int dgx_ec_get_max_state(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	*state = ARRAY_SIZE(dgx_ec_floor_states) - 1;
	return 0;
}

static int dgx_ec_get_cur_state(struct thermal_cooling_device *cdev,
				unsigned long *state)
{
	struct dgx_ec_fan_control_data *data = cdev->devdata;
	u16 floor;
	int ret;

	ret = mutex_lock_interruptible(&data->lock);
	if (ret)
		return ret;
	ret = dgx_ec_read_lower_floor(data, &floor);
	if (!ret)
		ret = dgx_ec_reconcile_floor(data, floor);
	if (!ret)
		*state = data->current_state;
	mutex_unlock(&data->lock);

	return ret;
}

static int dgx_ec_set_cur_state(struct thermal_cooling_device *cdev,
				unsigned long state)
{
	struct dgx_ec_fan_control_data *data = cdev->devdata;
	u16 current_floor;
	u16 target_floor;
	int restore_ret;
	int ret;

	if (state >= ARRAY_SIZE(dgx_ec_floor_states))
		return -EINVAL;

	ret = mutex_lock_interruptible(&data->lock);
	if (ret)
		return ret;

	ret = dgx_ec_read_lower_floor(data, &current_floor);
	if (ret)
		goto unlock;
	ret = dgx_ec_reconcile_floor(data, current_floor);
	if (ret)
		goto unlock;
	if (state == data->current_state)
		goto unlock;
	if (state == 0) {
		ret = dgx_ec_restore_automatic_locked(data);
		goto report;
	}

	target_floor = dgx_ec_floor_states[state];
	ret = dgx_ec_write_lower_floor(data, target_floor);
	if (ret)
		goto failed_write;
	ret = dgx_ec_read_lower_floor(data, &current_floor);
	if (ret)
		goto failed_write;
	if (current_floor != target_floor) {
		ret = -EIO;
		goto failed_write;
	}

	ret = dgx_ec_reconcile_floor(data, current_floor);

report:
	if (!ret)
		dev_info(&data->ffa_dev->dev,
			 "cooling state=%lu lower floor=%#x\n", state,
			 dgx_ec_floor_states[state]);
	goto unlock;

failed_write:
	restore_ret = dgx_ec_restore_automatic_locked(data);
	if (restore_ret)
		dev_emerg(&data->ffa_dev->dev,
			  "state change failed (%d), automatic restore failed (%d)\n",
			  ret, restore_ret);

unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static const struct thermal_cooling_device_ops dgx_ec_cooling_ops = {
	.get_max_state = dgx_ec_get_max_state,
	.get_cur_state = dgx_ec_get_cur_state,
	.set_cur_state = dgx_ec_set_cur_state,
};

static int dgx_ec_reboot_notify(struct notifier_block *notifier,
				unsigned long action, void *unused)
{
	struct dgx_ec_fan_control_data *data =
		container_of(notifier, struct dgx_ec_fan_control_data,
			     reboot_notifier);

	dgx_ec_restore_automatic(data, "reboot");
	return NOTIFY_DONE;
}

static int dgx_ec_fan_control_suspend(struct device *dev)
{
	struct dgx_ec_fan_control_data *data = dev_get_drvdata(dev);

	return dgx_ec_restore_automatic(data, "suspend");
}

static int dgx_ec_fan_control_resume(struct device *dev)
{
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(dgx_ec_fan_control_pm_ops,
				dgx_ec_fan_control_suspend,
				dgx_ec_fan_control_resume);

static int dgx_ec_fan_control_probe(struct ffa_device *ffa_dev)
{
	struct dgx_ec_fan_control_data *data;
	struct device *hwmon_dev;
	u16 floor;
	int ret;

	if (!dgx_ec_is_supported_platform())
		return -ENODEV;
	ret = dgx_ec_validate_transport(ffa_dev);
	if (ret)
		return ret;

	data = devm_kzalloc(&ffa_dev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->ffa_dev = ffa_dev;
	mutex_init(&data->lock);
	ffa_dev_set_drvdata(ffa_dev, data);

	mutex_lock(&data->lock);
	ret = dgx_ec_read_capabilities(data);
	if (ret)
		goto unlock;
	ret = dgx_ec_read_lower_floor(data, &floor);
	if (ret)
		goto unlock;
	if (floor != DGX_EC_LIMIT_UNSET) {
		dev_err(&ffa_dev->dev,
			"refusing to replace existing lower floor %#x\n", floor);
		ret = -EBUSY;
		goto unlock;
	}
	ret = dgx_ec_refresh(data);
	if (ret)
		goto unlock;
	dev_info(&ffa_dev->dev, "baseline fan1=%u fan2=%u RPM\n",
		 data->rpm[0], data->rpm[1]);
	mutex_unlock(&data->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(&ffa_dev->dev,
						 "dgx_ec_fan", data,
						 &dgx_ec_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return dev_err_probe(&ffa_dev->dev, PTR_ERR(hwmon_dev),
				     "failed to register hwmon\n");

	data->cooling_dev = thermal_cooling_device_register(
		"dgx_ec_fan_floor", data, &dgx_ec_cooling_ops);
	if (IS_ERR(data->cooling_dev))
		return dev_err_probe(&ffa_dev->dev, PTR_ERR(data->cooling_dev),
				     "failed to register cooling device\n");

	data->reboot_notifier.notifier_call = dgx_ec_reboot_notify;
	ret = devm_register_reboot_notifier(&ffa_dev->dev,
					    &data->reboot_notifier);
	if (ret) {
		thermal_cooling_device_unregister(data->cooling_dev);
		return dev_err_probe(&ffa_dev->dev, ret,
				     "failed to register reboot restoration\n");
	}

	dev_info(&ffa_dev->dev,
		 "additive fan-floor cooling device registered in automatic state\n");
	return 0;

unlock:
	mutex_unlock(&data->lock);
	return dev_err_probe(&ffa_dev->dev, ret,
			     "refusing fan-floor cooling device\n");
}

static void dgx_ec_fan_control_remove(struct ffa_device *ffa_dev)
{
	struct dgx_ec_fan_control_data *data = ffa_dev_get_drvdata(ffa_dev);

	if (!data)
		return;
	thermal_cooling_device_unregister(data->cooling_dev);
	dgx_ec_restore_automatic(data, "module removal");
	dev_info(&ffa_dev->dev, "fan-floor cooling device removed\n");
}

static const struct ffa_device_id dgx_ec_fan_control_ids[] = {
	{ UUID_INIT(0x78b04d80, 0xd21d, 0x4986,
		    0x8a, 0xcb, 0x46, 0x7b, 0x60, 0x24, 0x7a, 0xc5) },
	{}
};

static struct ffa_driver dgx_ec_fan_control_driver = {
	.name = "dgx-ec-fan-control",
	.probe = dgx_ec_fan_control_probe,
	.remove = dgx_ec_fan_control_remove,
	.id_table = dgx_ec_fan_control_ids,
	.driver = {
		.pm = pm_sleep_ptr(&dgx_ec_fan_control_pm_ops),
	},
};

static int __init dgx_ec_fan_control_init(void)
{
	if (!dgx_ec_is_supported_platform())
		return -ENODEV;
	return ffa_register(&dgx_ec_fan_control_driver);
}

static void __exit dgx_ec_fan_control_exit(void)
{
	ffa_unregister(&dgx_ec_fan_control_driver);
}

module_init(dgx_ec_fan_control_init);
module_exit(dgx_ec_fan_control_exit);

MODULE_SOFTDEP("pre: arm-ffa");
MODULE_AUTHOR("Christopher Owen");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NVIDIA DGX Spark EC additive fan-floor cooling device");
MODULE_VERSION("0.1.1");
