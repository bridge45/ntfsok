PROJECT := xnu/darwintests

# When building as part of xnu_tests, we get passed a DSTROOT that's got the
# unit test path in it already.  But, BASEDSTROOT doesn't, so use that instead.
ifdef BASEDSTROOT
override DSTROOT = $(BASEDSTROOT)
endif

INVALID_ARCHS = i386
ENABLE_LTE_TESTS=YES

OTHER_LTE_INCLUDE_FILES += \
	/System/Library/PrivateFrameworks/LoggingSupport.framework, \
	/System/Library/PrivateFrameworks/MobileKeyBag.framework, \
	/System/Library/Frameworks/IOSurface.framework, \
	/usr/local/lib/libdarwintest_utils.dylib, \
	/usr/lib/libapple_crypto.dylib,

DEVELOPER_DIR ?= $(shell xcode-select -p)

# the xnu build system will only ever call us with the default target
.DEFAULT_GOAL := install

include $(DEVELOPER_DIR)/AppleInternal/Makefiles/darwintest/Makefile.common

OTHER_CFLAGS  = -Weverything -Wno-gnu-union-cast -Wno-missing-field-initializers -Wno-partial-availability
OTHER_CFLAGS += -Wno-missing-noreturn -Wno-vla -Wno-reserved-id-macro -Wno-documentation-unknown-command
OTHER_CFLAGS += -Wno-padded -Wno-used-but-marked-unused -Wno-covered-switch-default -Wno-nullability-extension
OTHER_CFLAGS += -Wno-gnu-empty-initializer -Wno-unused-macros -Wno-undef -Wno-fixed-enum-extension
OTHER_CFLAGS += -Wno-gnu-auto-type -Wno-switch-enum -Wno-variadic-macros
OTHER_CFLAGS += --std=gnu11 -isystem $(SDKROOT)/System/Library/Frameworks/System.framework/PrivateHeaders
OTHER_CFLAGS += -UT_NAMESPACE_PREFIX -DT_NAMESPACE_PREFIX=xnu
OTHER_CFLAGS += -F $(SDKROOT)/System/Library/PrivateFrameworks

OTHER_CFLAGS += -Wl,-sectcreate,__INFO_FILTER,__disable,/dev/null


CODESIGN:=$(shell xcrun -sdk "$(TARGETSDK)" -find codesign)
CODESIGN_HARDENED_RUNTIME:=$(CODESIGN) -o runtime
CODESIGN_ALLOCATE:=$(shell xcrun -sdk "$(TARGETSDK)" -find codesign_allocate)

ifeq ($(PLATFORM),iPhoneOS)
  IOS_TEST_COMPAT = YES
else
  IOS_TEST_COMPAT = NO
endif

# to have custom compiler flags to
# target: OTHER_CFLAGS += <my flags>

atm_diagnostic_flag: OTHER_CFLAGS += drop_priv.c

atm_diagnostic_flag_entitled: CODE_SIGN_ENTITLEMENTS = atm_diagnostic_flag.entitlements
atm_diagnostic_flag_entitled: OTHER_CFLAGS += drop_priv.c

avx: INVALID_ARCHS = $(filter arm%,$(ARCH_CONFIGS))
avx: OTHER_CFLAGS += -mavx512f -mavx512bw -mavx512vl
avx: OTHER_CFLAGS += -I$(SDKROOT)/System/Library/Frameworks/System.framework/PrivateHeaders
avx: CONFIG_FLAGS := $(filter-out -O%,$(CONFIG_FLAGS))
# Level 2 optimization must be used to prevent compiler from generating
# invalid instructions when compiling with AVX-512 flags.
avx: CONFIG_FLAGS += -O2
# Disable vzeroupper insertion to work around rdar://problem/35035096
avx: CONFIG_FLAGS += -mllvm -x86-use-vzeroupper=0
ifneq (osx,$(TARGET_NAME))
EXCLUDED_SOURCES += avx.c
endif

CUSTOM_TARGETS = sr_entitlement_helper

sr_entitlement_helper: sr_entitlement_helper.c
	mkdir -p $(SYMROOT)
	$(CC) -I $(OBJROOT) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) sr_entitlement_helper.c -o $(SYMROOT)/$@
	echo $(CODESIGN) --force --sign - --timestamp=none --entitlements sr_entitlement.entitlements $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements sr_entitlement.entitlements $(SYMROOT)/$@;

install-sr_entitlement_helper: sr_entitlement_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/sr_entitlement_helper $(INSTALLDIR)

sr_entitlement: OTHER_LDFLAGS += -ldarwintest_utils

restrict_jit: CODE_SIGN_ENTITLEMENTS = restrict_jit.entitlements

backtracing_tests: OTHER_LDFLAGS += -framework CoreSymbolication
backtracing_tests: CODE_SIGN_ENTITLEMENTS = kernel_symbolication_entitlements.plist

data_protection: OTHER_LDFLAGS += -ldarwintest_utils -framework IOKit

CUSTOM_TARGETS += immovable_send_client vm_spawn_tool port_exhaustion_client port_table_limits_client fd_table_limits_client

exception_tests: excserver_protect exc_helpers.c
exception_tests: CODE_SIGN_ENTITLEMENTS = exception_tests.entitlements
exception_tests: OTHER_CFLAGS += $(OBJROOT)/excserver_protect.c
exception_tests: OTHER_CFLAGS += -I $(OBJROOT)
exception_tests: OTHER_CFLAGS += -DENTITLED=1

immovable_send: excserver
immovable_send: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)
immovable_send: OTHER_LDFLAGS += -ldarwintest_utils -lpthread -framework IOKit
immovable_send: immovable_send_client

immovable_send_client: immovable_send_client.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) immovable_send_client.c -o $(SYMROOT)/immovable_send_client

