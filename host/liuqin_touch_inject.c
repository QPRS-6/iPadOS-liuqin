/*
 * libliuqintouch: the touch screen and the buttons for backboardd.
 *
 * iOS expects touches from Apple's multitouch driver through IOHIDFamily, which
 * this port does not have.  Instead the kernel touch driver posts samples to
 * /dev/liuqin-touch (bsd/dev/liuqin_input.c) and this library, injected into
 * backboardd, turns them into the digitizer/keyboard events that driver would
 * have sent - through the event server's own virtual-service API.
 *
 * Adapted from ios-sweet's sibling project hackios (oosexgang/hackios-xnu-userland,
 * MIT): its libredwood.dylib solves the same problem on iOS 16.5.  The event
 * construction and the retry-until-opened logic follow it; the transport is our
 * character device.
 *
 * Build: tools/build-inject.sh (arm64, iOS 16).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOUCH_DEVICE            "/dev/liuqin-touch"

#define TOUCH_DOWN              1
#define TOUCH_MOVE              2
#define TOUCH_UP                3
#define BUTTON_DOWN             4
#define BUTTON_UP               5

#define BUTTON_LOCK             1
#define BUTTON_VOLUME_UP        2
#define BUTTON_VOLUME_DOWN      3
#define BUTTON_HOME             4

struct liuqin_touch_sample {
	uint64_t time;
	uint32_t type;
	uint32_t finger;
	uint32_t x, y;
};

/* IOKit's HID event system: no public header on iOS, prototypes as in 16.x */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

struct virtual_service_callbacks {
	uint64_t version;
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

static void say(const char *format, ...);

/*
 * The private IOKit HID symbols have no public header and no .tbd in the SDK.
 * Linking against a hand-made stub instead records it as a load-time dependency
 * ("Library not loaded: /usr/lib/libLiUqinHIDPrivate.dylib", and backboardd
 * crash-loops), so they are looked up at run time from the shared cache.
 */
#include <dlfcn.h>

static struct {
	IOHIDEventSystemClientRef (*eventSystemClientCreateWithType)(CFAllocatorRef, int, CFDictionaryRef);
	void (*eventSystemClientScheduleWithDispatchQueue)(IOHIDEventSystemClientRef, dispatch_queue_t);
	IOHIDServiceClientRef (*virtualServiceClientCreateWithCallbacks)(IOHIDEventSystemClientRef, CFDictionaryRef,
	    const struct virtual_service_callbacks *, void *, void *);
	Boolean (*virtualServiceClientRemove)(IOHIDServiceClientRef);
	Boolean (*virtualServiceClientDispatchEvent)(IOHIDServiceClientRef, IOHIDEventRef);
	IOHIDEventRef (*eventCreateDigitizerEvent)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t,
	    uint32_t, double, double, double, double, double, double, Boolean, Boolean, uint32_t);
	IOHIDEventRef (*eventCreateDigitizerFingerEvent)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, uint32_t,
	    double, double, double, double, double, Boolean, Boolean, uint32_t);
	IOHIDEventRef (*eventCreateKeyboardEvent)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, Boolean, uint32_t);
	void (*eventAppendEvent)(IOHIDEventRef, IOHIDEventRef, uint32_t);
	void (*eventSetIntegerValue)(IOHIDEventRef, uint32_t, CFIndex);
} hid;

static void *hidLookup(void *image, const char *name)
{
	void *symbol = dlsym(image, name);

	if (symbol == NULL) {
		say("missing symbol %s\n", name);
	}
	return symbol;
}

