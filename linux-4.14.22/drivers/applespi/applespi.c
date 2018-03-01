/*
 * MacBook (Pro) SPI keyboard and touchpad driver
 *
 * Copyright (c) 2015-2018 Federico Lorenzi
 * Copyright (c) 2017-2018 Ronald Tschalär
 *
 * SPDX-License-Identifier: GPL-2.0
 */

/**
 * The keyboard and touchpad controller on the MacBook8,1, MacBook9,1 and
 * MacBookPro12,1 can be driven either by USB or SPI. However the USB pins
 * are only connected on the MacBookPro12,1, all others need this driver.
 * The interface is selected using ACPI methods:
 *
 * * UIEN ("USB Interface Enable"): If invoked with argument 1, disables SPI
 *   and enables USB. If invoked with argument 0, disables USB.
 * * UIST ("USB Interface Status"): Returns 1 if USB is enabled, 0 otherwise.
 * * SIEN ("SPI Interface Enable"): If invoked with argument 1, disables USB
 *   and enables SPI. If invoked with argument 0, disables SPI.
 * * SIST ("SPI Interface Status"): Returns 1 if SPI is enabled, 0 otherwise.
 * * ISOL: Resets the four GPIO pins used for SPI. Intended to be invoked with
 *   argument 1, then once more with argument 0.
 *
 * UIEN and UIST are only provided on the MacBookPro12,1.
 *
 * SPI-based Protocol
 * ------------------
 *
 * The device and driver exchange messages (struct message); each message is
 * encapsulated in one or more packets (struct spi_packet). There are two types
 * of exchanges: reads, and writes. A read is signaled by a GPE, upon which one
 * message can be read from the device. A write exchange consists of writing a
 * command message, immediately reading a short status packet, and then, upon
 * receiving a GPE, reading the response messsage. Write exchanges cannot be
 * interleaved, i.e. a new write exchange must not be started till the previous
 * write exchange is complete. Whether a received message is part of a read or
 * write exchange is indicated in the encapsulating packet's flags field.
 *
 * A single message may be too large to fit in a single packet (which has a
 * fixed, 256-byte size). In that case it will be split over multiple,
 * consecutive packets.
 */

#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/spinlock.h>
#include <linux/crc16.h>
#include <linux/wait.h>
#include <linux/leds.h>
#include <linux/ktime.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-polldev.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#define PRE_SPI_PROPERTIES
#endif

#ifdef PRE_SPI_PROPERTIES
#include <linux/workqueue.h>
#include <linux/notifier.h>
#endif

#define APPLESPI_PACKET_SIZE	256
#define APPLESPI_STATUS_SIZE	4

#define PACKET_TYPE_READ	0x20
#define PACKET_TYPE_WRITE	0x40
#define PACKET_DEV_KEYB		0x01
#define PACKET_DEV_TPAD		0x02

#define MAX_ROLLOVER		6
#define MAX_MODIFIERS		8

#define MAX_FINGERS		11
#define MAX_FINGER_ORIENTATION	16384
#define MAX_PKTS_PER_MSG	2

#define MIN_KBD_BL_LEVEL	32
#define MAX_KBD_BL_LEVEL	255
#define KBD_BL_LEVEL_SCALE	1000000
#define KBD_BL_LEVEL_ADJ	\
	((MAX_KBD_BL_LEVEL - MIN_KBD_BL_LEVEL) * KBD_BL_LEVEL_SCALE / 255)

#define DBG_CMD_TP_INI		BIT(0)
#define DBG_CMD_BL		BIT(1)
#define DBG_CMD_CL		BIT(2)
#define DBG_RD_KEYB		BIT(8)
#define DBG_RD_TPAD		BIT(9)
#define DBG_RD_UNKN		BIT(10)
#define DBG_RD_IRQ		BIT(11)
#define DBG_TP_DIM		BIT(16)

#define	debug_print(mask, fmt, ...) \
	do { \
		if (debug & mask) \
			printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); \
	} while (0)
#define	debug_print_buffer(mask, fmt, ...) \
	do { \
		if (debug & mask) \
			print_hex_dump(KERN_DEBUG, pr_fmt(fmt), \
				       DUMP_PREFIX_NONE, 32, 1, ##__VA_ARGS__, \
				       false); \
	} while (0)

#define APPLE_FLAG_FKEY		0x01

#define SPI_RW_CHG_DLY		100	/* from experimentation, in us */

static unsigned int fnmode = 1;
module_param(fnmode, uint, 0644);
MODULE_PARM_DESC(fnmode, "Mode of fn key on Apple keyboards (0 = disabled, [1] = fkeyslast, 2 = fkeysfirst)");

static unsigned int iso_layout;
module_param(iso_layout, uint, 0644);
MODULE_PARM_DESC(iso_layout, "Enable/Disable hardcoded ISO-layout of the keyboard. ([0] = disabled, 1 = enabled)");

static unsigned int debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "Enable/Disable debug logging. This is a bitmask.");

static int touchpad_dimensions[4];
module_param_array(touchpad_dimensions, int, NULL, 0444);
MODULE_PARM_DESC(touchpad_dimensions, "The pixel dimensions of the touchpad, as x_min,x_max,y_min,y_max .");

/**
 * struct keyboard_protocol - keyboard message.
 * message.type = 0x0110, message.length = 0x000a
 *
 * @unknown1:		unknown
 * @modifiers:		bit-set of modifier/control keys pressed
 * @unknown2:		unknown
 * @keys_pressed:	the (non-modifier) keys currently pressed
 * @fn_pressed:		whether the fn key is currently pressed
 * @crc_16:		crc over the whole message struct (message header +
 *			this struct) minus this @crc_16 field
 */
struct keyboard_protocol {
	__u8			unknown1;
	__u8			modifiers;
	__u8			unknown2;
	__u8			keys_pressed[MAX_ROLLOVER];
	__u8			fn_pressed;
	__le16			crc_16;
};

/**
 * struct tp_finger - single trackpad finger structure, le16-aligned
 *
 * @origin:		zero when switching track finger
 * @abs_x:		absolute x coodinate
 * @abs_y:		absolute y coodinate
 * @rel_x:		relative x coodinate
 * @rel_y:		relative y coodinate
 * @tool_major:		tool area, major axis
 * @tool_minor:		tool area, minor axis
 * @orientation:	16384 when point, else 15 bit angle
 * @touch_major:	touch area, major axis
 * @touch_minor:	touch area, minor axis
 * @unused:		zeros
 * @pressure:		pressure on forcetouch touchpad
 * @multi:		one finger: varies, more fingers: constant
 * @crc_16:		on last finger: crc over the whole message struct
 *			(i.e. message header + this struct) minus the last
 *			@crc_16 field; unknown on all other fingers.
 */
struct tp_finger {
	__le16 origin;
	__le16 abs_x;
	__le16 abs_y;
	__le16 rel_x;
	__le16 rel_y;
	__le16 tool_major;
	__le16 tool_minor;
	__le16 orientation;
	__le16 touch_major;
	__le16 touch_minor;
	__le16 unused[2];
	__le16 pressure;
	__le16 multi;
	__le16 crc_16;
};

/**
 * struct touchpad_protocol - touchpad message.
 * message.type = 0x0210
 *
 * @unknown1:		unknown
 * @clicked:		1 if a button-click was detected, 0 otherwise
 * @unknown2:		unknown
 * @number_of_fingers:	the number of fingers being reported in @fingers
 * @clicked2:		same as @clicked
 * @unknown3:		unknown
 * @fingers:		the data for each finger
 */
struct touchpad_protocol {
	__u8			unknown1[1];
	__u8			clicked;
	__u8			unknown2[28];
	__u8			number_of_fingers;
	__u8			clicked2;
	__u8			unknown3[16];
	struct tp_finger	fingers[0];
};

/**
 * struct command_protocol_init - initialize touchpad.
 * message.type = 0x0252, message.length = 0x0002
 *
 * @cmd:		value: 0x0102
 * @crc_16:		crc over the whole message struct (message header +
 *			this struct) minus this @crc_16 field
 */
struct command_protocol_init {
	__le16			cmd;
	__le16			crc_16;
};

/**
 * struct command_protocol_capsl - toggle caps-lock led
 * message.type = 0x0151, message.length = 0x0002
 *
 * @unknown:		value: 0x01 (length?)
 * @led:		0 off, 2 on
 * @crc_16:		crc over the whole message struct (message header +
 *			this struct) minus this @crc_16 field
 */
struct command_protocol_capsl {
	__u8			unknown;
	__u8			led;
	__le16			crc_16;
};

