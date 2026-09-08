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
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/reboot.h>
#include <linux/sched.h>
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
#define DGX_EC_RECOVERY_COOLDOWN_MS	30000U
#define DGX_EC_RECOVERY_OBSERVE_MS	100U
#define DGX_EC_OEM_READ_COMMAND		12U
#define DGX_EC_MAILBOX_STATUS		0x06000504U
#define DGX_EC_MAILBOX_RESPONSE		0x06000800U
#define DGX_EC_RTC			0x06000788U
#define DGX_EC_TRACE_LENGTH		16U
#define DGX_EC_DRIVER_VERSION		"0.1.3"

static const uuid_t dgx_ec_packet_uuid =
	UUID_INIT(0x78b04d80, 0xd21d, 0x4986,
		  0x8a, 0xcb, 0x46, 0x7b, 0x60, 0x24, 0x7a, 0xc5);

static const uuid_t dgx_ec_oem_uuid =
	UUID_INIT(0x884a63a0, 0x3285, 0x4120,
		  0x83, 0xaa, 0xee, 0xc0, 0x08, 0xa0, 0xa5, 0x46);

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

/* Fixed-size, in-memory history only. Printed at a paced recovery incident. */
struct dgx_ec_trace {
	u64 seq, start_ns, submit_ns, end_ns;
	u32 polls, pending;
	int pid, cpu, transport, poll_transport, sp_status, result;
	u16 floor;
	u8 operation, input_length, output_length, first_poll, last_poll;
	bool submitted;
};

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
	bool recovery_attempted;
	bool recovery_unverified;
	unsigned long next_recovery;
	u32 recovery_count;
	u64 incident, trace_seq, submissions[8], last_write_ns;
	u8 requested_operation;
	const char *request_phase;
	struct dgx_ec_trace trace[DGX_EC_TRACE_LENGTH];
};

static void dgx_ec_trace_begin(struct dgx_ec_fan_control_data *data,
			       u8 operation, bool submitted)
{
	struct dgx_ec_trace *t = &data->trace[data->trace_seq % DGX_EC_TRACE_LENGTH];

	memset(t, 0, sizeof(*t));
	t->seq = ++data->trace_seq;
	t->start_ns = ktime_get_ns();
	t->pid = task_pid_nr(current);
	t->cpu = raw_smp_processor_id();
	t->operation = operation;
	t->submitted = submitted;
	t->sp_status = -1; /* Not observed, including transport failures. */
	t->result = -EINPROGRESS;
	t->first_poll = t->last_poll = 0xff;
}

static void dgx_ec_log_trace(struct dgx_ec_fan_control_data *data,
			     const struct dgx_ec_trace *t)
{
	dev_warn(&data->ffa_dev->dev,
		"incident=%llu tx=%llu phase=%s op=%#x in=%u out=%u floor=%#x pid=%d cpu=%d start_ns=%llu submit_us=%llu elapsed_us=%llu transport=%d sp=%d polls=%u pending=%u first=%#x last=%#x poll_transport=%d result=%d\n",
		data->incident, t->seq, t->submitted ? "submit" : "preflight",
		t->operation, t->input_length, t->output_length, t->floor,
		t->pid, t->cpu, t->start_ns, t->submit_ns / 1000,
		(t->end_ns - t->start_ns) / 1000, t->transport, t->sp_status,
		t->polls, t->pending, t->first_poll, t->last_poll, t->poll_transport, t->result);
}

static void dgx_ec_log_incident(struct dgx_ec_fan_control_data *data)
{
	u64 now = ktime_get_ns();
	u64 first = data->trace_seq > DGX_EC_TRACE_LENGTH ?
		data->trace_seq - DGX_EC_TRACE_LENGTH : 0;
	u64 seq;

	dev_warn(&data->ffa_dev->dev,
		"incident=%llu begin driver=%s op=%#x phase=%s mono_ns=%llu confirmed_state=%lu attempted_floor=%#x uncertain=%u unverified=%u telemetry_valid=%u cached_rpm=%u,%u telemetry_age_ms=%u\n",
		data->incident, DGX_EC_DRIVER_VERSION, data->requested_operation, data->request_phase,
		now, data->current_state, data->attempted_floor, data->floor_uncertain,
		data->recovery_unverified, data->telemetry_valid, data->rpm[0], data->rpm[1],
		jiffies_to_msecs(jiffies - data->last_updated));
	dev_warn(&data->ffa_dev->dev,
		"incident=%llu totals caps=%llu floor_reads=%llu floor_writes=%llu telemetry=%llu last_write_attempt_age_ms=%llu recoveries=%u cooldown_ms=%u\n",
		data->incident, data->submissions[1], data->submissions[4],
		data->submissions[5], data->submissions[7],
		data->submissions[5] ? (now - data->last_write_ns) / 1000000 : 0,
		data->recovery_count, DGX_EC_RECOVERY_COOLDOWN_MS);
	for (seq = first; seq < data->trace_seq; seq++)
		dgx_ec_log_trace(data, &data->trace[seq % DGX_EC_TRACE_LENGTH]);
}