static bool
hidResolve(void)
{
	static const char *const images[] = {
		"/System/Library/Frameworks/IOKit.framework/IOKit",
		"/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
		NULL,
	};
	void *image = NULL;

	for (int i = 0; images[i] != NULL && image == NULL; i++) {
		image = dlopen(images[i], RTLD_NOW | RTLD_GLOBAL);
	}
	if (image == NULL) {
		image = RTLD_DEFAULT;
	}
	hid.eventSystemClientCreateWithType = hidLookup(image, "IOHIDEventSystemClientCreateWithType");
	hid.eventSystemClientScheduleWithDispatchQueue = hidLookup(image, "IOHIDEventSystemClientScheduleWithDispatchQueue");
	hid.virtualServiceClientCreateWithCallbacks = hidLookup(image, "IOHIDVirtualServiceClientCreateWithCallbacks");
	hid.virtualServiceClientRemove = hidLookup(image, "IOHIDVirtualServiceClientRemove");
	hid.virtualServiceClientDispatchEvent = hidLookup(image, "IOHIDVirtualServiceClientDispatchEvent");
	hid.eventCreateDigitizerEvent = hidLookup(image, "IOHIDEventCreateDigitizerEvent");
	hid.eventCreateDigitizerFingerEvent = hidLookup(image, "IOHIDEventCreateDigitizerFingerEvent");
	hid.eventCreateKeyboardEvent = hidLookup(image, "IOHIDEventCreateKeyboardEvent");
	hid.eventAppendEvent = hidLookup(image, "IOHIDEventAppendEvent");
	hid.eventSetIntegerValue = hidLookup(image, "IOHIDEventSetIntegerValue");
	return hid.eventSystemClientCreateWithType && hid.virtualServiceClientCreateWithCallbacks &&
	    hid.virtualServiceClientDispatchEvent && hid.eventCreateDigitizerEvent &&
	    hid.eventCreateDigitizerFingerEvent && hid.eventAppendEvent && hid.eventSetIntegerValue &&
	    hid.eventSetIntegerValue && hid.eventCreateKeyboardEvent && hid.virtualServiceClientRemove &&
	    hid.eventSystemClientScheduleWithDispatchQueue;
}

#define HID_CLIENT_SIMPLE               4
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30
#define HID_USAGE_MENU                  0x40
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3
#define DIGITIZER_RANGE                 0x1
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

/* Edge contacts: UIKit's screen-edge gestures take no touch that is not marked. */
#define EDGE_SIDE       (16.0 / 375)
#define EDGE_TOP        (16.0 / 812)
#define EDGE_BOTTOM     (34.0 / 812)

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n, fd;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	/* Console writes would draw text over the UI; the line still goes to
	 * stderr, which the kernel's /dev/null tee keeps for the log ring. */
	write(2, line, (size_t)n);
}


static void send_touch(const struct liuqin_touch_sample *t);
static void release_stuck(void);

static uint64_t g_last_sample;          /* when the driver last sent anything */
static unsigned long g_samples, g_ups, g_dispatch_failures;

/*
 * Nothing for 250 ms with a finger still down: the driver will not send the lift
 * (the finger left the panel, or its frames stopped).  Release everything so
 * iOS does not keep a finger pressed.
 */
/*
 * The character device has no select/poll implementation, so the watchdog runs
 * on a thread of its own next to the blocking reader.
 */
static void *
watchdog(void *argument __unused)
{
	for (;;) {
		usleep(100 * 1000);
		release_stuck();
	}
	return NULL;
}

static void
release_stuck(void)
{
	uint64_t now = mach_absolute_time();
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		(void)mach_timebase_info(&tb);
	}
	if (g_last_sample == 0) {
		return;
	}
	uint64_t ns = (now - g_last_sample) * tb.numer / tb.denom;

	if (ns < 250ull * 1000 * 1000) {
		return;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (fingers[i].touching) {
			struct liuqin_touch_sample t = {
				.time = now, .type = TOUCH_UP, .finger = i,
				.x = (uint32_t)(fingers[i].x * 65535.0), .y = (uint32_t)(fingers[i].y * 65535.0),
			};

			send_touch(&t);
			g_ups++;
		}
	}
	g_last_sample = 0;
	say("released stuck fingers\n");
}