/**
 * struct command_protocol_bl - set keyboard backlight brightness
 * message.type = 0xB051, message.length = 0x0006
 *
 * @const1:		value: 0x01B0
 * @level:		the brightness level to set
 * @const2:		value: 0x0001 (backlight off), 0x01F4 (backlight on)
 * @crc_16:		crc over the whole message struct (message header +
 *			this struct) minus this @crc_16 field
 */
struct command_protocol_bl {
	__le16			const1;
	__le16			level;
	__le16			const2;
	__le16			crc_16;
};

/**
 * struct message - a complete spi message.
 *
 * Each message begins with fixed header, followed by a message-type specific
 * payload, and ends with a 16-bit crc. Because of the varying lengths of the
 * payload, the crc is defined at the end of each payload struct, rather than
 * in this struct.
 *
 * @type:	the message type
 * @zero:	always 0
 * @counter:	incremented on each message, rolls over after 255; there is a
 *		separate counter for each message type.
 * @rsp_buf_len:response buffer length (the exact nature of this field is quite
 *		speculative). On a request/write this is often the same as
 *		@length, though in some cases it has been seen to be much larger
 *		(e.g. 0x400); on a response/read this the same as on the
 *		request; for reads that are not responses it is 0.
 * @length:	length of the remainder of the data in the whole message
 *		structure (after re-assembly in case of being split over
 *		multiple spi-packets), minus the trailing crc. The total size
 *		of the message struct is therefore @length + 10.
 */
struct message {
	__le16		type;
	__u8		zero;
	__u8		counter;
	__le16		rsp_buf_len;
	__le16		length;
	union {
		struct keyboard_protocol	keyboard;
		struct touchpad_protocol	touchpad;
		struct command_protocol_init	init_command;
		struct command_protocol_capsl	capsl_command;
		struct command_protocol_bl	bl_command;
		__u8				data[0];
	};
} __packed __aligned(2);

/* type + zero + counter + rsp_buf_len + length */
#define MSG_HEADER_SIZE	8

/**
 * struct spi_packet - a complete spi packet; always 256 bytes. This carries
 * the (parts of the) message in the data. But note that this does not
 * necessarily contain a complete message, as in some cases (e.g. many
 * fingers pressed) the message is split over multiple packets (see the
 * @offset, @remaining, and @length fields). In general the data parts in
 * spi_packet's are concatenated until @remaining is 0, and the result is an
 * message.
 *
 * @flags:	0x40 = write (to device), 0x20 = read (from device); note that
 *		the response to a write still has 0x40.
 * @device:	1 = keyboard, 2 = touchpad
 * @offset:	specifies the offset of this packet's data in the complete
 *		message; i.e. > 0 indicates this is a continuation packet (in
 *		the second packet for a message split over multiple packets
 *		this would then be the same as the @length in the first packet)
 * @remaining:	number of message bytes remaining in subsequents packets (in
 *		the first packet of a message split over two packets this would
 *		then be the same as the @length in the second packet)
 * @length:	length of the valid data in the @data in this packet
 * @data:	all or part of a message
 * @crc_16:	crc over this whole structure minus this @crc_16 field. This
 *		covers just this packet, even on multi-packet messages (in
 *		contrast to the crc in the message).
 */
struct spi_packet {
	__u8			flags;
	__u8			device;
	__le16			offset;
	__le16			remaining;
	__le16			length;
	__u8			data[246];
	__le16			crc_16;
} __packed __aligned(2);

struct spi_settings {
#ifdef PRE_SPI_PROPERTIES
	u64	spi_sclk_period;	/* period in ns */
	u64	spi_word_size;		/* in number of bits */
	u64	spi_bit_order;		/* 1 = MSB_FIRST, 0 = LSB_FIRST */
	u64	spi_spo;		/* clock polarity: 0 = low, 1 = high */
	u64	spi_sph;		/* clock phase: 0 = first, 1 = second */
#endif
	u64	spi_cs_delay;		/* cs-to-clk delay in us */
	u64	reset_a2r_usec;		/* active-to-receive delay? */
	u64	reset_rec_usec;		/* ? (cur val: 10) */
};

struct applespi_tp_info {
	int	x_min;
	int	x_max;
	int	y_min;
	int	y_max;
};

struct applespi_data {
	struct spi_device		*spi;
	struct spi_settings		spi_settings;
	struct input_dev		*keyboard_input_dev;
	struct input_dev		*touchpad_input_dev;

	u8				*tx_buffer;
	u8				*tx_status;
	u8				*rx_buffer;

	u8				*msg_buf;
	unsigned int			saved_msg_len;

	struct applespi_tp_info		tp_info;

	u8				last_keys_pressed[MAX_ROLLOVER];
	u8				last_keys_fn_pressed[MAX_ROLLOVER];
	u8				last_fn_pressed;
	struct input_mt_pos		pos[MAX_FINGERS];
	int				slots[MAX_FINGERS];
	acpi_handle			handle;
	int				gpe;
	acpi_handle			sien;
	acpi_handle			sist;

	struct spi_transfer		dl_t;
	struct spi_transfer		rd_t;
	struct spi_message		rd_m;

	struct spi_transfer		wd_t;
	struct spi_transfer		wr_t;
	struct spi_transfer		st_t;
	struct spi_message		wr_m;

	bool				want_init_cmd;
	bool				want_cl_led_on;
	bool				have_cl_led_on;
	unsigned int			want_bl_level;
	unsigned int			have_bl_level;
	unsigned int			cmd_msg_cntr;
	/* lock to protect the above parameters and flags below */
	spinlock_t			cmd_msg_lock;
	bool				cmd_msg_queued;
	unsigned int			cmd_log_mask;

	struct led_classdev		backlight_info;

	bool				drain;
	wait_queue_head_t		drain_complete;
	bool				read_active;
	bool				write_active;
};

static const unsigned char applespi_scancodes[] = {
	0, 0, 0, 0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_102ND,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, KEY_RO, 0, KEY_YEN, 0, 0, 0, 0, 0,
	0, KEY_KATAKANAHIRAGANA, KEY_MUHENKAN
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

struct applespi_key_translation {
	u16 from;
	u16 to;
	u8 flags;
};

static const struct applespi_key_translation applespi_fn_codes[] = {
	{ KEY_BACKSPACE, KEY_DELETE },
	{ KEY_ENTER,	KEY_INSERT },
	{ KEY_F1,	KEY_BRIGHTNESSDOWN,	APPLE_FLAG_FKEY },
	{ KEY_F2,	KEY_BRIGHTNESSUP,	APPLE_FLAG_FKEY },
	{ KEY_F3,	KEY_SCALE,		APPLE_FLAG_FKEY },
	{ KEY_F4,	KEY_DASHBOARD,		APPLE_FLAG_FKEY },
	{ KEY_F5,	KEY_KBDILLUMDOWN,	APPLE_FLAG_FKEY },
	{ KEY_F6,	KEY_KBDILLUMUP,		APPLE_FLAG_FKEY },
	{ KEY_F7,	KEY_PREVIOUSSONG,	APPLE_FLAG_FKEY },
	{ KEY_F8,	KEY_PLAYPAUSE,		APPLE_FLAG_FKEY },
	{ KEY_F9,	KEY_NEXTSONG,		APPLE_FLAG_FKEY },
	{ KEY_F10,	KEY_MUTE,		APPLE_FLAG_FKEY },
	{ KEY_F11,	KEY_VOLUMEDOWN,		APPLE_FLAG_FKEY },
	{ KEY_F12,	KEY_VOLUMEUP,		APPLE_FLAG_FKEY },
	{ KEY_RIGHT,	KEY_END },
	{ KEY_LEFT,	KEY_HOME },
	{ KEY_DOWN,	KEY_PAGEDOWN },
	{ KEY_UP,	KEY_PAGEUP },
	{ },
};

static const struct applespi_key_translation apple_iso_keyboard[] = {
	{ KEY_GRAVE,	KEY_102ND },
	{ KEY_102ND,	KEY_GRAVE },
	{ },
};

static struct applespi_tp_info applespi_macbookpro131_info = {
	-6243, 6749, -170, 7685
};

static struct applespi_tp_info applespi_macbookpro133_info = {
	-7456, 7976, -163, 9283
};

/* MacBook8, MacBook9, MacBook10 */
static struct applespi_tp_info applespi_default_info = {
	-5087, 5579, -182, 6089
};

static const struct dmi_system_id applespi_touchpad_infos[] = {
	{
		.ident = "Apple MacBookPro13,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro13,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,2")
		},
		.driver_data = &applespi_macbookpro131_info, /* same touchpad */
	},
	{
		.ident = "Apple MacBookPro13,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro13,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple MacBookPro14,1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,1")
		},
		.driver_data = &applespi_macbookpro131_info,
	},
	{
		.ident = "Apple MacBookPro14,2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,2")
		},
		.driver_data = &applespi_macbookpro131_info, /* same touchpad */
	},
	{
		.ident = "Apple MacBookPro14,3",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "MacBookPro14,3")
		},
		.driver_data = &applespi_macbookpro133_info,
	},
	{
		.ident = "Apple Generic MacBook(Pro)",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Apple Inc."),
		},
		.driver_data = &applespi_default_info,
	},
};