install-immovable_send_client: immovable_send_client
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/immovable_send_client $(INSTALLDIR)/

port_exhaustion: OTHER_LDFLAGS += -ldarwintest_utils -lpthread
port_exhaustion: port_exhaustion_client

port_exhaustion_client: port_exhaustion_client.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) port_exhaustion_client.c -o $(SYMROOT)/port_exhaustion_client

install-port_exhaustion_client: port_exhaustion_client
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/port_exhaustion_client $(INSTALLDIR)/

port_table_limits: rnserver
port_table_limits: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist
port_table_limits: OTHER_CFLAGS += $(OBJROOT)/rnServer.c -I $(OBJROOT)
port_table_limits: OTHER_LDFLAGS += -ldarwintest_utils -lpthread
port_table_limits: port_table_limits_client

port_table_limits_client: port_table_limits_client.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) port_table_limits_client.c -o $(SYMROOT)/port_table_limits_client
	$(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@

install-port_table_limits_client: port_table_limits_client
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/port_table_limits_client $(INSTALLDIR)/

fd_table_limits: rnserver
fd_table_limits: OTHER_CFLAGS += $(OBJROOT)/rnServer.c -I $(OBJROOT)
fd_table_limits: OTHER_LDFLAGS += -ldarwintest_utils -lpthread
fd_table_limits: fd_table_limits_client

fd_table_limits_client: fd_table_limits_client.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) fd_table_limits_client.c -o $(SYMROOT)/fd_table_limits_client
	$(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@

install-fd_table_limits_client: fd_table_limits_client
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/fd_table_limits_client $(INSTALLDIR)/

vm_spawn_tool: INVALID_ARCHS = i386
vm_spawn_tool: vm_spawn_tool.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) vm_spawn_tool.c -o $(SYMROOT)/vm_spawn_tool

install-vm_spawn_tool: vm_spawn_tool
	mkdir -p $(INSTALLDIR)/tools
	cp $(SYMROOT)/vm_spawn_tool $(INSTALLDIR)/tools/

CUSTOM_TARGETS += imm_pinned_control_port_crasher

imm_pinned_control_port: excserver
imm_pinned_control_port: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist
imm_pinned_control_port: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)
imm_pinned_control_port: OTHER_LDFLAGS += -ldarwintest_utils -lpthread
imm_pinned_control_port: imm_pinned_control_port_crasher

imm_pinned_control_port_crasher: imm_pinned_control_port_crasher.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) imm_pinned_control_port_crasher.c -o $(SYMROOT)/imm_pinned_control_port_crasher
	$(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@

install-imm_pinned_control_port_crasher: imm_pinned_control_port_crasher
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/imm_pinned_control_port_crasher $(INSTALLDIR)/

kas_info: OTHER_LDFLAGS += -framework CoreSymbolication
kas_info: CODE_SIGN_ENTITLEMENTS = kernel_symbolication_entitlements.plist

INCLUDED_TEST_SOURCE_DIRS += ktrace

ktrace/kdebug_tests: INVALID_ARCHS = i386
ktrace/kdebug_tests: OTHER_LDFLAGS = -framework ktrace -ldarwintest_utils -framework kperf
ktrace/kdebug_tests: OTHER_CFLAGS += test_utils.c

ktrace/kdebug_coproc_tests: INVALID_ARCHS = i386
ktrace/kdebug_coproc_tests: OTHER_LDFLAGS = -framework ktrace -ldarwintest_utils -framework kperf
ktrace/kdebug_coproc_tests: OTHER_CFLAGS += test_utils.c

ktrace/kpc_tests: OTHER_LDFLAGS += -framework kperf -framework ktrace
ktrace/kpc_tests: INVALID_ARCHS = i386

ktrace/kperf_tests: OTHER_CFLAGS += ktrace/kperf_helpers.c drop_priv.c
ktrace/kperf_tests: OTHER_LDFLAGS += -framework kperf -framework kperfdata -framework ktrace -ldarwintest_utils

ktrace/kperf_backtracing_tests: OTHER_CFLAGS += ktrace/kperf_helpers.c
ktrace/kperf_backtracing_tests: OTHER_LDFLAGS += -framework kperf -framework kperfdata -framework ktrace
ktrace/kperf_backtracing_tests: OTHER_LDFLAGS += -framework CoreSymbolication
ktrace/kperf_backtracing_tests: CODE_SIGN_ENTITLEMENTS = kernel_symbolication_entitlements.plist

EXCLUDED_SOURCES += drop_priv.c ktrace/kperf_helpers.c xnu_quick_test_helpers.c memorystatus_assertion_helpers.c bpflib.c in_cksum.c test_utils.c inet_transfer.c

ifneq ($(IOS_TEST_COMPAT),YES)
EXCLUDED_SOURCES += jumbo_va_spaces_28530648.c perf_compressor.c memorystatus_freeze_test.c vm/entitlement_increased_memory_limit.c
endif

perf_compressor: OTHER_LDFLAGS += -ldarwintest_utils
perf_compressor: CODE_SIGN_ENTITLEMENTS=./private_entitlement.plist


memorystatus_freeze_test: CODE_SIGN_ENTITLEMENTS=./task_for_pid_entitlement.plist
memorystatus_freeze_test: OTHER_LDFLAGS += -ldarwintest_utils
memorystatus_freeze_test: OTHER_CFLAGS += -ldarwintest_utils memorystatus_assertion_helpers.c

memorystatus_is_assertion: OTHER_LDFLAGS += -ldarwintest_utils
memorystatus_is_assertion: OTHER_CFLAGS += memorystatus_assertion_helpers.c

memorystatus_vm_map_fork: OTHER_CFLAGS += test_utils.c

shared_cache_tests: OTHER_LDFLAGS += -ldarwintest_utils

stackshot_tests: OTHER_CFLAGS += -Wno-objc-messaging-id
stackshot_tests: OTHER_LDFLAGS += -lkdd -lz -ldarwintest_utils -framework Foundation

stackshot_accuracy: OTHER_CFLAGS += -ldarwintest_utils -Wno-objc-messaging-id
stackshot_accuracy: OTHER_LDFLAGS += -lkdd -ldarwintest_utils -framework Foundation
stackshot_accuracy: INVALID_ARCHS = i386

ifeq ($(PLATFORM),MacOSX)
CUSTOM_TARGETS += stackshot_translated_child

stackshot_translated_child: INVALID_ARCHS = arm64 arm64e
stackshot_translated_child: stackshot_translated_child.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) stackshot_translated_child.c -o $(SYMROOT)/stackshot_translated_child

