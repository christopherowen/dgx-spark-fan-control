// SPDX-License-Identifier: GPL-2.0-only
/* Operator-only, one-shot diagnostic for the documented SoC 2.155.11 race.
 * Retains the existing driver and takes its mutex. No arbitrary addresses,
 * acknowledgement-register reads, or persistent control surface. Optional
 * restoration can only remove an explicitly identified additive floor.
 */
#include <linux/arm_ffa.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/thermal.h>
#include <linux/unaligned.h>
#include <linux/uuid.h>

static bool recover;
module_param(recover, bool, 0400);
MODULE_PARM_DESC(recover, "Allow one read-lower-floor request only after two idle mailbox observations");
static unsigned int restore_floor = 0xffff;
module_param(restore_floor, uint, 0400);
MODULE_PARM_DESC(restore_floor, "Operator-confirmed existing floor to remove; default 65535 disables restoration");

static const uuid_t packet_uuid =
	UUID_INIT(0x78b04d80, 0xd21d, 0x4986, 0x8a, 0xcb, 0x46, 0x7b, 0x60, 0x24, 0x7a, 0xc5);
static const uuid_t oem_uuid =
	UUID_INIT(0x884a63a0, 0x3285, 0x4120, 0x83, 0xaa, 0xee, 0xc0, 0x08, 0xa0, 0xa5, 0x46);

/* Exact common prefix of published 0.1.0 and 0.1.1, using target headers.
 * This is deliberately version-pinned diagnostic machinery, not a driver ABI.
 * Never write these fields; retaining the owner's mutex excludes sysfs clients.
 */
struct owner_prefix {
	struct ffa_device *ffa_dev;
	struct thermal_cooling_device *cooling_dev;
	struct notifier_block reboot_notifier;
	struct mutex lock;
	unsigned long last_updated;
	unsigned long current_state;
};

static int match_service(struct device *dev, const void *id)
{
	return uuid_equal(&to_ffa_dev(dev)->uuid, id);
}

static bool valid_service(struct ffa_device *ffa)
{
	return ffa->ops && ffa->ops->info_ops && ffa->ops->info_ops->api_version_get &&
		ffa->ops->msg_ops && ffa->ops->msg_ops->sync_send_receive2 &&
		ffa->ops->info_ops->api_version_get() == FFA_VERSION_1_2 &&
		!ffa->mode_32bit && ffa->vm_id == 0x8003 && ffa->properties == 0x0109;
}

static int poll_packet(struct ffa_device *ffa, u8 *state, u16 *floor)
{
	struct ffa_send_direct_data2 msg = { 0 };
	u8 *raw = (u8 *)msg.data;
	int ret;

	raw[0] = 2;
	ret = ffa->ops->msg_ops->sync_send_receive2(ffa, &msg);
	if (ret)
		return ret;
	*state = raw[0];
	*floor = get_unaligned_le16(raw + 1);
	return *state > 2 ? -EBADMSG : 0;
}

static int read_fixed(struct ffa_device *ffa, u32 address, u8 length, u8 *out)
{
	struct ffa_send_direct_data2 msg = { 0 };
	u8 *raw = (u8 *)msg.data;
	int ret;

	/* Status, response header/payload, and RTC canary only. */
	if (!((address == 0x06000504 && length == 1) ||
	      (address == 0x06000800 && length == 5) ||
	      (address == 0x06000788 && length == 6)))
		return -EPERM;
	raw[0] = 12;
	put_unaligned_le32(address, raw + 1);
	put_unaligned_le32(length, raw + 5);
	ret = ffa->ops->msg_ops->sync_send_receive2(ffa, &msg);
	if (!ret)
		memcpy(out, raw, length);
	return ret;
}

static bool valid_rtc(const u8 *rtc)
{
	static const u8 maxima[] = { 59, 59, 23, 31, 12, 99 };
	unsigned int i;

	for (i = 0; i < 6; i++) {
		if ((rtc[i] & 15) > 9 || (rtc[i] >> 4) > 9 ||
		    (rtc[i] >> 4) * 10 + (rtc[i] & 15) > maxima[i])
			return false;
	}
	return rtc[3] && rtc[4];
}