static const char *applespi_debug_facility(unsigned int log_mask)
{
	switch (log_mask) {
	case DBG_CMD_TP_INI:
		return "Touchpad Initialization";
	case DBG_CMD_BL:
		return "Backlight Command";
	case DBG_CMD_CL:
		return "Caps-Lock Command";
	case DBG_RD_KEYB:
		return "Keyboard Event";
	case DBG_RD_TPAD:
		return "Touchpad Event";
	case DBG_RD_UNKN:
		return "Unknown Event";
	case DBG_RD_IRQ:
		return "Interrupt Request";
	case DBG_TP_DIM:
		return "Touchpad Dimensions";
	default:
		return "-Unknown-";
	}
}

static void applespi_setup_read_txfrs(struct applespi_data *applespi)
{
	struct spi_message *msg = &applespi->rd_m;
	struct spi_transfer *dl_t = &applespi->dl_t;
	struct spi_transfer *rd_t = &applespi->rd_t;

	memset(dl_t, 0, sizeof(*dl_t));
	memset(rd_t, 0, sizeof(*rd_t));

	dl_t->delay_usecs = applespi->spi_settings.spi_cs_delay;

	rd_t->rx_buf = applespi->rx_buffer;
	rd_t->len = APPLESPI_PACKET_SIZE;

	spi_message_init(msg);
	spi_message_add_tail(dl_t, msg);
	spi_message_add_tail(rd_t, msg);
}

static void applespi_setup_write_txfrs(struct applespi_data *applespi)
{
	struct spi_message *msg = &applespi->wr_m;
	struct spi_transfer *dl_t = &applespi->wd_t;
	struct spi_transfer *wr_t = &applespi->wr_t;
	struct spi_transfer *st_t = &applespi->st_t;

	memset(dl_t, 0, sizeof(*dl_t));
	memset(wr_t, 0, sizeof(*wr_t));
	memset(st_t, 0, sizeof(*st_t));

	dl_t->delay_usecs = applespi->spi_settings.spi_cs_delay;

	wr_t->tx_buf = applespi->tx_buffer;
	wr_t->len = APPLESPI_PACKET_SIZE;
	wr_t->delay_usecs = SPI_RW_CHG_DLY;

	st_t->rx_buf = applespi->tx_status;
	st_t->len = APPLESPI_STATUS_SIZE;

	spi_message_init(msg);
	spi_message_add_tail(dl_t, msg);
	spi_message_add_tail(wr_t, msg);
	spi_message_add_tail(st_t, msg);
}

static int applespi_async(struct applespi_data *applespi,
			  struct spi_message *message, void (*complete)(void *))
{
	message->complete = complete;
	message->context = applespi;

	return spi_async(applespi->spi, message);
}

static inline bool applespi_check_write_status(struct applespi_data *applespi,
					       int sts)
{
	static u8 sts_ok[] = { 0xac, 0x27, 0x68, 0xd5 };
	bool ret = true;

	if (sts < 0) {
		ret = false;
		pr_warn("Error writing to device: %d\n", sts);
	} else if (memcmp(applespi->tx_status, sts_ok,
			  APPLESPI_STATUS_SIZE) != 0) {
		ret = false;
		pr_warn("Error writing to device: %x %x %x %x\n",
			applespi->tx_status[0], applespi->tx_status[1],
			applespi->tx_status[2], applespi->tx_status[3]);
	}

	return ret;
}

#ifdef PRE_SPI_PROPERTIES

struct appleacpi_spi_registration_info {
	struct class_interface	cif;
	struct acpi_device	*adev;
	struct spi_device	*spi;
	struct spi_master	*spi_master;
	struct delayed_work	work;
	struct notifier_block	slave_notifier;
};

struct applespi_acpi_map_entry {
	char *name;
	size_t field_offset;
};

static const struct applespi_acpi_map_entry applespi_spi_settings_map[] = {
	{ "spiSclkPeriod", offsetof(struct spi_settings, spi_sclk_period) },
	{ "spiWordSize",   offsetof(struct spi_settings, spi_word_size) },
	{ "spiBitOrder",   offsetof(struct spi_settings, spi_bit_order) },
	{ "spiSPO",        offsetof(struct spi_settings, spi_spo) },
	{ "spiSPH",        offsetof(struct spi_settings, spi_sph) },
	{ "spiCSDelay",    offsetof(struct spi_settings, spi_cs_delay) },
	{ "resetA2RUsec",  offsetof(struct spi_settings, reset_a2r_usec) },
	{ "resetRecUsec",  offsetof(struct spi_settings, reset_rec_usec) },
};

static u8 *acpi_dsm_uuid = "a0b5b7c6-1318-441c-b0c9-fe695eaf949b";

static int applespi_find_settings_field(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(applespi_spi_settings_map); i++) {
		if (strcmp(applespi_spi_settings_map[i].name, name) == 0)
			return applespi_spi_settings_map[i].field_offset;
	}

	return -1;
}

static int applespi_get_spi_settings(acpi_handle handle,
				     struct spi_settings *settings)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	guid_t guid, *uuid = &guid;
#else
	u8 uuid[16];
#endif
	union acpi_object *spi_info;
	union acpi_object name;
	union acpi_object value;
	int i;
	int field_off;
	u64 *field;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
	guid_parse(acpi_dsm_uuid, uuid);
#else
	acpi_str_to_uuid(acpi_dsm_uuid, uuid);
#endif

	spi_info = acpi_evaluate_dsm(handle, uuid, 1, 1, NULL);
	if (!spi_info) {
		pr_err("Failed to get SPI info from _DSM method\n");
		return -ENODEV;
	}
	if (spi_info->type != ACPI_TYPE_PACKAGE) {
		pr_err("Unexpected data returned from SPI _DSM method: type=%d\n",
		       spi_info->type);
		ACPI_FREE(spi_info);
		return -ENODEV;
	}

	/*
	 * The data is stored in pairs of items, first a string containing
	 * the name of the item, followed by an 8-byte buffer containing the
	 * value in little-endian.
	 */
	for (i = 0; i < spi_info->package.count - 1; i += 2) {
		name = spi_info->package.elements[i];
		value = spi_info->package.elements[i + 1];

		if (!(name.type == ACPI_TYPE_STRING &&
		      value.type == ACPI_TYPE_BUFFER &&
		      value.buffer.length == 8)) {
			pr_warn("Unexpected data returned from SPI _DSM method: name.type=%d, value.type=%d\n",
				name.type, value.type);
			continue;
		}

		field_off = applespi_find_settings_field(name.string.pointer);
		if (field_off < 0) {
			pr_debug("Skipping unknown SPI setting '%s'\n",
				 name.string.pointer);
			continue;
		}

		field = (u64 *)((char *)settings + field_off);
		*field = le64_to_cpu(*((__le64 *)value.buffer.pointer));
	}
	ACPI_FREE(spi_info);

	return 0;
}

#else

static int applespi_get_spi_settings(struct applespi_data *applespi)
{
	struct acpi_device *adev = ACPI_COMPANION(&applespi->spi->dev);
	const union acpi_object *o;
	struct spi_settings *settings = &applespi->spi_settings;

	if (!acpi_dev_get_property(adev, "spiCSDelay", ACPI_TYPE_BUFFER, &o))
		settings->spi_cs_delay = *(u64 *)o->buffer.pointer;
	else
		pr_warn("Property spiCSDelay not found\n");

	if (!acpi_dev_get_property(adev, "resetA2RUsec", ACPI_TYPE_BUFFER, &o))
		settings->reset_a2r_usec = *(u64 *)o->buffer.pointer;
	else
		pr_warn("Property resetA2RUsec not found\n");

	if (!acpi_dev_get_property(adev, "resetRecUsec", ACPI_TYPE_BUFFER, &o))
		settings->reset_rec_usec = *(u64 *)o->buffer.pointer;
	else
		pr_warn("Property resetRecUsec not found\n");

	pr_debug("SPI settings: spi_cs_delay=%llu reset_a2r_usec=%llu reset_rec_usec=%llu\n",
		 settings->spi_cs_delay, settings->reset_a2r_usec,
		 settings->reset_rec_usec);

	return 0;
}