/* A line every 5 s: says whether the driver is still delivering samples. */
static void
status(void)
{
	static uint64_t next;
	uint64_t now = mach_absolute_time();
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		(void)mach_timebase_info(&tb);
	}
	uint64_t ns = (now - next) * tb.numer / tb.denom;

	if (next != 0 && ns < 60000ull * 1000 * 1000) {
		return;
	}
	next = now;
	say("status: %lu samples, %lu ups, %lu dispatch failures\n", g_samples, g_ups, g_dispatch_failures);
}


struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
	IOHIDServiceClientRef client;
	dispatch_queue_t queue;
};

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	return value ? CFRetain(value) : NULL;
}

static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
		say("service opened by the event server\n");
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties, pair;
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	if (made == NULL || name == NULL) {
		return NULL;
	}
	properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("Liuqin"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	made->properties = properties;
	for (int attempt = 1; attempt <= 90; attempt++) {
		made->opened = false;
		service = hid.virtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			/* The window server may start before the HID event system and
			 * the kernel's HID service are ready (metal3: backboardd at 50 s,
			 * create failed at 57 s; the old root's backboardd at 79 s
			 * succeeded).  Retry instead of giving up on the first NULL. */
			if (attempt == 1 || attempt % 10 == 0)
				say("create %s retry %d\n", product, attempt);
			usleep(1000 * 1000);
			continue;
		}
		for (int tenths = 0; tenths < 5 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			say("%s ready\n", product);
			break;
		}
		say("event server did not open %s (attempt %d)\n", product, attempt);
		hid.virtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	CFRelease(pair);
	CFRelease(pairs);
	CFRelease(page_number);
	CFRelease(usage_number);
	CFRelease(zero);
	CFRelease(name);
	return service;
}

static IOHIDServiceClientRef touch_screen, buttons;

static void
send_touch(const struct liuqin_touch_sample *t)
{
	uint32_t change = t->type == TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (touch_screen == NULL || f >= MAX_FINGERS || (t->type != TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	g_last_sample = t->time;
	g_samples++;
	if (t->type == TOUCH_UP) {
		g_ups++;
	}
	fingers[f].touching = t->type != TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = hid.eventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0,
	    fingers[f].x, fingers[f].y, 0, 0, 0, 0, any, any, 0);
	if (hand == NULL) {
		return;
	}
	hid.eventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	hid.eventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = hid.eventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		if (finger != NULL) {
			hid.eventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
			hid.eventAppendEvent(hand, finger, 0);
			CFRelease(finger);
		}
	}
	{
		static unsigned long seen;
		bool ok = hid.virtualServiceClientDispatchEvent(touch_screen, hand);

		if (!ok) {
			g_dispatch_failures++;
		}
		/* First few events, then every 50th: enough to tell "no events at
		 * all" from "events dispatched but ignored". */
		if (seen < 5 || (seen % 50) == 0) {
			say("event %lu: type %u finger %u %.3f,%.3f -> dispatch %s\n",
			    seen, (unsigned)t->type, (unsigned)f, fingers[f].x, fingers[f].y,
			    ok ? "ok" : "FAILED");
		}
		seen++;
	}
	CFRelease(hand);
}

static void
send_button(const struct liuqin_touch_sample *t)
{
	uint32_t usage = t->finger == BUTTON_LOCK ? HID_USAGE_POWER : t->finger == BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == BUTTON_VOLUME_DOWN ? HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (buttons == NULL || usage == 0) {
		return;
	}
	event = hid.eventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == BUTTON_DOWN, 0);
	if (event != NULL) {
		hid.eventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
		hid.virtualServiceClientDispatchEvent(buttons, event);
		CFRelease(event);
	}
}

static void *
reader(void *unused __unused)
{
	struct liuqin_touch_sample samples[16];
	ssize_t got;
	int fd;

	for (;;) {
		if ((fd = open(TOUCH_DEVICE, O_RDONLY)) < 0) {
			usleep(100000);
			continue;
		}
		say("reading %s\n", TOUCH_DEVICE);
		while ((got = read(fd, samples, sizeof(samples))) > 0) {
			for (size_t i = 0; i < (size_t)got / sizeof(samples[0]); i++) {
				if (samples[i].type >= BUTTON_DOWN) {
					send_button(&samples[i]);
				} else {
					send_touch(&samples[i]);
				}
			}
			status();
		}
		close(fd);
		usleep(100000);
	}
	return NULL;
}