static int checked_floor(struct ffa_device *packet, struct ffa_device *oem,
			 u16 expected)
{
	struct ffa_send_direct_data2 msg = { 0 };
	u8 *raw = (u8 *)msg.data;
	u8 state, header[5], status;
	u16 floor;
	unsigned int attempt;
	int ret;

	ret = poll_packet(packet, &state, &floor);
	if (ret || state)
		return ret ? ret : -EBUSY;
	raw[0] = 1;
	raw[1] = 4;
	raw[3] = 2;
	ret = packet->ops->msg_ops->sync_send_receive2(packet, &msg);
	if (ret || get_unaligned_le32(raw))
		return ret ? ret : -EREMOTEIO;
	for (attempt = 0; attempt < 20; attempt++) {
		msleep(50);
		ret = poll_packet(packet, &state, &floor);
		if (ret || state != 2)
			break;
	}
	if (ret || state)
		return ret ? ret : -EBUSY;
	ret = read_fixed(oem, 0x06000800, 5, header);
	if (ret)
		return ret;
	ret = read_fixed(oem, 0x06000504, 1, &status);
	if (ret)
		return ret;
	pr_info("dgx_ec_mailbox_probe: checked floor expected=%#x cached=%#x mailbox=%#x response=%*ph\n",
		expected, floor, status, 5, header);
	/* Check both the secure cache and physical response buffer. Shared-mailbox
	 * interference is a refusal, never a reason to relax this ownership check.
	 */
	if ((status & 3) || header[0] != 7 || header[1] != 4 || header[2] ||
	    get_unaligned_le16(header + 3) != expected || floor != expected)
		return -ESTALE;
	return 0;
}

static int restore_automatic(struct ffa_device *packet, struct ffa_device *oem)
{
	struct ffa_send_direct_data2 msg = { 0 };
	u8 *raw = (u8 *)msg.data;
	u8 state;
	u16 floor;
	unsigned int attempt;
	int ret;

	ret = checked_floor(packet, oem, restore_floor);
	if (!ret)
		ret = checked_floor(packet, oem, restore_floor);
	if (ret)
		return ret;
	raw[0] = 1;
	raw[1] = 5; /* The only allowed write is lower-floor UNSET. */
	raw[2] = 2;
	raw[4] = 0xff;
	raw[5] = 0xff;
	ret = packet->ops->msg_ops->sync_send_receive2(packet, &msg);
	if (ret || get_unaligned_le32(raw))
		return ret ? ret : -EREMOTEIO;
	for (attempt = 0; attempt < 20; attempt++) {
		msleep(50);
		ret = poll_packet(packet, &state, &floor);
		if (ret || state != 2)
			break;
	}
	if (ret || state)
		return ret ? ret : -EBUSY;
	ret = checked_floor(packet, oem, 0xffff);
	if (!ret)
		ret = checked_floor(packet, oem, 0xffff);
	if (!ret)
		pr_info("dgx_ec_mailbox_probe: removed operator-confirmed floor=%u; automatic read back twice; owner unchanged\n", restore_floor);
	return ret;
}

static int diagnose(struct ffa_device *packet, struct ffa_device *oem)
{
	u8 before, after, status1, status2, header[5], rtc[6];
	u16 floor;
	unsigned int attempt;
	struct ffa_send_direct_data2 msg = { 0 };
	u8 *raw = (u8 *)msg.data;
	int ret;

	ret = poll_packet(packet, &before, &floor);
	if (ret)
		return ret;
	ret = read_fixed(oem, 0x06000788, 6, rtc);
	if (ret)
		return ret;
	ret = read_fixed(oem, 0x06000504, 1, &status1);
	if (ret)
		return ret;
	ret = read_fixed(oem, 0x06000800, 5, header);
	if (ret)
		return ret;
	msleep(100);
	ret = read_fixed(oem, 0x06000504, 1, &status2);
	if (ret)
		return ret;
	ret = poll_packet(packet, &after, &floor);
	if (ret)
		return ret;
	pr_info("dgx_ec_mailbox_probe: poll=%u->%u mailbox=%#x,%#x rtc=%*ph response=%*ph\n",
		before, after, status1, status2, 6, rtc, 5, header);
	if (restore_floor != 0xffff) {
		if (recover || restore_floor < 2700 || restore_floor > 13500 ||
		    before || after || status1 != status2 || (status2 & 3) || !valid_rtc(rtc))
			return -EAGAIN;
		return restore_automatic(packet, oem);
	}
	if (!recover)
		return 0;
	/* OEM failures may return zeroed data, so an idle-looking byte alone is
	 * insufficient. Require a recognizable packet header and plausible RTC.
	 * The mailbox is shared: the dispatcher also accepts families 0x10..0x14
	 * (0x93977478), so its last response need not be the fan packet.
	 * The packet sender independently enforces the physical mailbox guard.
	 */
	if (before != 2 || after != 2 || status1 != status2 || (status2 & 3) ||
	    !valid_rtc(rtc) ||
	    !((header[0] == 7 && (header[1] == 1 || header[1] == 4 ||
				 header[1] == 5 || header[1] == 7)) ||
	      (header[0] >= 0x10 && header[0] <= 0x14)))
		return -EAGAIN;
	raw[0] = 1;
	raw[1] = 4; /* Read lower floor, never a setter. */
	raw[2] = 0;
	raw[3] = 2;
	ret = packet->ops->msg_ops->sync_send_receive2(packet, &msg);
	if (ret)
		return ret;
	pr_info("dgx_ec_mailbox_probe: one read retry submit status=%#x\n",
		get_unaligned_le32(raw));
	if (get_unaligned_le32(raw))
		return -EREMOTEIO;
	for (attempt = 0; attempt < 20; attempt++) {
		msleep(50);
		ret = poll_packet(packet, &after, &floor);
		if (ret)
			return ret;
		if (after != 2)
			break;
	}
	pr_info("dgx_ec_mailbox_probe: read retry poll=%u floor=%#x (owner unchanged)\n", after, floor);
	return after == 0 ? 0 : after == 2 ? -ETIMEDOUT : -EREMOTEIO;
}