#endif

static int applespi_setup_spi(struct applespi_data *applespi)
{
	int sts;

#ifdef PRE_SPI_PROPERTIES
	sts = applespi_get_spi_settings(applespi->handle,
					&applespi->spi_settings);
#else
	sts = applespi_get_spi_settings(applespi);
#endif
	if (sts)
		return sts;

	spin_lock_init(&applespi->cmd_msg_lock);
	init_waitqueue_head(&applespi->drain_complete);

	return 0;
}

static int applespi_enable_spi(struct applespi_data *applespi)
{
	int result;
	unsigned long long spi_status;

	/* check if SPI is already enabled, so we can skip the delay below */
	result = acpi_evaluate_integer(applespi->sist, NULL, NULL, &spi_status);
	if (ACPI_SUCCESS(result) && spi_status)
		return 0;

	/* SIEN(1) will enable SPI communication */
	result = acpi_execute_simple_method(applespi->sien, NULL, 1);
	if (ACPI_FAILURE(result)) {
		pr_err("SIEN failed: %s\n", acpi_format_exception(result));
		return -ENODEV;
	}

	/*
	 * Allow the SPI interface to come up before returning. Without this
	 * delay, the SPI commands to enable multitouch mode may not reach
	 * the trackpad controller, causing pointer movement to break upon
	 * resume from sleep.
	 */
	msleep(50);

	return 0;
}

static int applespi_send_cmd_msg(struct applespi_data *applespi);

static void applespi_msg_complete(struct applespi_data *applespi,
				  bool is_write_msg, bool is_read_compl)
{
	unsigned long flags;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	if (is_read_compl)
		applespi->read_active = false;
	if (is_write_msg)
		applespi->write_active = false;

	if (applespi->drain && !applespi->write_active)
		wake_up_all(&applespi->drain_complete);

	if (is_write_msg) {
		applespi->cmd_msg_queued = false;
		applespi_send_cmd_msg(applespi);
	}

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static void applespi_async_write_complete(void *context)
{
	struct applespi_data *applespi = context;

	debug_print(applespi->cmd_log_mask, "--- %s ------------------------\n",
		    applespi_debug_facility(applespi->cmd_log_mask));
	debug_print_buffer(applespi->cmd_log_mask, "write  ",
			   applespi->tx_buffer, APPLESPI_PACKET_SIZE);
	debug_print_buffer(applespi->cmd_log_mask, "status ",
			   applespi->tx_status, APPLESPI_STATUS_SIZE);

	if (!applespi_check_write_status(applespi, applespi->wr_m.status))
		/*
		 * If we got an error, we presumably won't get the expected
		 * response message either.
		 */
		applespi_msg_complete(applespi, true, false);
}

static int applespi_send_cmd_msg(struct applespi_data *applespi)
{
	u16 crc;
	int sts;
	struct spi_packet *packet = (struct spi_packet *)applespi->tx_buffer;
	struct message *message = (struct message *)packet->data;
	u16 msg_len;
	u8 device;

	/* check if draining */
	if (applespi->drain)
		return 0;

	/* check whether send is in progress */
	if (applespi->cmd_msg_queued)
		return 0;

	/* set up packet */
	memset(packet, 0, APPLESPI_PACKET_SIZE);

	/* are we processing init commands? */
	if (applespi->want_init_cmd) {
		applespi->want_init_cmd = false;
		applespi->cmd_log_mask = DBG_CMD_TP_INI;

		/* build init command */
		device = PACKET_DEV_TPAD;

		message->type = cpu_to_le16(0x0252);
		msg_len = sizeof(message->init_command);

		message->init_command.cmd = cpu_to_le16(0x0102);

	/* do we need caps-lock command? */
	} else if (applespi->want_cl_led_on != applespi->have_cl_led_on) {
		applespi->have_cl_led_on = applespi->want_cl_led_on;
		applespi->cmd_log_mask = DBG_CMD_CL;

		/* build led command */
		device = PACKET_DEV_KEYB;

		message->type = cpu_to_le16(0x0151);
		msg_len = sizeof(message->capsl_command);

		message->capsl_command.unknown = 0x01;
		message->capsl_command.led = applespi->have_cl_led_on ? 2 : 0;

	/* do we need backlight command? */
	} else if (applespi->want_bl_level != applespi->have_bl_level) {
		applespi->have_bl_level = applespi->want_bl_level;
		applespi->cmd_log_mask = DBG_CMD_BL;

		/* build command buffer */
		device = PACKET_DEV_KEYB;

		message->type = cpu_to_le16(0xB051);
		msg_len = sizeof(message->bl_command);

		message->bl_command.const1 = cpu_to_le16(0x01B0);
		message->bl_command.level =
				cpu_to_le16(applespi->have_bl_level);

		if (applespi->have_bl_level > 0)
			message->bl_command.const2 = cpu_to_le16(0x01F4);
		else
			message->bl_command.const2 = cpu_to_le16(0x0001);

	/* everything's up-to-date */
	} else {
		return 0;
	}

	/* finalize packet */
	packet->flags = PACKET_TYPE_WRITE;
	packet->device = device;
	packet->length = cpu_to_le16(MSG_HEADER_SIZE + msg_len);

	message->counter = applespi->cmd_msg_cntr++ & 0xff;

	message->length = cpu_to_le16(msg_len - 2);
	message->rsp_buf_len = message->length;

	crc = crc16(0, (u8 *)message, le16_to_cpu(packet->length) - 2);
	*((__le16 *)&message->data[msg_len - 2]) = cpu_to_le16(crc);

	crc = crc16(0, (u8 *)packet, sizeof(*packet) - 2);
	packet->crc_16 = cpu_to_le16(crc);

	/* send command */
	sts = applespi_async(applespi, &applespi->wr_m,
			     applespi_async_write_complete);

	if (sts != 0) {
		pr_warn("Error queueing async write to device: %d\n", sts);
	} else {
		applespi->cmd_msg_queued = true;
		applespi->write_active = true;
	}

	return sts;
}

static void applespi_init(struct applespi_data *applespi)
{
	unsigned long flags;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->want_init_cmd = true;
	applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static int applespi_set_capsl_led(struct applespi_data *applespi,
				  bool capslock_on)
{
	unsigned long flags;
	int sts;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->want_cl_led_on = capslock_on;
	sts = applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	return sts;
}

static void applespi_set_bl_level(struct led_classdev *led_cdev,
				  enum led_brightness value)

{
	struct applespi_data *applespi =
		container_of(led_cdev, struct applespi_data, backlight_info);
	unsigned long flags;
	int sts;

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	if (value == 0)
		applespi->want_bl_level = value;
	else
		/*
		 * The backlight does not turn on till level 32, so we scale
		 * the range here so that from a user's perspective it turns
		 * on at 1.
		 */
		applespi->want_bl_level = (unsigned int)
			((value * KBD_BL_LEVEL_ADJ) / KBD_BL_LEVEL_SCALE +
			 MIN_KBD_BL_LEVEL);

	sts = applespi_send_cmd_msg(applespi);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);
}

static int applespi_event(struct input_dev *dev, unsigned int type,
			  unsigned int code, int value)
{
	struct applespi_data *applespi = input_get_drvdata(dev);

	switch (type) {
	case EV_LED:
		applespi_set_capsl_led(applespi,
				       !!test_bit(LED_CAPSL, dev->led));
		return 0;
	}

	return -1;
}

/* lifted from the BCM5974 driver */
/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x)
{
	return (signed short)le16_to_cpu(x);
}

static void report_finger_data(struct input_dev *input, int slot,
			       const struct input_mt_pos *pos,
			       const struct tp_finger *f)
{
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

	input_report_abs(input, ABS_MT_TOUCH_MAJOR,
			 raw2int(f->touch_major) << 1);
	input_report_abs(input, ABS_MT_TOUCH_MINOR,
			 raw2int(f->touch_minor) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR,
			 raw2int(f->tool_major) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MINOR,
			 raw2int(f->tool_minor) << 1);
	input_report_abs(input, ABS_MT_ORIENTATION,
			 MAX_FINGER_ORIENTATION - raw2int(f->orientation));
	input_report_abs(input, ABS_MT_POSITION_X, pos->x);
	input_report_abs(input, ABS_MT_POSITION_Y, pos->y);
}

static int report_tp_state(struct applespi_data *applespi,
			   struct touchpad_protocol *t)
{
	static int min_x, max_x, min_y, max_y;
	static bool dim_updated;
	static ktime_t last_print;