install-stackshot_translated_child: stackshot_translated_child
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/stackshot_translated_child $(INSTALLDIR)/
else
EXCLUDED_SOURCES += stackshot_translated_child.c
endif

telemetry: OTHER_LDFLAGS = -framework ktrace -framework kperf -framework CoreFoundation

memorystatus_zone_test: OTHER_CFLAGS += -isystem $(SDKROOT)/System/Library/Frameworks/System.framework/PrivateHeaders
memorystatus_zone_test: OTHER_LDFLAGS += -framework ktrace
memorystatus_zone_test: OTHER_LDFLAGS += -ldarwintest_utils

memcmp_zero: OTHER_CFLAGS += ../osfmk/arm64/memcmp_zero.s

text_corruption: OTHER_LDFLAGS += -ldarwintest_utils
CUSTOM_TARGETS += text_corruption_helper

text_corruption_helper:
	$(CC) $(LDFLAGS) $(CFLAGS) text_corruption_helper.c -lm -o $(SYMROOT)/$@;
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-text_corruption_helper: text_corruption_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/text_corruption_helper $(INSTALLDIR)/

kevent_qos: OTHER_CFLAGS += -Wno-unused-macros
kevent_qos: OTHER_CFLAGS += -I $(OBJROOT)/

mach_get_times: OTHER_LDFLAGS += -ldarwintest_utils

monotonic_core: OTHER_LDFLAGS += -framework ktrace

perfmon_unit_tests: OTHER_CFLAGS += -Wno-missing-variable-declarations -Wno-format-pedantic -Wno-language-extension-token

perf_exit: perf_exit_proc
perf_exit: OTHER_LDFLAGS = -framework ktrace -ldarwintest_utils
perf_exit: CODE_SIGN_ENTITLEMENTS=./private_entitlement.plist

CUSTOM_TARGETS += prioritize_process_launch_helper
prioritize_process_launch: prioritize_process_launch_helper

prioritize_process_launch_helper: prioritize_process_launch_helper.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) prioritize_process_launch_helper.c -o $(SYMROOT)/prioritize_process_launch_helper
	echo $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-prioritize_process_launch_helper: prioritize_process_launch_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/prioritize_process_launch_helper $(INSTALLDIR)/


perf_spawn_fork: CODE_SIGN_ENTITLEMENTS=./private_entitlement.plist

mach_exception_reply: OTHER_CFLAGS += -Wno-cast-align

os_thread_self_restrict: os_thread_self_restrict.c os_thread_self_restrict-entitlements.plist
os_thread_self_restrict: CODE_SIGN_ENTITLEMENTS=os_thread_self_restrict-entitlements.plist
os_thread_self_restrict: OTHER_LDFLAGS += -sectcreate __TEXT __info_plist os_thread_self_restrict-identifier.plist

subsystem_root_path: subsystem_root_path.c subsystem_root_path-entitlements.plist
subsystem_root_path: CODE_SIGN_ENTITLEMENTS=subsystem_root_path-entitlements.plist

