/*
 * libliuqinsetup: make SpringBoard treat the device as activated and refuse the
 * first-boot setup assistant.  Same two hooks as HackiOS' inject_sb.c (MIT),
 * without its iOS 16.5 address checks: on this 16.6.1 root the classes are
 * looked up and the methods replaced by name.
 *
 *   -[SBLockdownManager _setupActivationState] keeps the answer in an int at +8:
 *   2 = activated, 1 = not, 3 = unknown.  With no answer SpringBoard shows the
 *   setup ("hello") instead of the lock screen.
 *   -[SBSetupManager _setSetupRequiredReason:] with 0 means no setup assistant.
 *
 * The library is inserted into SpringBoard by /usr/libexec/liuqin-springboard.
 */
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <mach-o/dyld.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof line, format, ap);
	va_end(ap);
	if (n > (int)sizeof line - 1) {
		n = (int)sizeof line - 1;
	}
	write(2, line, (size_t)n);
}

#define LOCKDOWN_STATE(manager) (*(int *)((char *)(manager) + 8))
#define LOCKDOWN_ACTIVATED 2

static void
setup_activation_state(id self, __unused SEL selector)
{
	LOCKDOWN_STATE(self) = LOCKDOWN_ACTIVATED;
	say("liuqin setup: activation state -> 2 (activated)\n");
}

static void (*original_set_setup_required_reason)(id, SEL, long);

static void
set_setup_required_reason(id self, SEL selector, long reason)
{
	if (reason != 0) {
		say("liuqin setup: setup reason %ld -> 0\n", reason);
	}
	if (original_set_setup_required_reason != NULL) {
		original_set_setup_required_reason(self, selector, 0);
	}
}

static int patched_activation, patched_reason;

static void
patch_classes(void)
{
	if (!patched_activation) {
		Class lock = objc_getClass("SBLockdownManager");
		Method activation = lock != Nil ? class_getInstanceMethod(lock, sel_registerName("_setupActivationState")) : NULL;

		if (activation != NULL) {
			method_setImplementation(activation, (IMP)setup_activation_state);
			patched_activation = 1;
			say("liuqin setup: SBLockdownManager activation patched\n");
		}
	}
	if (!patched_reason) {
		Class setup = objc_getClass("SBSetupManager");
		Method reason = setup != Nil ? class_getInstanceMethod(setup, sel_registerName("_setSetupRequiredReason:")) : NULL;

		if (reason != NULL) {
			original_set_setup_required_reason = (void (*)(id, SEL, long))method_getImplementation(reason);
			method_setImplementation(reason, (IMP)set_setup_required_reason);
			patched_reason = 1;
			say("liuqin setup: SBSetupManager reason patched\n");
		}
	}
}

static void
image_added(const struct mach_header *header __unused, intptr_t slide __unused)
{
	patch_classes();
}

__attribute__((constructor)) static void
liuqin_setup_init(void)
{
	say("liuqin setup: loaded\n");
	patch_classes();
	_dyld_register_func_for_add_image(image_added);
	say("liuqin setup: activation %s, setup reason %s\n",
	    patched_activation ? "patched" : "not yet", patched_reason ? "patched" : "not yet");
}