	const struct tp_finger *f;
	struct input_dev *input = applespi->touchpad_input_dev;
	const struct applespi_tp_info *tp_info = &applespi->tp_info;
	int i, n;

	n = 0;

	for (i = 0; i < t->number_of_fingers; i++) {
		f = &t->fingers[i];
		if (raw2int(f->touch_major) == 0)
			continue;
		applespi->pos[n].x = raw2int(f->abs_x);
		applespi->pos[n].y = tp_info->y_min + tp_info->y_max -
				     raw2int(f->abs_y);
		n++;

		if (debug & DBG_TP_DIM) {
			#define UPDATE_DIMENSIONS(val, op, last) \
				do { \
					if (raw2int(val) op last) { \
						last = raw2int(val); \
						dim_updated = true; \
					} \
				} while (0)

			UPDATE_DIMENSIONS(f->abs_x, <, min_x);
			UPDATE_DIMENSIONS(f->abs_x, >, max_x);
			UPDATE_DIMENSIONS(f->abs_y, <, min_y);
			UPDATE_DIMENSIONS(f->abs_y, >, max_y);
		}
	}

	if (debug & DBG_TP_DIM) {
		if (dim_updated &&
		    ktime_ms_delta(ktime_get(), last_print) > 1000) {
			printk(KERN_DEBUG
			       pr_fmt("New touchpad dimensions: %d %d %d %d\n"),
			       min_x, max_x, min_y, max_y);
			dim_updated = false;
			last_print = ktime_get();
		}
	}

	input_mt_assign_slots(input, applespi->slots, applespi->pos, n, 0);

	for (i = 0; i < n; i++)
		report_finger_data(input, applespi->slots[i],
				   &applespi->pos[i], &t->fingers[i]);

	input_mt_sync_frame(input);
	input_report_key(input, BTN_LEFT, t->clicked);

	input_sync(input);
	return 0;
}

static const struct applespi_key_translation *applespi_find_translation(
		const struct applespi_key_translation *table, u16 key)
{
	const struct applespi_key_translation *trans;

	for (trans = table; trans->from; trans++)
		if (trans->from == key)
			return trans;

	return NULL;
}

static unsigned int applespi_code_to_key(u8 code, int fn_pressed)
{
	unsigned int key = applespi_scancodes[code];
	const struct applespi_key_translation *trans;

	if (fnmode) {
		int do_translate;

		trans = applespi_find_translation(applespi_fn_codes, key);
		if (trans) {
			if (trans->flags & APPLE_FLAG_FKEY)
				do_translate = (fnmode == 2 && fn_pressed) ||
					       (fnmode == 1 && !fn_pressed);
			else
				do_translate = fn_pressed;

			if (do_translate)
				key = trans->to;
		}
	}

	if (iso_layout) {
		trans = applespi_find_translation(apple_iso_keyboard, key);
		if (trans)
			key = trans->to;
	}

	return key;
}

static void applespi_handle_keyboard_event(struct applespi_data *applespi,
					   struct keyboard_protocol
							*keyboard_protocol)
{
	int i, j;
	unsigned int key;
	bool still_pressed;

	/* check released keys */
	for (i = 0; i < MAX_ROLLOVER; i++) {
		still_pressed = false;
		for (j = 0; j < MAX_ROLLOVER; j++) {
			if (applespi->last_keys_pressed[i] ==
			    keyboard_protocol->keys_pressed[j]) {
				still_pressed = true;
				break;
			}
		}

		if (!still_pressed) {
			key = applespi_code_to_key(
					applespi->last_keys_pressed[i],
					applespi->last_keys_fn_pressed[i]);
			input_report_key(applespi->keyboard_input_dev, key, 0);
			applespi->last_keys_fn_pressed[i] = 0;
		}
	}

	/* check pressed keys */
	for (i = 0; i < MAX_ROLLOVER; i++) {
		if (keyboard_protocol->keys_pressed[i] <
				ARRAY_SIZE(applespi_scancodes) &&
		    keyboard_protocol->keys_pressed[i] > 0) {
			key = applespi_code_to_key(
					keyboard_protocol->keys_pressed[i],
					keyboard_protocol->fn_pressed);
			input_report_key(applespi->keyboard_input_dev, key, 1);
			applespi->last_keys_fn_pressed[i] =
					keyboard_protocol->fn_pressed;
		}
	}

	/* check control keys */
	for (i = 0; i < MAX_MODIFIERS; i++) {
		u8 *modifiers = &keyboard_protocol->modifiers;

		if (test_bit(i, (unsigned long *)modifiers)) {
			input_report_key(applespi->keyboard_input_dev,
					 applespi_controlcodes[i], 1);
		} else {
			input_report_key(applespi->keyboard_input_dev,
					 applespi_controlcodes[i], 0);
		}
	}

	/* check function key */
	if (keyboard_protocol->fn_pressed && !applespi->last_fn_pressed) {
		input_report_key(applespi->keyboard_input_dev, KEY_FN, 1);
	} else if (!keyboard_protocol->fn_pressed &&
		   applespi->last_fn_pressed) {
		input_report_key(applespi->keyboard_input_dev, KEY_FN, 0);
	}
	applespi->last_fn_pressed = keyboard_protocol->fn_pressed;

	/* done */
	input_sync(applespi->keyboard_input_dev);
	memcpy(&applespi->last_keys_pressed, keyboard_protocol->keys_pressed,
	       sizeof(applespi->last_keys_pressed));
}

static void applespi_handle_cmd_response(struct applespi_data *applespi,
					 struct spi_packet *packet,
					 struct message *message)
{
	if (le16_to_cpu(message->length) != 0x0000) {
		dev_warn_ratelimited(&applespi->spi->dev,
				     "Received unexpected write response: length=%x\n",
				     le16_to_cpu(message->length));
		return;
	}

	if (packet->device == PACKET_DEV_TPAD &&
	    le16_to_cpu(message->type) == 0x0252 &&
	    le16_to_cpu(message->rsp_buf_len) == 0x0002)
		pr_info("modeswitch done.\n");
}

static bool applespi_verify_crc(struct applespi_data *applespi, u8 *buffer,
				size_t buflen)
{
	u16 crc;

	crc = crc16(0, buffer, buflen);
	if (crc != 0) {
		dev_warn_ratelimited(&applespi->spi->dev,
				     "Received corrupted packet (crc mismatch)\n");
		return false;
	}

	return true;
}

static void applespi_debug_print_read_packet(struct applespi_data *applespi,
					     struct spi_packet *packet)
{
	unsigned int dbg_mask;

	if (packet->flags == PACKET_TYPE_READ &&
	    packet->device == PACKET_DEV_KEYB) {
		dbg_mask = DBG_RD_KEYB;

	} else if (packet->flags == PACKET_TYPE_READ &&
		   packet->device == PACKET_DEV_TPAD) {
		dbg_mask = DBG_RD_TPAD;

	} else if (packet->flags == PACKET_TYPE_WRITE) {
		dbg_mask = applespi->cmd_log_mask;

	} else {
		dbg_mask = DBG_RD_UNKN;
	}

	debug_print(dbg_mask, "--- %s ---------------------------\n",
		    applespi_debug_facility(dbg_mask));
	debug_print_buffer(dbg_mask, "read   ", applespi->rx_buffer,
			   APPLESPI_PACKET_SIZE);
}