EXCLUDED_SOURCES += $(wildcard bounded_ptr_src/*.cpp)
bounded_ptr: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++17
bounded_ptr: $(wildcard bounded_ptr_src/*.cpp) bounded_ptr.cpp

EXCLUDED_SOURCES += bounded_ptr_03.cpp
bounded_ptr_03: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++03
bounded_ptr_03: bounded_ptr_03.cpp

EXCLUDED_SOURCES += $(wildcard bounded_array_src/*.cpp)
bounded_array: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++17
bounded_array: $(wildcard bounded_array_src/*.cpp) bounded_array.cpp

EXCLUDED_SOURCES += $(wildcard bounded_array_ref_src/*.cpp)
bounded_array_ref: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++17
bounded_array_ref: $(wildcard bounded_array_ref_src/*.cpp) bounded_array_ref.cpp

EXCLUDED_SOURCES += $(wildcard intrusive_shared_ptr_src/*.cpp)
intrusive_shared_ptr: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++17
intrusive_shared_ptr: $(wildcard intrusive_shared_ptr_src/*.cpp) intrusive_shared_ptr.cpp

EXCLUDED_SOURCES += $(wildcard safe_allocation_src/*.cpp)
safe_allocation: OTHER_CXXFLAGS += -Werror=implicit-int-conversion -Werror=shorten-64-to-32 -I$(SRCROOT)/../libkern -std=c++17
safe_allocation: $(wildcard safe_allocation_src/*.cpp) safe_allocation.cpp

EXCLUDED_SOURCES += osptr_compat.cpp
osptr_98: OTHER_CXXFLAGS += -I$(SRCROOT)/../libkern -std=c++98 -DOSPTR_STD="98"
osptr_98: osptr_compat.cpp
	$(CXX) $(DT_CXXFLAGS) $(OTHER_CXXFLAGS) $(CXXFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
osptr_11: OTHER_CXXFLAGS += -I$(SRCROOT)/../libkern -std=c++11 -DOSPTR_STD="11"
osptr_11: osptr_compat.cpp
	$(CXX) $(DT_CXXFLAGS) $(OTHER_CXXFLAGS) $(CXXFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
osptr_14: OTHER_CXXFLAGS += -I$(SRCROOT)/../libkern -std=c++14 -DOSPTR_STD="14"
osptr_14: osptr_compat.cpp
	$(CXX) $(DT_CXXFLAGS) $(OTHER_CXXFLAGS) $(CXXFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
osptr_17: OTHER_CXXFLAGS += -I$(SRCROOT)/../libkern -std=c++17 -DOSPTR_STD="17"
osptr_17: osptr_compat.cpp
	$(CXX) $(DT_CXXFLAGS) $(OTHER_CXXFLAGS) $(CXXFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

priority_queue: OTHER_CXXFLAGS += -std=c++17
zalloc_buddy: OTHER_CFLAGS += -Wno-format-pedantic

os_refcnt: OTHER_CFLAGS += -I$(SRCROOT)/../libkern/ -Wno-gcc-compat -Wno-undef -O3 -flto

kernel_inspection: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist
kernel_inspection: OTHER_CFLAGS += -DENTITLED=1

turnstile_multihop: OTHER_CFLAGS += -Wno-unused-macros
turnstile_multihop: OTHER_CFLAGS += -I $(OBJROOT)/

CUSTOM_TARGETS += perf_exit_proc

perf_exit_proc:
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) perf_exit_proc.c -o $(SYMROOT)/perf_exit_proc

install-perf_exit_proc: perf_exit_proc
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/perf_exit_proc $(INSTALLDIR)/

stackshot_idle_25570396: OTHER_LDFLAGS += -lkdd -framework Foundation

stackshot_block_owner_14362384: OTHER_LDFLAGS += -framework Foundation -lpthread -lkdd
ifeq ($(PLATFORM),MacOSX)
stackshot_block_owner_14362384: OTHER_LDFLAGS += -lpcre
endif

all: $(DSTROOT)/usr/local/bin/kcdata

$(DSTROOT)/usr/local/bin/kcdata: $(SRCROOT)/../tools/lldbmacros/kcdata.py
	mkdir -p $(dir $@)
	cp $< $@
	chmod a+x $@

xnu_quick_test: OTHER_CFLAGS += xnu_quick_test_helpers.c

CUSTOM_TARGETS += vm_set_max_addr_helper

vm_set_max_addr_helper: vm_set_max_addr_helper.c
	$(CC) $(OTHER_CFLAGS) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) vm_set_max_addr_helper.c -o $(SYMROOT)/vm_set_max_addr_helper; \
	echo $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-vm_set_max_addr_helper: vm_set_max_addr_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/vm_set_max_addr_helper $(INSTALLDIR)/

CUSTOM_TARGETS += subsystem_root_path_helper_entitled
CUSTOM_TARGETS += subsystem_root_path_helper

subsystem_root_path_helper_entitled: subsystem_root_path_helper.c subsystem_root_path-entitlements.plist
	$(CC) $(OTHER_CFLAGS) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) subsystem_root_path_helper.c -o $(SYMROOT)/subsystem_root_path_helper_entitled; \
	echo $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements subsystem_root_path-entitlements.plist $(SYMROOT)/$@;

install-subsystem_root_path_helper_entitled: subsystem_root_path_helper_entitled
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/subsystem_root_path_helper_entitled $(INSTALLDIR)/

subsystem_root_path_helper: subsystem_root_path_helper.c
	$(CC) $(OTHER_CFLAGS) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) subsystem_root_path_helper.c -o $(SYMROOT)/subsystem_root_path_helper; \
	echo $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-subsystem_root_path_helper: subsystem_root_path_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/subsystem_root_path_helper $(INSTALLDIR)/

CUSTOM_TARGETS += vm_test_code_signing_helper

vm_test_code_signing_helper: vm_test_code_signing_helper.c
	$(CC) $(OTHER_CFLAGS) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) vm_test_code_signing_helper.c -o $(SYMROOT)/vm_test_code_signing_helper; \
	echo $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@; \
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-vm_test_code_signing_helper: vm_test_code_signing_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/vm_test_code_signing_helper $(INSTALLDIR)/

vm_test_code_signing: OTHER_LDFLAGS += -ldarwintest_utils

INCLUDED_TEST_SOURCE_DIRS += vm ipc

# Revert to legacy vm_test suite until <rdar://problem/56675212> gets solved
EXCLUDED_SOURCES += vm/vm_allocation.c

ifeq ($(IOS_TEST_COMPAT),YES)
OTHER_TEST_TARGETS += jumbo_va_spaces_28530648_unentitled jumbo_va_spaces_52551256 vm_phys_footprint_legacy vm/entitlement_increased_memory_limit vm/entitlement_increased_memory_limit_unentitled
jumbo_va_spaces_28530648: CODE_SIGN_ENTITLEMENTS = jumbo_va_spaces_28530648.entitlements
jumbo_va_spaces_28530648: OTHER_CFLAGS += -DENTITLED=1 -DTESTNAME=jumbo_va_spaces_28530648
jumbo_va_spaces_28530648: OTHER_LDFLAGS += -ldarwintest_utils
jumbo_va_spaces_28530648: OTHER_LDFLAGS += -sectcreate __TEXT __info_plist jumbo_va_spaces_28530648-identifier.plist

jumbo_va_spaces_52551256: CODE_SIGN_ENTITLEMENTS = jumbo_va_spaces_52551256.entitlements
jumbo_va_spaces_52551256: OTHER_CFLAGS += -DENTITLED=1 -DTESTNAME=jumbo_va_spaces_52551256
jumbo_va_spaces_52551256: OTHER_LDFLAGS += -ldarwintest_utils
jumbo_va_spaces_52551256: jumbo_va_spaces_28530648.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements $(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@;

jumbo_va_spaces_28530648_unentitled: OTHER_LDFLAGS += -ldarwintest_utils
jumbo_va_spaces_28530648_unentitled: OTHER_CFLAGS += -DTESTNAME=jumbo_va_spaces_28530648_unentitled
jumbo_va_spaces_28530648_unentitled: jumbo_va_spaces_28530648.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

vm/entitlement_increased_memory_limit: CODE_SIGN_ENTITLEMENTS = vm/entitlement_increased_memory_limit.entitlements
vm/entitlement_increased_memory_limit: OTHER_CFLAGS += -DENTITLED=1
vm/entitlement_increased_memory_limit: OTHER_LDFLAGS += -ldarwintest_utils memorystatus_assertion_helpers.c
vm/entitlement_increased_memory_limit: vm/entitlement_increased_memory_limit.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements $(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@;

vm/entitlement_increased_memory_limit_unentitled: OTHER_LDFLAGS += -ldarwintest_utils memorystatus_assertion_helpers.c
vm/entitlement_increased_memory_limit_unentitled: vm/entitlement_increased_memory_limit.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

vm_phys_footprint_legacy: OTHER_LDFLAGS += -framework CoreFoundation -framework IOSurface
vm_phys_footprint_legacy: OTHER_CFLAGS += -DLEGACY_FOOTPRINT_ENTITLED=1
vm_phys_footprint_legacy: CODE_SIGN_ENTITLEMENTS=./legacy_footprint.entitlement
vm_phys_footprint_legacy: vm_phys_footprint.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

endif

task_info_28439149: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist

ipc_read_inspect: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist

ipc_thread_ports_race: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist
ipc_thread_ports_race: OTHER_LDFLAGS += -ldarwintest_utils

proc_info: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist
proc_info: OTHER_LDFLAGS += -ldarwintest_utils

proc_info_list_kthreads: CODE_SIGN_ENTITLEMENTS = ./proc_info_list_kthreads.entitlements

proc_info_44873309: CODE_SIGN_ENTITLEMENTS = ./proc_info_44873309.entitlements

disk_mount_conditioner: disk_mount_conditioner*
disk_mount_conditioner: CODE_SIGN_ENTITLEMENTS=./disk_mount_conditioner-entitlements.plist
disk_mount_conditioner: OTHER_LDFLAGS += -ldarwintest_utils

disk_mount_conditioner: OTHER_TEST_TARGETS += disk_mount_conditioner_unentitled
disk_mount_conditioner_unentitled: OTHER_CFLAGS += -DTEST_UNENTITLED
disk_mount_conditioner_unentitled: OTHER_LDFLAGS += -ldarwintest_utils
disk_mount_conditioner_unentitled: disk_mount_conditioner.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

work_interval_test: CODE_SIGN_ENTITLEMENTS = work_interval_test.entitlements
work_interval_test: OTHER_CFLAGS += -DENTITLED=1

settimeofday_29193041: OTHER_CFLAGS += drop_priv.c

settimeofday_29193041_entitled: CODE_SIGN_ENTITLEMENTS = settimeofday_29193041.entitlements
settimeofday_29193041_entitled: OTHER_CFLAGS += drop_priv.c

thread_group_set_32261625: OTHER_LDFLAGS = -framework ktrace

task_info: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist
task_info: OTHER_CFLAGS += test_utils.c

extract_right_soft_fail: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist

ifneq ($(IOS_TEST_COMPAT),YES)
	EXCLUDED_SOURCES += task_vm_info_decompressions.c
endif

ifneq ($(PLATFORM),iPhoneOS)
	EXCLUDED_SOURCES += vm/compression_sweep.c
endif

tcp_bind_connect: CODE_SIGN_ENTITLEMENTS = network_entitlements.plist
socket_bind_35243417: CODE_SIGN_ENTITLEMENTS = network_entitlements.plist
socket_bind_35685803: CODE_SIGN_ENTITLEMENTS = network_entitlements.plist

net_tuntests: CODE_SIGN_ENTITLEMENTS = network_entitlements.plist

net_bridge: inet_transfer.c bpflib.c in_cksum.c
net_bridge: OTHER_LDFLAGS += -ldarwintest_utils
net_bridge: CODE_SIGN_ENTITLEMENTS = network_entitlements.plist

CUSTOM_TARGETS += posix_spawn_archpref_helper

posix_spawn_archpref_helper: posix_spawn_archpref_helper.c
	$(CC) $(LDFLAGS) $(CFLAGS) posix_spawn_archpref_helper.c -o $(SYMROOT)/$@;
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-posix_spawn_archpref_helper: posix_spawn_archpref_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/posix_spawn_archpref_helper $(INSTALLDIR)/

CUSTOM_TARGETS += posix_spawn_alt_rosetta_helper

posix_spawn_alt_rosetta_helper: posix_spawn_alt_rosetta_helper.c
	$(CC) $(LDFLAGS) $(CFLAGS) posix_spawn_alt_rosetta_helper.c -o $(SYMROOT)/$@;
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-posix_spawn_alt_rosetta_helper: posix_spawn_alt_rosetta_helper
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/posix_spawn_alt_rosetta_helper $(INSTALLDIR)/

MIG:=SDKROOT=$(SDKROOT) $(shell xcrun -sdk "$(TARGETSDK)" -find mig)

CUSTOM_TARGETS += excserver

excserver:
	$(MIG) $(CFLAGS) \
		-sheader $(OBJROOT)/excserver.h \
		-server $(OBJROOT)/excserver.c \
		-header /dev/null -user /dev/null \
		excserver.defs
install-excserver: ;

CUSTOM_TARGETS += excserver_protect

excserver_protect:
	$(MIG) $(CFLAGS) \
		-DMACH_EXC_SERVER_TASKIDTOKEN \
		-sheader $(OBJROOT)/excserver_protect.h \
		-server $(OBJROOT)/excserver_protect.c \
		-header /dev/null -user /dev/null \
		excserver_protect.defs
install-excserver_protect: ;

EXCLUDED_SOURCES += exc_helpers.c

CUSTOM_TARGETS += notifyserver

notifyserver:
	$(MIG) $(CFLAGS) \
		-DMACH_NOTIFY_SERVICE_PORT_DESTROYED_EXPECTED \
		-user /dev/null \
		-header $(OBJROOT)/notify.h \
		-sheader $(OBJROOT)/notifyServer.h \
		-server $(OBJROOT)/notifyServer.c \
		notifyserver.defs
install-notifyserver: ;

CUSTOM_TARGETS += rnserver

rnserver:
	$(MIG) $(CFLAGS) \
		-user /dev/null \
		-header $(OBJROOT)/rn.h \
		-sheader $(OBJROOT)/rnServer.h \
		-server $(OBJROOT)/rnServer.c \
		rnserver.defs
install-rnserver: ;


x18: OTHER_CFLAGS += -Wno-language-extension-token

pmap_bench: INVALID_ARCHS = $(filter-out arm%,$(ARCH_CONFIGS))

hw_breakpoint_step_arm64: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist
hw_breakpoint_step_arm64: INVALID_ARCHS = $(filter-out arm64%,$(ARCH_CONFIGS))
hw_breakpoint_step_arm64: excserver
hw_breakpoint_step_arm64: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)

exc_resource_threads: excserver
exc_resource_threads: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)

mach_service_port: notifyserver
mach_service_port: OTHER_CFLAGS += $(OBJROOT)/notifyServer.c -I $(OBJROOT)

fp_exception: excserver exc_helpers.c
fp_exception: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)

ptrauth_failure: excserver exc_helpers.c
ptrauth_failure: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT) -Wno-language-extension-token
ptrauth_failure: CODESIGN = $(CODESIGN_HARDENED_RUNTIME)

decompression_failure: excserver exc_helpers.c
decompression_failure: OTHER_CFLAGS += $(OBJROOT)/excserver.c -I $(OBJROOT)

ifeq ($(findstring arm64,$(ARCH_CONFIGS)),)
EXCLUDED_SOURCES += arm_cpu_capabilities.c
else
arm_cpu_capabilities: INVALID_ARCHS = armv7k x86_64
arm_cpu_capabilities: excserver exc_helpers.c
arm_cpu_capabilities: OTHER_CFLAGS += $(OBJROOT)/excserver.c -Wno-language-extension-token -mcpu=apple-a15
endif

ifeq ($(findstring x86_64,$(ARCH_CONFIGS)),)
EXCLUDED_SOURCES += ldt_code32.s ldt.c
else # target = osx
$(OBJROOT)/ldt_mach_exc_server.c:
	$(MIG) $(CFLAGS) \
		-user /dev/null \
		-server $(OBJROOT)/ldt_mach_exc_server.c \
		-header $(OBJROOT)/ldt_mach_exc.h \
		mach_exc.defs

ldt: INVALID_ARCHS = $(ARCH_CONFIGS)
ldt: $(OBJROOT)/ldt_mach_exc_server.c
ldt: OTHER_CFLAGS += -arch x86_64 -I $(OBJROOT) $(SRCROOT)/ldt_code32.s -Wl,-pagezero_size,0x1000 -Wno-missing-variable-declarations
ldt: CODE_SIGN_ENTITLEMENTS=ldt_entitlement.plist
endif

ifneq ($(PLATFORM),BridgeOS)
EXCLUDED_SOURCES += remote_time.c
else
remote_time: INVALID_ARCHS = armv7 armv7s arm64_32
endif

vm_phys_footprint: OTHER_LDFLAGS += -framework CoreFoundation -framework IOSurface

vm_kern_count_wired_kernelcache: OTHER_CFLAGS += -I$(SDKROOT)/System/Library/Frameworks/Kernel.framework/PrivateHeaders/mach

debug_control_port_for_pid: CODE_SIGN_ENTITLEMENTS = ./debug_control_port_for_pid_entitlement.plist

prng: OTHER_LDFLAGS += -ldarwintest_utils

preoslog: OTHER_LDFLAGS += -ldarwintest_utils
preoslog: OTHER_CFLAGS += test_utils.c

task_policy: CODE_SIGN_ENTITLEMENTS = ./task_policy_entitlement.plist

OTHER_TEST_TARGETS += task_policy_unentitled
task_policy_unentitled: CODE_SIGN_ENTITLEMENTS = task_for_pid_entitlement.plist
task_policy_unentitled: OTHER_CFLAGS += -DUNENTITLED
task_policy_unentitled: task_policy.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	$(CODESIGN) --force --sign - --timestamp=none --entitlements $(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@


port_object_description_race_77259918: OTHER_CFLAGS += -framework IOKit -framework CoreFoundation
port_object_description_race_77259918: iokit/port_object_description_race_77259918.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $^ -o $(SYMROOT)/$@
OTHER_TEST_TARGETS += port_object_description_race_77259918

EXCLUDED_SOURCES += get_shared_cache_address.c
ifneq ($(filter iPhoneOS MacOSX, $(PLATFORM)),)
CUSTOM_TARGETS += get_shared_cache_address
get_shared_cache_address: INVALID_ARCHS = arm64 i386 x86_64
get_shared_cache_address: get_shared_cache_address.c
	$(CC) $(OTHER_CFLAGS) $(CFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/get_shared_cache_address

install-get_shared_cache_address: get_shared_cache_address
	$(CODESIGN) --force --sign - --timestamp=none --identifier=com.apple.get_shared_cache_address $(SYMROOT)/get_shared_cache_address
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/get_shared_cache_address $(INSTALLDIR)/
endif

ifneq ($(PLATFORM),MacOSX)
EXCLUDED_SOURCES += hvtest_x86.m hvtest_x86_guest.c hvtest_x86_asm.s
else
EXCLUDED_SOURCES += hvtest_x86_guest.c hvtest_x86_asm.s
hvtest_x86: CODE_SIGN_ENTITLEMENTS = hv_public.entitlements
hvtest_x86: INVALID_ARCHS += arm64e arm64 i386
hvtest_x86: OTHER_CFLAGS += hvtest_x86_guest.c hvtest_x86_asm.s -framework Hypervisor -framework Foundation
endif

OTHER_TEST_TARGETS += io_catalog_send_data vm_memory_share_tests

vm_memory_share_tests: INVALID_ARCHS = i386
vm_memory_share_tests: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist
vm_memory_share_tests: vm_memory_tests_src/main.c vm_memory_tests_src/common.c vm_memory_tests_src/vm_tests.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $^ -o $(SYMROOT)/$@
	echo $(CODESIGN) --force --sign - --timestamp=none --entitlements $(SRCROOT)/$(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@;
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements "$(SRCROOT)/$(CODE_SIGN_ENTITLEMENTS)" $(SYMROOT)/$@;

# build the mach server as individual helper which does not use libdarwintest
CUSTOM_TARGETS += vm_memory_share_tests_server
vm_memory_share_tests_server: CODE_SIGN_ENTITLEMENTS = ./task_for_pid_entitlement.plist
vm_memory_share_tests_server: vm_memory_tests_src/server.c vm_memory_tests_src/common.c vm_memory_tests_src/vm_tests.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $^ -o $(SYMROOT)/vm_memory_share_tests_server
	echo $(CODESIGN) --force --sign - --timestamp=none --entitlements $(SRCROOT)/$(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@;
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements "$(SRCROOT)/$(CODE_SIGN_ENTITLEMENTS)" $(SYMROOT)/$@;

install-vm_memory_share_tests_server: vm_memory_share_tests_server
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/vm_memory_share_tests_server $(INSTALLDIR)/

io_catalog_send_data: INVALID_ARCHS = i386
io_catalog_send_data: OTHER_CFLAGS += -DTEST_UNENTITLED -framework IOKit -framework CoreFoundation -framework Foundation

io_catalog_send_data: iokit/io_catalog_send_data.m
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

ifeq ($(PLATFORM),MacOSX)
EXCLUDED_SOURCES += vm/kern_max_task_pmem.c
endif

EXCLUDED_SOURCES += benchmark/helpers.c

perf_vmfault: OTHER_CFLAGS += benchmark/helpers.c

fault_throughput: vm/fault_throughput.c
	mkdir -p $(SYMROOT)/vm
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/vm/$@
fault_throughput: OTHER_CFLAGS += benchmark/helpers.c

install-fault_throughput: fault_throughput
	mkdir -p $(INSTALLDIR)/vm
	cp $(SYMROOT)/vm/fault_throughput $(INSTALLDIR)/vm/

BATS_PLISTS += $(SRCROOT)/vm/fault_throughput.plist

ifeq ($(PLATFORM),MacOSX)
mixed_pagesize: vm/mixed_pagesize.c
	mkdir -p $(SYMROOT)/vm
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/vm/$@

install-mixed_pagesize: mixed_pagesize
	mkdir -p $(INSTALLDIR)/vm
	cp $(SYMROOT)/vm/mixed_pagesize $(INSTALLDIR)/vm/

BATS_PLISTS += $(SRCROOT)/vm/mixed_pagesize.plist

EXCLUDED_SOURCES += vm/mixed_pagesize.c vm/mixed_pagesize.plist
CUSTOM_TARGETS += mixed_pagesize
else
EXCLUDED_SOURCES += vm/mixed_pagesize.c vm/mixed_pagesize.plist
endif

manual_wq_cooperative: manual_wq_cooperative.c
	mkdir -p $(SYMROOT)
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

install-manual_wq_cooperative: manual_wq_cooperative
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/manual_wq_cooperative $(INSTALLDIR)

BATS_PLISTS += $(SRCROOT)/workqueue.plist
EXCLUDED_SOURCES += manual_wq_cooperative.c workqueue.plist
CUSTOM_TARGETS += manual_wq_cooperative

fault_throughput_benchrun:
	mkdir -p $(SYMROOT)/vm
	cp $(SRCROOT)/vm/fault_throughput.lua $(SYMROOT)/vm/fault_throughput.lua
	chmod +x $(SYMROOT)/vm/fault_throughput.lua

install-fault_throughput_benchrun: fault_throughput_benchrun
	mkdir -p $(INSTALLDIR)/vm
	cp $(SYMROOT)/vm/fault_throughput.lua $(INSTALLDIR)/vm
	chmod +x $(INSTALLDIR)/vm/fault_throughput.lua

CUSTOM_TARGETS += fault_throughput fault_throughput_benchrun
EXCLUDED_SOURCES += vm/fault_throughput.plist vm/fault_throughput.c

perf_madvise: vm/perf_madvise.c
	mkdir -p $(SYMROOT)/vm
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/vm/$@
perf_madvise: OTHER_CFLAGS += benchmark/helpers.c
install-perf_madvise: perf_madvise
	mkdir -p $(INSTALLDIR)/vm
	cp $(SYMROOT)/vm/perf_madvise $(INSTALLDIR)/vm/
perf_madvise_benchrun:
	mkdir -p $(SYMROOT)/vm
	cp $(SRCROOT)/vm/perf_madvise.lua $(SYMROOT)/vm/perf_madvise.lua
	chmod +x $(SYMROOT)/vm/perf_madvise.lua
install-perf_madvise_benchrun: perf_madvise_benchrun
	mkdir -p $(INSTALLDIR)/vm
	cp $(SYMROOT)/vm/perf_madvise.lua $(INSTALLDIR)/vm
	chmod +x $(INSTALLDIR)/vm/perf_madvise.lua

CUSTOM_TARGETS += perf_madvise perf_madvise_benchrun
EXCLUDED_SOURCES += vm/perf_madvise.c

task_create_suid_cred: CODE_SIGN_ENTITLEMENTS = ./task_create_suid_cred_entitlement.plist

OTHER_TEST_TARGETS += task_create_suid_cred_unentitled
task_create_suid_cred_unentitled: OTHER_CFLAGS += -DUNENTITLED
task_create_suid_cred_unentitled: task_create_suid_cred.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@

ioconnectasyncmethod_57641955: OTHER_LDFLAGS += -framework IOKit

ifeq ($(PLATFORM),BridgeOS)
EXCLUDED_SOURCES += ipsec.m
else
ipsec: OTHER_LDFLAGS += -framework Foundation -framework CoreFoundation -framework NetworkExtension
ipsec: CODE_SIGN_ENTITLEMENTS = ipsec.entitlements
endif

test_sysctl_kern_procargs_25397314: OTHER_LDFLAGS += -framework Foundation -ldarwintest_utils

INCLUDED_TEST_SOURCE_DIRS += counter

EXCLUDED_SOURCES += counter/common.c
counter/counter: OTHER_CFLAGS += counter/common.c test_utils.c
counter/counter: OTHER_LDFLAGS += -ldarwintest_utils -ldarwintest

counter/benchmark: counter/benchmark.c
	mkdir -p $(SYMROOT)/counter
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

counter/benchmark: OTHER_CFLAGS += counter/common.c benchmark/helpers.c

install-counter/benchmark: counter/benchmark
	mkdir -p $(INSTALLDIR)/counter
	cp $(SYMROOT)/counter/benchmark $(INSTALLDIR)/counter/

counter/benchmark_benchrun:
	mkdir -p $(SYMROOT)/counter
	cp $(SRCROOT)/counter/benchmark.lua $(SYMROOT)/counter/benchmark.lua
	chmod +x $(SYMROOT)/counter/benchmark.lua

install-counter/benchmark_benchrun: counter/benchmark_benchrun
	mkdir -p $(INSTALLDIR)/counter
	cp $(SYMROOT)/counter/benchmark.lua $(INSTALLDIR)/counter/
	chmod +x $(INSTALLDIR)/counter/benchmark.lua

CUSTOM_TARGETS += counter/benchmark counter/benchmark_benchrun
EXCLUDED_SOURCES += counter/benchmark.c

ifneq ($(PLATFORM),MacOSX)
EXCLUDED_SOURCES += vm/page_size_globals.c
else
vm/page_size_globals: INVALID_ARCHS = arm64 arm64e
endif

# Same helper used in execperf, different build system.
static_binary: ../tools/tests/execperf/exit-asm.S
	$(CC) $(DT_CFLAGS) $(CFLAGS) $(filter-out -l%,$(DT_LDFLAGS) $(LDFLAGS) $(OTHER_LDFLAGS)) $< -o $(SYMROOT)/static_binary -static -nostartfiles -nodefaultlibs -Wl,-version_load_command -e _main
	CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/static_binary

install-static_binary: static_binary
	mkdir -p $(INSTALLDIR)
	cp $(SYMROOT)/static_binary $(INSTALLDIR)

CUSTOM_TARGETS += static_binary
EXCLUDED_SOURCES += static_binary.c

test_static_binary_csflags: static_binary
test_static_binary_csflags: OTHER_LDFLAGS += -ldarwintest_utils

INCLUDED_TEST_SOURCE_DIRS += lockf_uaf_poc

trial_experiments: CODE_SIGN_ENTITLEMENTS = trial_experiments.entitlements
trial_experiments: OTHER_CFLAGS += -DENTITLED=1 test_utils.c drop_priv.c
trial_experiments: OTHER_LDFLAGS += -ldarwintest_utils
trial_experiments: trial_experiments.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none --entitlements $(CODE_SIGN_ENTITLEMENTS) $(SYMROOT)/$@;

trial_experiments_unentitled: OTHER_CFLAGS += drop_priv.c test_utils.c
trial_experiments_unentitled: trial_experiments.c
	$(CC) $(DT_CFLAGS) $(OTHER_CFLAGS) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) $< -o $(SYMROOT)/$@
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

CUSTOM_TARGETS += print_apple_array
print_apple_array: print_apple_array.c
	$(CC) $(DT_CFLAGS) -I $(OBJROOT) $(CFLAGS) $(DT_LDFLAGS) $(OTHER_LDFLAGS) $(LDFLAGS) print_apple_array.c -o $(SYMROOT)/print_apple_array
	env CODESIGN_ALLOCATE=$(CODESIGN_ALLOCATE) $(CODESIGN) --force --sign - --timestamp=none $(SYMROOT)/$@;

install-print_apple_array: print_apple_array
	mkdir -p $(INSTALLDIR)/tools
	cp $(SYMROOT)/print_apple_array $(INSTALLDIR)/tools/

OTHER_TEST_TARGETS += trial_experiments trial_experiments_unentitled

entropy: OTHER_CFLAGS += -framework perfdata

zero_to_n_tests: OTHER_LDFLAGS += -ldarwintest_utils

include $(DEVELOPER_DIR)/AppleInternal/Makefiles/darwintest/Makefile.targets