static int __init mailbox_init(void)
{
	struct device *packet_dev, *oem_dev;
	struct owner_prefix *owner;
	struct module *module;
	int ret = -ENODEV;

	if (!dmi_match(DMI_SYS_VENDOR, "NVIDIA") ||
	    !dmi_match(DMI_PRODUCT_NAME, "NVIDIA_DGX_Spark") ||
	    !dmi_match(DMI_BOARD_NAME, "P4242"))
		return -ENODEV;
	packet_dev = bus_find_device(&ffa_bus_type, NULL, &packet_uuid, match_service);
	if (!packet_dev)
		return -ENODEV;
	oem_dev = bus_find_device(&ffa_bus_type, NULL, &oem_uuid, match_service);
	if (!oem_dev)
		goto put_packet;
	device_lock(packet_dev);
	if (!packet_dev->driver || strcmp(packet_dev->driver->name, "dgx-ec-fan-control"))
		goto unlock_device;
	module = packet_dev->driver->owner;
	if (!module || !module->version || !module->srcversion ||
	    !((!strcmp(module->version, "0.1.0") &&
	       (!strcmp(module->srcversion, "FFB84C2FB9D4979B29876AD") ||
		!strcmp(module->srcversion, "7BEC2E2D9DF9895B99E207A"))) ||
	      (!strcmp(module->version, "0.1.1") &&
	       (!strcmp(module->srcversion, "81A2F2BDE37A05AD960065A") ||
		!strcmp(module->srcversion, "340672C7AB99E30FB20D4A3")))))
		goto unlock_device;
	owner = dev_get_drvdata(packet_dev);
	if (!owner || owner->ffa_dev != to_ffa_dev(packet_dev) || !owner->cooling_dev ||
	    owner->cooling_dev->devdata != owner ||
	    !valid_service(to_ffa_dev(packet_dev)) || !valid_service(to_ffa_dev(oem_dev)))
		goto unlock_device;
	if (!try_module_get(module))
		goto unlock_device;
	mutex_lock(&owner->lock);
	pr_info("dgx_ec_mailbox_probe: locked owner version=%s confirmed_state=%lu recover=%u\n",
		module->version, owner->current_state, recover);
	ret = owner->current_state > 12 ? -EINVAL :
		diagnose(to_ffa_dev(packet_dev), to_ffa_dev(oem_dev));
	mutex_unlock(&owner->lock);
	module_put(module);
unlock_device:
	device_unlock(packet_dev);
	put_device(oem_dev);
put_packet:
	put_device(packet_dev);
	pr_info("dgx_ec_mailbox_probe: finished ret=%d\n", ret);
	return ret;
}

static void __exit mailbox_exit(void) {}
module_init(mailbox_init);
module_exit(mailbox_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("One-shot, fixed-address DGX Spark mailbox diagnosis and guarded read retry");
MODULE_VERSION("0.1.0");