static void applespi_got_data(struct applespi_data *applespi)
{
	struct spi_packet *packet;
	struct message *message;
	unsigned int msg_len;
	unsigned int off;
	unsigned int rem;
	unsigned int len;

	/* process packet header */
	if (!applespi_verify_crc(applespi, applespi->rx_buffer,
				 APPLESPI_PACKET_SIZE)) {
		unsigned long flags;

		spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

		if (applespi->drain) {
			applespi->read_active = false;
			applespi->write_active = false;

			wake_up_all(&applespi->drain_complete);
		}

		spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

		return;
	}

	packet = (struct spi_packet *)applespi->rx_buffer;

	applespi_debug_print_read_packet(applespi, packet);

	off = le16_to_cpu(packet->offset);
	rem = le16_to_cpu(packet->remaining);
	len = le16_to_cpu(packet->length);

	if (len > sizeof(packet->data)) {
		dev_warn_ratelimited(&applespi->spi->dev,
				     "Received corrupted packet (invalid packet length)\n");
		goto cleanup;
	}

	/* handle multi-packet messages */
	if (rem > 0 || off > 0) {
		if (off != applespi->saved_msg_len) {
			dev_warn_ratelimited(&applespi->spi->dev,
					     "Received unexpected offset (got %u, expected %u)\n",
					     off, applespi->saved_msg_len);
			goto cleanup;
		}

		if (off + rem > MAX_PKTS_PER_MSG * APPLESPI_PACKET_SIZE) {
			dev_warn_ratelimited(&applespi->spi->dev,
					     "Received message too large (size %u)\n",
					     off + rem);
			goto cleanup;
		}

		if (off + len > MAX_PKTS_PER_MSG * APPLESPI_PACKET_SIZE) {
			dev_warn_ratelimited(&applespi->spi->dev,
					     "Received message too large (size %u)\n",
					     off + len);
			goto cleanup;
		}

		memcpy(applespi->msg_buf + off, &packet->data, len);
		applespi->saved_msg_len += len;

		if (rem > 0)
			return;

		message = (struct message *)applespi->msg_buf;
		msg_len = applespi->saved_msg_len;
	} else {
		message = (struct message *)&packet->data;
		msg_len = len;
	}

	applespi->saved_msg_len = 0;

	/* got complete message - verify */
	if (le16_to_cpu(message->length) != msg_len - MSG_HEADER_SIZE - 2) {
		dev_warn_ratelimited(&applespi->spi->dev,
				     "Received corrupted packet (invalid message length)\n");
		goto cleanup;
	}

	if (!applespi_verify_crc(applespi, (u8 *)message, msg_len))
		goto cleanup;

	/* handle message */
	if (packet->flags == PACKET_TYPE_READ &&
	    packet->device == PACKET_DEV_KEYB) {
		applespi_handle_keyboard_event(applespi, &message->keyboard);

	} else if (packet->flags == PACKET_TYPE_READ &&
		   packet->device == PACKET_DEV_TPAD) {
		struct touchpad_protocol *tp = &message->touchpad;

		size_t tp_len = sizeof(*tp) +
				tp->number_of_fingers * sizeof(tp->fingers[0]);
		if (le16_to_cpu(message->length) + 2 != tp_len) {
			dev_warn_ratelimited(&applespi->spi->dev,
					     "Received corrupted packet (invalid message length)\n");
			goto cleanup;
		}

		if (tp->number_of_fingers > MAX_FINGERS) {
			dev_warn_ratelimited(&applespi->spi->dev,
					     "Number of reported fingers (%u) exceeds max (%u))\n",
					     tp->number_of_fingers,
					     MAX_FINGERS);
			tp->number_of_fingers = MAX_FINGERS;
		}

		report_tp_state(applespi, tp);

	} else if (packet->flags == PACKET_TYPE_WRITE) {
		applespi_handle_cmd_response(applespi, packet, message);
	}

cleanup:
	/*
	 * Note: this relies on the fact that we are blocking the processing of
	 * spi messages at this point, i.e. that no further transfers or cs
	 * changes are processed while we delay here.
	 */
	udelay(SPI_RW_CHG_DLY);

	/* clean up */
	applespi_msg_complete(applespi, packet->flags == PACKET_TYPE_WRITE,
			      true);
}

static void applespi_async_read_complete(void *context)
{
	struct applespi_data *applespi = context;

	if (applespi->rd_m.status < 0)
		pr_warn("Error reading from device: %d\n",
			applespi->rd_m.status);
	else
		applespi_got_data(applespi);

	acpi_finish_gpe(NULL, applespi->gpe);
}

static u32 applespi_notify(acpi_handle gpe_device, u32 gpe, void *context)
{
	struct applespi_data *applespi = context;
	int sts;
	unsigned long flags;

	debug_print(DBG_RD_IRQ, "--- %s ---------------------------\n",
		    applespi_debug_facility(DBG_RD_IRQ));

	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	sts = applespi_async(applespi, &applespi->rd_m,
			     applespi_async_read_complete);
	if (sts != 0)
		pr_warn("Error queueing async read to device: %d\n", sts);
	else
		applespi->read_active = true;

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	return ACPI_INTERRUPT_HANDLED;
}

static int applespi_probe(struct spi_device *spi)
{
	struct applespi_data *applespi;
	int result, i;
	unsigned long long gpe, usb_status;

	/* check if the USB interface is present and enabled already */
	result = acpi_evaluate_integer(ACPI_HANDLE(&spi->dev), "UIST", NULL,
				       &usb_status);
	if (ACPI_SUCCESS(result) && usb_status) {
		/* let the USB driver take over instead */
		pr_info("USB interface already enabled\n");
		return -ENODEV;
	}

	/* allocate driver data */
	applespi = devm_kzalloc(&spi->dev, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;

	applespi->spi = spi;
	applespi->handle = ACPI_HANDLE(&spi->dev);

	/* store the driver data */
	spi_set_drvdata(spi, applespi);

	/* create our buffers */
	applespi->tx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE,
					   GFP_KERNEL);
	applespi->tx_status = devm_kmalloc(&spi->dev, APPLESPI_STATUS_SIZE,
					   GFP_KERNEL);
	applespi->rx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE,
					   GFP_KERNEL);
	applespi->msg_buf = devm_kmalloc(&spi->dev, MAX_PKTS_PER_MSG *
						    APPLESPI_PACKET_SIZE,
					 GFP_KERNEL);

	if (!applespi->tx_buffer || !applespi->tx_status ||
	    !applespi->rx_buffer)
		return -ENOMEM;

	/* set up our spi messages */
	applespi_setup_read_txfrs(applespi);
	applespi_setup_write_txfrs(applespi);

	/* cache ACPI method handles */
	if (ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIEN",
					 &applespi->sien)) ||
	    ACPI_FAILURE(acpi_get_handle(applespi->handle, "SIST",
					 &applespi->sist))) {
		pr_err("Failed to get required ACPI method handle\n");
		return -ENODEV;
	}

	/* switch on the SPI interface */
	result = applespi_setup_spi(applespi);
	if (result)
		return result;

	result = applespi_enable_spi(applespi);
	if (result)
		return result;

	/* set up touchpad dimensions */
	applespi->tp_info = *(struct applespi_tp_info *)
			dmi_first_match(applespi_touchpad_infos)->driver_data;

	if (touchpad_dimensions[0] || touchpad_dimensions[1] ||
	    touchpad_dimensions[2] || touchpad_dimensions[3]) {
		applespi->tp_info.x_min = touchpad_dimensions[0];
		applespi->tp_info.x_max = touchpad_dimensions[1];
		applespi->tp_info.y_min = touchpad_dimensions[2];
		applespi->tp_info.y_max = touchpad_dimensions[3];
	} else {
		touchpad_dimensions[0] = applespi->tp_info.x_min;
		touchpad_dimensions[1] = applespi->tp_info.x_max;
		touchpad_dimensions[2] = applespi->tp_info.y_min;
		touchpad_dimensions[3] = applespi->tp_info.y_max;
	}

	/* setup the keyboard input dev */
	applespi->keyboard_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->keyboard_input_dev)
		return -ENOMEM;

	applespi->keyboard_input_dev->name = "Apple SPI Keyboard";
	applespi->keyboard_input_dev->phys = "applespi/input0";
	applespi->keyboard_input_dev->dev.parent = &spi->dev;
	applespi->keyboard_input_dev->id.bustype = BUS_SPI;

	applespi->keyboard_input_dev->evbit[0] =
			BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
	applespi->keyboard_input_dev->ledbit[0] = BIT_MASK(LED_CAPSL);

	input_set_drvdata(applespi->keyboard_input_dev, applespi);
	applespi->keyboard_input_dev->event = applespi_event;

	for (i = 0; i < ARRAY_SIZE(applespi_scancodes); i++)
		if (applespi_scancodes[i])
			input_set_capability(applespi->keyboard_input_dev,
					     EV_KEY, applespi_scancodes[i]);

	for (i = 0; i < ARRAY_SIZE(applespi_controlcodes); i++)
		if (applespi_controlcodes[i])
			input_set_capability(applespi->keyboard_input_dev,
					     EV_KEY, applespi_controlcodes[i]);

	for (i = 0; i < ARRAY_SIZE(applespi_fn_codes); i++)
		if (applespi_fn_codes[i].to)
			input_set_capability(applespi->keyboard_input_dev,
					     EV_KEY, applespi_fn_codes[i].to);

	input_set_capability(applespi->keyboard_input_dev, EV_KEY, KEY_FN);

	result = input_register_device(applespi->keyboard_input_dev);
	if (result) {
		pr_err("Unabled to register keyboard input device (%d)\n",
		       result);
		return -ENODEV;
	}

	/* now, set up the touchpad as a separate input device */
	applespi->touchpad_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->touchpad_input_dev)
		return -ENOMEM;

	applespi->touchpad_input_dev->name = "Apple SPI Touchpad";
	applespi->touchpad_input_dev->phys = "applespi/input1";
	applespi->touchpad_input_dev->dev.parent = &spi->dev;
	applespi->touchpad_input_dev->id.bustype = BUS_SPI;

	input_set_capability(applespi->touchpad_input_dev, EV_REL, REL_X);
	input_set_capability(applespi->touchpad_input_dev, EV_REL, REL_Y);

	__set_bit(INPUT_PROP_POINTER, applespi->touchpad_input_dev->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, applespi->touchpad_input_dev->propbit);

	/* finger touch area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MAJOR,
			     0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MINOR,
			     0, 2048, 0, 0);

	/* finger approach area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MAJOR,
			     0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MINOR,
			     0, 2048, 0, 0);

	/* finger orientation */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_ORIENTATION,
			     -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION,
			     0, 0);

	/* finger position */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_X,
			     applespi->tp_info.x_min, applespi->tp_info.x_max,
			     0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_Y,
			     applespi->tp_info.y_min, applespi->tp_info.y_max,
			     0, 0);

	input_set_capability(applespi->touchpad_input_dev, EV_KEY,
			     BTN_TOOL_FINGER);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_LEFT);

	input_mt_init_slots(applespi->touchpad_input_dev, MAX_FINGERS,
			    INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED |
			    INPUT_MT_TRACK);

	result = input_register_device(applespi->touchpad_input_dev);
	if (result) {
		pr_err("Unabled to register touchpad input device (%d)\n",
		       result);
		return -ENODEV;
	}

	/*
	 * The applespi device doesn't send interrupts normally (as is described
	 * in its DSDT), but rather seems to use ACPI GPEs.
	 */
	result = acpi_evaluate_integer(applespi->handle, "_GPE", NULL, &gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to obtain GPE for SPI slave device: %s\n",
		       acpi_format_exception(result));
		return -ENODEV;
	}
	applespi->gpe = (int)gpe;

	result = acpi_install_gpe_handler(NULL, applespi->gpe,
					  ACPI_GPE_LEVEL_TRIGGERED,
					  applespi_notify, applespi);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to install GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		return -ENODEV;
	}

	result = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(result)) {
		pr_err("Failed to enable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(result));
		acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);
		return -ENODEV;
	}

	/* switch the touchpad into multitouch mode */
	applespi_init(applespi);

	/* set up keyboard-backlight */
	applespi->backlight_info.name            = "spi::kbd_backlight";
	applespi->backlight_info.default_trigger = "kbd-backlight";
	applespi->backlight_info.brightness_set  = applespi_set_bl_level;

	result = devm_led_classdev_register(&spi->dev,
					    &applespi->backlight_info);
	if (result) {
		pr_err("Unable to register keyboard backlight class dev (%d)\n",
		       result);
		/* not fatal */
	}

	/* done */
	pr_info("spi-device probe done: %s\n", dev_name(&spi->dev));

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);
	unsigned long flags;

	/* wait for all outstanding writes to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->drain = true;
	wait_event_lock_irq(applespi->drain_complete, !applespi->write_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* shut things down */
	acpi_disable_gpe(NULL, applespi->gpe);
	acpi_remove_gpe_handler(NULL, applespi->gpe, applespi_notify);

	/* wait for all outstanding reads to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	wait_event_lock_irq(applespi->drain_complete, !applespi->read_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* done */
	pr_info("spi-device remove done: %s\n", dev_name(&spi->dev));
	return 0;
}