static bool dgx_ec_is_supported_platform(void)
{
	return dmi_match(DMI_SYS_VENDOR, "NVIDIA") &&
	       dmi_match(DMI_PRODUCT_NAME, "NVIDIA_DGX_Spark") &&
	       dmi_match(DMI_BOARD_NAME, "P4242");
}

static int dgx_ec_validate_transport(struct ffa_device *ffa_dev,
				     const uuid_t *uuid)
{
	u32 ffa_version;

	if (!uuid_equal(&ffa_dev->uuid, uuid))
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

static int dgx_ec_submit_status(struct dgx_ec_fan_control_data *data,
				struct ffa_send_direct_data2 *message)
{
	struct ffa_device *ffa_dev = data->ffa_dev;
	struct dgx_ec_trace *t;
	u8 *raw = (u8 *)message->data;
	u8 operation = raw[1];
	int ret;

	data->request_phase = "completion";
	dgx_ec_trace_begin(data, operation, true);
	t = &data->trace[(data->trace_seq - 1) % DGX_EC_TRACE_LENGTH];
	t->input_length = raw[2];
	t->output_length = raw[3];
	if (operation < ARRAY_SIZE(data->submissions))
		data->submissions[operation]++;
	if (operation == DGX_EC_FAN_SET_LOWER_LIMIT) {
		t->floor = get_unaligned_le16(raw + 4);
		data->last_write_ns = t->start_ns;
	}
	ret = ffa_dev->ops->msg_ops->sync_send_receive2(ffa_dev, message);
	t->end_ns = ktime_get_ns();
	t->submit_ns = t->end_ns - t->start_ns;
	t->transport = ret;
	t->result = ret;
	if (ret) {
		dev_warn_ratelimited(&ffa_dev->dev,
			"FF-A submit operation=%#x failed: %d\n",
			operation, ret);
		return ret;
	}

	ret = get_unaligned_le32(raw);
	t->sp_status = ret;
	if (ret)
		dev_warn_ratelimited(&ffa_dev->dev,
			"packet submit operation=%#x status=%#x\n",
			operation, ret);
	if (ret == DGX_EC_PACKET_SP_ESPI_READ_FAILED)
		ret = -EIO;
	else if (ret == DGX_EC_PACKET_SP_MAILBOX_BUSY)
		ret = -EBUSY;
	else if (ret)
		ret = -EREMOTEIO;
	t->result = ret;
	return ret;
}

static int dgx_ec_wait_for_completion(struct dgx_ec_fan_control_data *data,
				      void *output, size_t output_length)
{
	struct ffa_device *ffa_dev = data->ffa_dev;
	struct dgx_ec_trace *t = &data->trace[(data->trace_seq - 1) % DGX_EC_TRACE_LENGTH];
	unsigned int attempt;
	u8 state = 0xff;
	int ret;

	for (attempt = 0; attempt < DGX_EC_PACKET_POLL_ATTEMPTS; attempt++) {
		ret = dgx_ec_packet_poll(ffa_dev, &state, output, output_length);
		t->poll_transport = ret;
		t->polls++;
		if (ret)
			goto done;
		if (t->polls == 1)
			t->first_poll = state;
		t->last_poll = state;
		if (state == DGX_EC_PACKET_COMPLETE)
			goto done;
		if (state == DGX_EC_PACKET_EC_ERROR) {
			ret = -EREMOTEIO;
			goto done;
		}
		if (state != DGX_EC_PACKET_PENDING) {
			ret = -EBADMSG;
			goto done;
		}
		t->pending++;
		msleep(DGX_EC_PACKET_POLL_DELAY_MS);
	}

	dev_warn_ratelimited(&ffa_dev->dev,
		"packet poll timed out: tx=%llu op=%#x phase=%s state=%#x after %u polls\n",
		t->seq, t->operation, t->submitted ? "completion" : "preflight",
		state, DGX_EC_PACKET_POLL_ATTEMPTS);
	ret = -ETIMEDOUT;
done:
	t->result = ret;
	t->end_ns = ktime_get_ns();
	return ret;
}

static int dgx_ec_reconcile_floor(struct dgx_ec_fan_control_data *data, u16 floor);

static int dgx_ec_match_oem(struct device *dev, const void *unused)
{
	return uuid_equal(&to_ffa_dev(dev)->uuid, &dgx_ec_oem_uuid);
}

/* The device reference survives unplug; its lock excludes binding/removal for
 * the entire recovery. Never borrow an OEM service owned by another driver.
 * Callers already hold the packet owner's transaction mutex.
 */
static struct ffa_device *dgx_ec_recovery_peer_get(struct dgx_ec_fan_control_data *data)
{
	struct device *dev;
	int ret;

	dev = bus_find_device(data->ffa_dev->dev.bus, NULL, NULL, dgx_ec_match_oem);
	if (!dev)
		return ERR_PTR(-ENODEV);
	if (!device_trylock(dev)) {
		put_device(dev);
		return ERR_PTR(-EBUSY);
	}
	ret = !device_is_registered(dev) || dev->driver ? -EBUSY :
		dgx_ec_validate_transport(to_ffa_dev(dev), &dgx_ec_oem_uuid);
	if (ret) {
		device_unlock(dev);
		put_device(dev);
		return ERR_PTR(ret);
	}
	return to_ffa_dev(dev);
}

static void dgx_ec_recovery_peer_put(struct ffa_device *oem)
{
	device_unlock(&oem->dev);
	put_device(&oem->dev);
}

static int dgx_ec_read_fixed(struct ffa_device *oem, u32 address,
			     u8 length, u8 *output)
{
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	int ret;

	/* No acknowledgement reads, writes, arbitrary addresses, or public ABI. */
	if (!((address == DGX_EC_MAILBOX_STATUS && length == 1) ||
	      (address == DGX_EC_MAILBOX_RESPONSE && length == 5) ||
	      (address == DGX_EC_RTC && length == 6)))
		return -EPERM;
	raw[0] = DGX_EC_OEM_READ_COMMAND;
	put_unaligned_le32(address, raw + 1);
	put_unaligned_le32(length, raw + 5);
	ret = oem->ops->msg_ops->sync_send_receive2(oem, &message);
	if (!ret)
		memcpy(output, raw, length);
	return ret;
}

static int dgx_ec_valid_rtc(const u8 *rtc)
{
	static const u8 maxima[] = { 59, 59, 23, 31, 12, 99 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(maxima); i++) {
		if ((rtc[i] & 15) > 9 || (rtc[i] >> 4) > 9 ||
		    (rtc[i] >> 4) * 10 + (rtc[i] & 15) > maxima[i])
			return 0;
	}
	return rtc[3] && rtc[4];
}

/* Exactly one read, with no preflight or recursive recovery. The only caller
 * allowed to bypass cached pending establishes physical idle independently.
 */
static int dgx_ec_submit_read(struct dgx_ec_fan_control_data *data, u8 operation,
			      size_t output_length, void *output)
{
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	int ret;

	raw[0] = DGX_EC_PACKET_SUBMIT_COMMAND;
	raw[1] = operation;
	raw[3] = output_length;
	ret = dgx_ec_submit_status(data, &message);
	if (ret)
		return ret;
	return dgx_ec_wait_for_completion(data, output, output_length);
}

static int dgx_ec_recover_idle(struct dgx_ec_fan_control_data *data,
			       struct ffa_device *oem, u16 *floor)
{
	u8 before = 0xff, after = 0xff, status1 = 0xff, status2 = 0xff, final_status = 0xff;
	u8 rtc[6] = { 0 }, header[5] = { 0 }, final_header[5] = { 0 }, reply[2] = { 0 };
	u32 valid = 0;
	u64 seq = data->trace_seq;
	const char *stage = "poll-before";
	int ret;

	ret = dgx_ec_packet_poll(data->ffa_dev, &before, NULL, 0);
	if (ret)
		goto out;
	valid |= 1U << 0;
	if (before != DGX_EC_PACKET_PENDING && before != DGX_EC_PACKET_COMPLETE) {
		ret = -EBADMSG;
		goto out;
	}
	stage = "rtc";
	ret = dgx_ec_read_fixed(oem, DGX_EC_RTC, sizeof(rtc), rtc);
	if (ret)
		goto out;
	valid |= 1U << 1;
	stage = "status-before";
	ret = dgx_ec_read_fixed(oem, DGX_EC_MAILBOX_STATUS, 1, &status1);
	if (ret)
		goto out;
	valid |= 1U << 2;
	stage = "response-before";
	ret = dgx_ec_read_fixed(oem, DGX_EC_MAILBOX_RESPONSE, sizeof(header), header);
	if (ret)
		goto out;
	valid |= 1U << 3;
	msleep(DGX_EC_RECOVERY_OBSERVE_MS);
	stage = "status-after";
	ret = dgx_ec_read_fixed(oem, DGX_EC_MAILBOX_STATUS, 1, &status2);
	if (ret)
		goto out;
	valid |= 1U << 4;
	stage = "poll-after";
	ret = dgx_ec_packet_poll(data->ffa_dev, &after, NULL, 0);
	if (ret)
		goto out;
	valid |= 1U << 5;
	stage = "idle-guard";
	/* OEM errors can masquerade as zero bytes. Require independent canaries.
	 * Reads may drain a late completion, so pending -> complete is admissible.
	 * The sender also checks mailbox busy at submission, closing that race.
	 */
	if ((after != DGX_EC_PACKET_PENDING && after != DGX_EC_PACKET_COMPLETE) ||
	    status1 != status2 || (status2 & 3) || !dgx_ec_valid_rtc(rtc) ||
	    !((header[0] == 7 && (header[1] == 1 || header[1] == 4 ||
				 header[1] == 5 || header[1] == 7)) ||
	      (header[0] >= 0x10 && header[0] <= 0x14))) {
		ret = -EAGAIN;
		goto out;
	}

	stage = "recovery-read";
	ret = dgx_ec_submit_read(data, DGX_EC_FAN_GET_LOWER_LIMIT,
				 sizeof(reply), reply);
	if (ret)
		goto out;
	valid |= 1U << 6;
	stage = "response-final";
	ret = dgx_ec_read_fixed(oem, DGX_EC_MAILBOX_RESPONSE, sizeof(final_header), final_header);
	if (ret)
		goto out;
	valid |= 1U << 7;
	stage = "status-final";
	ret = dgx_ec_read_fixed(oem, DGX_EC_MAILBOX_STATUS, 1, &final_status);
	if (ret)
		goto out;
	valid |= 1U << 8;
	stage = "reply-guard";
	/* A shared response overwritten by another service is a refusal. Cached
	 * success alone cannot authenticate a reply after firmware read failure.
	 */
	if ((final_status & 3) || final_header[0] != 7 || final_header[1] != 4 || final_header[2] ||
	    memcmp(reply, final_header + 3, sizeof(reply))) {
		ret = -ESTALE;
		goto out;
	}
	*floor = get_unaligned_le16(reply);
	stage = "ownership";
	ret = dgx_ec_reconcile_floor(data, *floor);
out:
	/* All fields are initialized; the mask marks successful boundary reads,
	 * not trustworthy content. Log existing observations only, with no new I/O.
	 */
	dev_warn(&data->ffa_dev->dev,
		"incident=%llu observation stage=%s ret=%d valid=%#x poll=%#x,%#x mailbox=%#x,%#x,%#x rtc=%02x:%02x:%02x:%02x:%02x:%02x\n",
		data->incident, stage, ret, valid, before, after, status1, status2, final_status,
		rtc[0], rtc[1], rtc[2], rtc[3], rtc[4], rtc[5]);
	dev_warn(&data->ffa_dev->dev,
		"incident=%llu response_before=%02x:%02x:%02x:%02x:%02x response_final=%02x:%02x:%02x:%02x:%02x cached_floor=%#x\n",
		data->incident, header[0], header[1], header[2], header[3], header[4],
		final_header[0], final_header[1], final_header[2], final_header[3], final_header[4],
		get_unaligned_le16(reply));
	if (data->trace_seq != seq)
		dgx_ec_log_trace(data, &data->trace[(data->trace_seq - 1) % DGX_EC_TRACE_LENGTH]);
	return ret;
}

static int dgx_ec_recover_pending(struct dgx_ec_fan_control_data *data, u16 *floor)
{
	struct ffa_device *oem;
	u64 started;
	int ret;

	if (data->recovery_attempted && time_before(jiffies, data->next_recovery)) {
		data->telemetry_valid = false;
		return -ETIMEDOUT;
	}
	data->recovery_attempted = true;
	data->next_recovery = jiffies + msecs_to_jiffies(DGX_EC_RECOVERY_COOLDOWN_MS);
	started = ktime_get_ns();
	data->incident++;
	dgx_ec_log_incident(data);
	data->telemetry_valid = false;
	oem = dgx_ec_recovery_peer_get(data);
	if (IS_ERR(oem)) {
		dev_warn(&data->ffa_dev->dev,
			"incident=%llu end stage=oem-service ret=%ld elapsed_us=%llu\n",
			data->incident, PTR_ERR(oem), (ktime_get_ns() - started) / 1000);
		return -ETIMEDOUT;
	}
	/* A failed physical cross-check must not be bypassed by a later caller
	 * accepting the same unauthenticated cache as an ordinary completion.
	 */
	data->recovery_unverified = true;
	ret = dgx_ec_recover_idle(data, oem, floor);
	dgx_ec_recovery_peer_put(oem);
	dev_warn(&data->ffa_dev->dev,
		"incident=%llu end ret=%d elapsed_us=%llu\n",
		data->incident, ret, (ktime_get_ns() - started) / 1000);
	if (ret)
		dev_warn_ratelimited(&data->ffa_dev->dev,
			"pending recovery refused/failed: %d; cooldown %u ms\n",
			ret, DGX_EC_RECOVERY_COOLDOWN_MS);
	else {
		data->recovery_unverified = false;
		data->recovery_count++;
		dev_warn_ratelimited(&data->ffa_dev->dev,
			"recovered idle-mailbox pending: floor=%#x state=%lu count=%u\n",
			*floor, data->current_state, data->recovery_count);
	}
	return ret;
}

static int dgx_ec_preflight(struct dgx_ec_fan_control_data *data, u8 operation)
{
	u16 floor;
	int ret;

	data->requested_operation = operation;
	data->request_phase = "blocked";
	if (data->recovery_unverified)
		return dgx_ec_recover_pending(data, &floor);

	data->request_phase = "preflight";
	dgx_ec_trace_begin(data, operation, false);
	/* Drain a late completion before another request can reuse the relay. */
	ret = dgx_ec_wait_for_completion(data, NULL, 0);
	if (ret == -ETIMEDOUT)
		ret = dgx_ec_recover_pending(data, &floor);
	if (ret)
		dev_warn_ratelimited(&data->ffa_dev->dev,
			"operation=%#x preflight failed: %d; original request not submitted\n",
			operation, ret);
	return ret;
}

static int dgx_ec_read_operation(struct dgx_ec_fan_control_data *data, u8 operation,
				 void *output)
{
	size_t output_length;
	u16 floor;
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

	ret = dgx_ec_preflight(data, operation);
	if (ret)
		return ret;

	ret = dgx_ec_submit_read(data, operation, output_length, output);
	if (ret != -ETIMEDOUT)
		return ret;
	ret = dgx_ec_recover_pending(data, &floor);
	if (ret)
		return ret;
	if (operation == DGX_EC_FAN_GET_LOWER_LIMIT) {
		put_unaligned_le16(floor, output);
		return 0;
	}
	/* Capabilities/telemetry must be read anew; never reinterpret the recovery
	 * floor as the original operation's response. One retry, no recursion.
	 */
	return dgx_ec_submit_read(data, operation, output_length, output);
}

static int dgx_ec_write_lower_floor(struct dgx_ec_fan_control_data *data,
				  u16 value)
{
	struct ffa_send_direct_data2 message = { 0 };
	u8 *raw = (u8 *)message.data;
	u16 floor;
	int ret;

	ret = dgx_ec_preflight(data, DGX_EC_FAN_SET_LOWER_LIMIT);
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
	ret = dgx_ec_submit_status(data, &message);
	if (ret)
		return ret;

	ret = dgx_ec_wait_for_completion(data, NULL, 0);
	if (ret != -ETIMEDOUT)
		return ret;
	ret = dgx_ec_recover_pending(data, &floor);
	if (ret)
		return ret;
	/* Never replay the setter: the first write may already have taken effect. */
	return floor == value ? 0 : -EIO;
}

static int dgx_ec_read_lower_floor(struct dgx_ec_fan_control_data *data,
				   u16 *floor)
{
	u8 limit_data[DGX_EC_FAN_LIMIT_LENGTH];
	int ret;

	ret = dgx_ec_read_operation(data,
				    DGX_EC_FAN_GET_LOWER_LIMIT, limit_data);
	if (!ret)
		*floor = get_unaligned_le16(limit_data);

	return ret;
}

static int dgx_ec_read_capabilities(struct dgx_ec_fan_control_data *data)
{
	struct dgx_ec_fan_capabilities capabilities;
	int ret;

	ret = dgx_ec_read_operation(data,
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

	ret = dgx_ec_read_operation(data,
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
	ret = dgx_ec_validate_transport(ffa_dev, &dgx_ec_packet_uuid);
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
MODULE_VERSION(DGX_EC_DRIVER_VERSION);