/*
 * Diagnostic: the Metal framework reads MetalPluginName/MetalPluginClassName
 * from the accelerator but never calls the plugin's initWithAcceleratorPort:
 * on this root.  Load the bundle here, in backboardd (a process the accelerator
 * grants), and call that method directly.  If the bundle loads, this proves the
 * remaining gap is the framework/plugin discovery rather than the bundle, AMFI
 * or the class; the plugin's own test thread then opens type 0x4147.
 */
static int
copy_file(const char *src, const char *dst)
{
	char buffer[65536];
	ssize_t n;
	int in, out;

	in = open(src, O_RDONLY);
	if (in < 0) {
		return -1;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buffer, sizeof buffer)) > 0) {
		ssize_t at = 0;
		while (at < n) {
			ssize_t wrote = write(out, buffer + at, (size_t)(n - at));
			if (wrote <= 0) {
				close(in);
				close(out);
				return -1;
			}
			at += wrote;
		}
	}
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

static void *
metal_probe_thread(void *argument __unused)
{
	void *iokit, *objc, *plugin;
	void *(*matching)(const char *);
	unsigned (*getService)(unsigned, void *);
	Class (*getClass)(const char *);
	SEL (*sel)(const char *);
	void *msgSend;
	Class cls;
	unsigned service;

	/* After the accelerator is published and backboardd is in its run loop. */
	sleep(20);
	say("metal probe: start\n");
	{
		const char *path = "/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal";
		struct stat st;
		unsigned char hdr[16] = {};
		int fd = open(path, O_RDONLY);
		if (fd < 0) {
			say("metal probe: open failed %d\n", errno);
		} else {
			ssize_t n = read(fd, hdr, sizeof hdr);
			int saved = errno;
			close(fd);
			if (lstat(path, &st) == 0) {
				say("metal probe: size %lld mode 0%o\n", (long long)st.st_size, st.st_mode);
			}
			if (n > 0) {
				char hex[64];
				int k = 0;
				for (int i = 0; i < n && k < (int)sizeof hex - 4; i++) {
					k += snprintf(hex + k, sizeof hex - (size_t)k, "%02x", hdr[i]);
				}
				say("metal probe: hdr %s n=%ld\n", hex, (long)n);
			} else {
				say("metal probe: read n=%ld errno %d\n", (long)n, saved);
			}
		}
	}
	iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
	if (iokit == NULL) {
		say("metal probe: no IOKit: %s\n", dlerror());
		return NULL;
	}
	matching = (void *(*)(const char *))dlsym(iokit, "IOServiceMatching");
	getService = (unsigned (*)(unsigned, void *))dlsym(iokit, "IOServiceGetMatchingService");
	objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
	if (objc == NULL) {
		say("metal probe: no libobjc: %s\n", dlerror());
		return NULL;
	}
	getClass = (Class (*)(const char *))dlsym(objc, "objc_getClass");
	sel = (SEL (*)(const char *))dlsym(objc, "sel_registerName");
	msgSend = dlsym(objc, "objc_msgSend");
	if (matching == NULL || getService == NULL || getClass == NULL || sel == NULL || msgSend == NULL) {
		say("metal probe: resolve failed\n");
		return NULL;
	}
	plugin = dlopen("/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal", RTLD_NOW | RTLD_LOCAL);
	if (plugin == NULL) {
		say("metal probe: plugin dlopen failed: %s\n", dlerror());
		/* The touch library loads fine from /var/tmp; try a fresh copy there,
		 * which also distinguishes a bad file/extent from dyld/signature. */
		if (copy_file("/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",
		    "/var/tmp/NativeAdrenoMetal.probe") == 0) {
			say("metal probe: copied to /var/tmp/NativeAdrenoMetal.probe\n");
			plugin = dlopen("/var/tmp/NativeAdrenoMetal.probe", RTLD_NOW | RTLD_LOCAL);
			if (plugin == NULL) {
				say("metal probe: copy dlopen failed: %s\n", dlerror());
			}
		} else {
			say("metal probe: copy failed\n");
		}
		if (plugin == NULL) {
			return NULL;
		}
	}
	cls = getClass("NativeAdrenoMetalDevice");
	if (cls == NULL) {
		say("metal probe: NativeAdrenoMetalDevice missing\n");
		return NULL;
	}
	service = getService(0, matching("IOAcceleratorES"));
	if (service == 0) {
		say("metal probe: no IOAcceleratorES service\n");
		return NULL;
	}
	{
		/* Do not call initWithAcceleratorPort: here: that starts the plugin's GPU test, and two tests at once (this probe plus the
		 * discovery's own) make the log ambiguous. The two opens below prove the file loaded and both driver paths work. */
		typedef int (*open_fn)(unsigned, unsigned, uint32_t, unsigned *);
		typedef int (*close_fn)(unsigned);
		open_fn openFn = (open_fn)dlsym(iokit, "IOServiceOpen");
		close_fn closeFn = (close_fn)dlsym(iokit, "IOServiceClose");
		unsigned conn;
		if (openFn == NULL) {
			say("metal probe: no IOServiceOpen\n");
		} else {
			conn = 0;
			say("metal probe: open 0x4147 rc %d conn 0x%x\n",
			    openFn(service, mach_task_self(), 0x4147, &conn), conn);
			if (conn != 0 && closeFn != NULL) {
				closeFn(conn);
			}
			conn = 0;
			say("metal probe: open type 1 rc %d conn 0x%x\n",
			    openFn(service, mach_task_self(), 1, &conn), conn);
			if (conn != 0 && closeFn != NULL) {
				closeFn(conn);
			}
		}
	}
	say("metal probe: done (init not called)\n");
	return NULL;
}