#ifdef CONFIG_PM
static int applespi_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);
	acpi_status status;
	unsigned long flags;

	/* wait for all outstanding writes to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	applespi->drain = true;
	wait_event_lock_irq(applespi->drain_complete, !applespi->write_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	/* disable the interrupt */
	status = acpi_disable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to disable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(status));
	}

	/* wait for all outstanding reads to finish */
	spin_lock_irqsave(&applespi->cmd_msg_lock, flags);

	wait_event_lock_irq(applespi->drain_complete, !applespi->read_active,
			    applespi->cmd_msg_lock);

	spin_unlock_irqrestore(&applespi->cmd_msg_lock, flags);

	pr_info("spi-device suspend done.\n");
	return 0;
}

static int applespi_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct applespi_data *applespi = spi_get_drvdata(spi);
	acpi_status status;

	/* ensure our flags and state reflect a newly resumed device */
	applespi->drain = false;
	applespi->have_cl_led_on = false;
	applespi->have_bl_level = 0;
	applespi->cmd_msg_queued = false;
	applespi->read_active = false;
	applespi->write_active = false;

	/* re-enable the interrupt */
	status = acpi_enable_gpe(NULL, applespi->gpe);
	if (ACPI_FAILURE(status)) {
		pr_err("Failed to re-enable GPE handler for GPE %d: %s\n",
		       applespi->gpe, acpi_format_exception(status));
	}

	/* switch on the SPI interface */
	applespi_enable_spi(applespi);

	/* switch the touchpad into multitouch mode */
	applespi_init(applespi);

	pr_info("spi-device resume done.\n");

	return 0;
}
#endif

static const struct acpi_device_id applespi_acpi_match[] = {
	{ "APP000D", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static UNIVERSAL_DEV_PM_OPS(applespi_pm_ops, applespi_suspend,
			    applespi_resume, NULL);

static struct spi_driver applespi_driver = {
	.driver		= {
		.name			= "applespi",
		.owner			= THIS_MODULE,

		.acpi_match_table	= ACPI_PTR(applespi_acpi_match),
		.pm			= &applespi_pm_ops,
	},
	.probe		= applespi_probe,
	.remove		= applespi_remove,
};

#ifdef PRE_SPI_PROPERTIES

#define SPI_DEV_CHIP_SEL	0	/* from DSDT UBUF */

/*
 * All the following code is to deal with the fact that the _CRS method for
 * the SPI device in the DSDT returns an empty resource, and the real info is
 * available from the _DSM method. So we need to hook into the ACPI device
 * registration and create and register the SPI device ourselves.
 *
 * All of this can be removed and replaced with
 * module_spi_driver(applespi_driver)
 * when the core adds support for this sort of setup.
 */

/*
 * Configure the spi device with the info from the _DSM method.
 */
static int appleacpi_config_spi_dev(struct spi_device *spi,
				    struct acpi_device *adev)
{
	struct spi_settings settings;
	int ret;

	ret = applespi_get_spi_settings(acpi_device_handle(adev), &settings);
	if (ret)
		return ret;

	spi->max_speed_hz = 1000000000 / settings.spi_sclk_period;
	spi->chip_select = SPI_DEV_CHIP_SEL;
	spi->bits_per_word = settings.spi_word_size;

	spi->mode =
		(settings.spi_spo * SPI_CPOL) |
		(settings.spi_sph * SPI_CPHA) |
		(settings.spi_bit_order == 0 ? SPI_LSB_FIRST : 0);

	spi->irq = -1;		/* uses GPE */

	spi->dev.platform_data = NULL;
	spi->controller_data = NULL;
	spi->controller_state = NULL;

	pr_debug("spi-config: max_speed_hz=%d, chip_select=%d, bits_per_word=%d, mode=%x, irq=%d\n",
		 spi->max_speed_hz, spi->chip_select, spi->bits_per_word,
		 spi->mode, spi->irq);

	return 0;
}

static int appleacpi_is_device_registered(struct device *dev, void *data)
{
	struct spi_device *spi = to_spi_device(dev);
	struct spi_master *spi_master = data;

	if (spi->master == spi_master && spi->chip_select == SPI_DEV_CHIP_SEL)
		return -EBUSY;
	return 0;
}

/*
 * Unregister all physical devices devices associated with the acpi device,
 * so that the new SPI device becomes the first physical device for it.
 * Otherwise we don't get properly registered as the driver for the spi
 * device.
 */
static void appleacpi_unregister_phys_devs(struct acpi_device *adev)
{
	struct acpi_device_physical_node *entry;
	struct device *dev;

	while (true) {
		mutex_lock(&adev->physical_node_lock);

		if (list_empty(&adev->physical_node_list)) {
			mutex_unlock(&adev->physical_node_lock);
			break;
		}

		entry = list_first_entry(&adev->physical_node_list,
					 struct acpi_device_physical_node,
					 node);
		dev = get_device(entry->dev);

		mutex_unlock(&adev->physical_node_lock);

		platform_device_unregister(to_platform_device(dev));
		put_device(dev);
	}
}

/*
 * Create the spi device for the keyboard and touchpad and register it with
 * the master spi device.
 */
static int appleacpi_register_spi_device(struct spi_master *spi_master,
					 struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;
	struct spi_device *spi;
	int ret;

	reg_info = acpi_driver_data(adev);

	/* check if an spi device is already registered */
	ret = bus_for_each_dev(&spi_bus_type, NULL, spi_master,
			       appleacpi_is_device_registered);
	if (ret == -EBUSY) {
		pr_info("Spi Device already registered - patched DSDT?\n");
		ret = 0;
		goto release_master;
	} else if (ret) {
		pr_err("Error checking for spi device registered: %d\n", ret);
		goto release_master;
	}

	/* none is; check if acpi device is there */
	if (acpi_bus_get_status(adev) || !adev->status.present) {
		pr_info("ACPI device is not present\n");
		ret = 0;
		goto release_master;
	}

	/*
	 * acpi device is there.
	 *
	 * First unregister any physical devices already associated with this
	 * acpi device (done by acpi_generic_device_attach).
	 */
	appleacpi_unregister_phys_devs(adev);

	/* create spi device */
	spi = spi_alloc_device(spi_master);
	if (!spi) {
		pr_err("Failed to allocate spi device\n");
		ret = -ENOMEM;
		goto release_master;
	}

	ret = appleacpi_config_spi_dev(spi, adev);
	if (ret)
		goto free_spi;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	acpi_set_modalias(adev, acpi_device_hid(adev), spi->modalias,
			  sizeof(spi->modalias));
#else
	strlcpy(spi->modalias, acpi_device_hid(adev), sizeof(spi->modalias));
#endif

	adev->power.flags.ignore_parent = true;

	ACPI_COMPANION_SET(&spi->dev, adev);
	acpi_device_set_enumerated(adev);

	/* add spi device */
	ret = spi_add_device(spi);
	if (ret) {
		adev->power.flags.ignore_parent = false;
		pr_err("Failed to add spi device: %d\n", ret);
		goto free_spi;
	}

	reg_info->spi = spi;

	pr_info("Added spi device %s\n", dev_name(&spi->dev));

	goto release_master;

free_spi:
	spi_dev_put(spi);
release_master:
	spi_master_put(spi_master);
	reg_info->spi_master = NULL;

	return ret;
}

static void appleacpi_dev_registration_worker(struct work_struct *work)
{
	struct appleacpi_spi_registration_info *info =
		container_of(work, struct appleacpi_spi_registration_info,
			     work.work);

	if (info->spi_master && !info->spi_master->running) {
		pr_debug_ratelimited("spi-master device is not running yet\n");
		schedule_delayed_work(&info->work, usecs_to_jiffies(100));
		return;
	}

	appleacpi_register_spi_device(info->spi_master, info->adev);
}

/*
 * Callback for whenever a new master spi device is added.
 */
static int appleacpi_spi_master_added(struct device *dev,
				      struct class_interface *cif)
{
	struct spi_master *spi_master =
		container_of(dev, struct spi_master, dev);
	struct appleacpi_spi_registration_info *info =
		container_of(cif, struct appleacpi_spi_registration_info, cif);
	struct acpi_device *master_adev = spi_master->dev.parent ?
		ACPI_COMPANION(spi_master->dev.parent) : NULL;

	pr_debug("New spi-master device %s (%s) with bus-number %d was added\n",
		 dev_name(&spi_master->dev),
		 master_adev ? acpi_device_hid(master_adev) : "-no-acpi-dev-",
		 spi_master->bus_num);

	if (master_adev != info->adev->parent)
		return 0;

	pr_info("Got spi-master device for device %s\n",
		acpi_device_hid(info->adev));

	/*
	 * mutexes are held here, preventing unregistering of physical devices,
	 * so need to do the actual registration in a worker.
	 */
	info->spi_master = spi_master_get(spi_master);
	schedule_delayed_work(&info->work, usecs_to_jiffies(100));

	return 0;
}

/*
 * Callback for whenever a slave spi device is added or removed.
 */
static int appleacpi_spi_slave_changed(struct notifier_block *nb,
				       unsigned long action, void *data)
{
	struct appleacpi_spi_registration_info *info =
		container_of(nb, struct appleacpi_spi_registration_info,
			     slave_notifier);
	struct spi_device *spi = data;

	pr_debug("SPI slave device changed: action=%lu, dev=%s\n",
		 action, dev_name(&spi->dev));

	switch (action) {
	case BUS_NOTIFY_DEL_DEVICE:
		if (spi == info->spi) {
			info->spi = NULL;
			return NOTIFY_OK;
		}
		break;

	default:
		break;
	}

	return NOTIFY_DONE;
}

/*
 * spi_master_class is not exported, so this is an ugly hack to get it anyway.
 */
static struct class *appleacpi_get_spi_master_class(void)
{
	struct spi_master *spi_master;
	struct device dummy;
	struct class *cls = NULL;

	memset(&dummy, 0, sizeof(dummy));

	spi_master = spi_alloc_master(&dummy, 0);
	if (spi_master) {
		cls = spi_master->dev.class;
		spi_master_put(spi_master);
	}

	return cls;
}

static int appleacpi_probe(struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;
	int ret;

	pr_debug("Probing acpi-device %s: bus-id='%s', adr=%lu, uid='%s'\n",
		 acpi_device_hid(adev), acpi_device_bid(adev),
		 acpi_device_adr(adev), acpi_device_uid(adev));

	ret = spi_register_driver(&applespi_driver);
	if (ret) {
		pr_err("Failed to register spi-driver: %d\n", ret);
		return ret;
	}

	/*
	 * Ideally we would just call spi_register_board_info() here,
	 * but that function is not exported. Additionally, we need to
	 * perform some extra work during device creation, such as
	 * unregistering physical devices. So instead we have do the
	 * registration ourselves. For that we see if our spi-master
	 * has been registered already, and if not jump through some
	 * hoops to make sure we are notified when it does.
	 */

	reg_info = kzalloc(sizeof(*reg_info), GFP_KERNEL);
	if (!reg_info) {
		ret = -ENOMEM;
		goto unregister_driver;
	}

	reg_info->adev = adev;
	INIT_DELAYED_WORK(&reg_info->work, appleacpi_dev_registration_worker);

	adev->driver_data = reg_info;

	/*
	 * Set up listening for spi slave removals so we can properly
	 * handle them.
	 */
	reg_info->slave_notifier.notifier_call =
		appleacpi_spi_slave_changed;
	ret = bus_register_notifier(&spi_bus_type,
				    &reg_info->slave_notifier);
	if (ret) {
		pr_err("Failed to register notifier for spi slaves: %d\n", ret);
		goto free_reg_info;
	}

	/*
	 * Listen for additions of spi-master devices so we can register our spi
	 * device when the relevant master is added.  Note that our callback
	 * gets called immediately for all existing master devices, so this
	 * takes care of registration when the master already exists too.
	 */
	reg_info->cif.class = appleacpi_get_spi_master_class();
	reg_info->cif.add_dev = appleacpi_spi_master_added;

	ret = class_interface_register(&reg_info->cif);
	if (ret) {
		pr_err("Failed to register watcher for spi-master: %d\n", ret);
		goto unregister_notifier;
	}

	if (!reg_info->spi_master) {
		pr_info("No spi-master device found for device %s - waiting for it to be registered\n",
			acpi_device_hid(adev));
	}

	pr_info("acpi-device probe done: %s\n", acpi_device_hid(adev));

	return 0;

unregister_notifier:
	bus_unregister_notifier(&spi_bus_type, &reg_info->slave_notifier);
free_reg_info:
	adev->driver_data = NULL;
	kfree(reg_info);
unregister_driver:
	spi_unregister_driver(&applespi_driver);
	return ret;
}

static int appleacpi_remove(struct acpi_device *adev)
{
	struct appleacpi_spi_registration_info *reg_info;

	reg_info = acpi_driver_data(adev);
	if (reg_info) {
		class_interface_unregister(&reg_info->cif);
		bus_unregister_notifier(&spi_bus_type,
					&reg_info->slave_notifier);
		cancel_delayed_work_sync(&reg_info->work);
		if (reg_info->spi)
			spi_unregister_device(reg_info->spi);
		kfree(reg_info);
	}

	spi_unregister_driver(&applespi_driver);

	pr_info("acpi-device remove done: %s\n", acpi_device_hid(adev));

	return 0;
}

static struct acpi_driver appleacpi_driver = {
	.name		= "appleacpi",
	.class		= "topcase", /* ? */
	.owner		= THIS_MODULE,
	.ids		= ACPI_PTR(applespi_acpi_match),
	.ops		= {
		.add		= appleacpi_probe,
		.remove		= appleacpi_remove,
	},
};

module_acpi_driver(appleacpi_driver)

#else

module_spi_driver(applespi_driver)

#endif

MODULE_LICENSE("GPL");