/*
 * Set-up runs on a thread of its own: this library is loaded into the window
 * server during dyld start-up, and publishing a service waits for the event
 * server to open it.  Doing that on the main thread stops the window server
 * from ever reaching its run loop (no picture at all), so the constructor only
 * starts the thread and returns.
 */
static void *
setup_thread(void *argument __unused)
{
	IOHIDEventSystemClientRef client;
	dispatch_queue_t queue;
	pthread_t thread;

	/* Give the window server time to finish starting. */
	usleep(3 * 1000 * 1000);
	if (!hidResolve()) {
		return NULL;
	}
	client = hid.eventSystemClientCreateWithType(NULL, HID_CLIENT_SIMPLE, NULL);
	if (client == NULL) {
		say("no HID event system client\n");
		return NULL;
	}
	queue = dispatch_queue_create("liuqin.touch", NULL);
	if (queue != NULL) {
		hid.eventSystemClientScheduleWithDispatchQueue(client, queue);
		dispatch_release(queue);
	} else {
		hid.eventSystemClientScheduleWithDispatchQueue(client, dispatch_get_main_queue());
	}
	touch_screen = make_service(client, "Liuqin Touch Screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
	buttons = make_service(client, "Liuqin Buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	if (touch_screen == NULL) {
		say("no touch screen service\n");
	}
	if (pthread_create(&thread, NULL, reader, NULL) != 0) {
		say("no reader thread\n");
	}
	if (pthread_create(&thread, NULL, watchdog, NULL) != 0) {
		say("no watchdog thread\n");
	}
	return NULL;
}

__attribute__((constructor)) static void
liuqin_touch_init(void)
{
	pthread_t thread;

	if (pthread_create(&thread, NULL, setup_thread, NULL) != 0) {
		say("no setup thread\n");
	}
	if (pthread_create(&thread, NULL, metal_probe_thread, NULL) != 0) {
		say("no metal probe thread\n");
	}
}
