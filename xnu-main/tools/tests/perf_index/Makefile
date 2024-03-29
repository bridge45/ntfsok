include ../Makefile.common

CFLAGS:=-c -Wall -pedantic -Os -isysroot $(SDKROOT) $(ARCH_FLAGS)
CFLAGS += -Wl,-sectcreate,__INFO_FILTER,__disable,/dev/null
LDFLAGS:= $(ARCH_FLAGS) -isysroot $(SDKROOT)

SRCROOT?=$(shell /bin/pwd)
DSTROOT?=$(shell /bin/pwd)/BUILD/dst
OBJROOT?=$(shell /bin/pwd)/BUILD/obj
SYMROOT?=$(shell /bin/pwd)/BUILD/sym


ifeq "$(Embedded)" "YES"
	TARGET_NAME:=PerfIndex.bundle-ios
else
	TARGET_NAME:=PerfIndex.bundle-osx
endif

all: $(DSTROOT) $(OBJROOT) $(SYMROOT) \
	$(DSTROOT)/perf_index \
	$(DSTROOT)/perfindex-cpu.dylib \
	$(DSTROOT)/perfindex-memory.dylib \
	$(DSTROOT)/perfindex-syscall.dylib \
	$(DSTROOT)/perfindex-fault.dylib \
	$(DSTROOT)/perfindex-zfod.dylib \
	$(DSTROOT)/perfindex-file_create.dylib \
	$(DSTROOT)/perfindex-file_read.dylib \
	$(DSTROOT)/perfindex-file_write.dylib \
	$(DSTROOT)/perfindex-ram_file_create.dylib \
	$(DSTROOT)/perfindex-ram_file_read.dylib \
	$(DSTROOT)/perfindex-ram_file_write.dylib \
	$(DSTROOT)/perfindex-iperf.dylib \
	$(DSTROOT)/perfindex-compile.dylib \
	$(DSTROOT)/PerfIndex.bundle

$(DSTROOT)/perfindex-cpu.dylib: $(OBJROOT)/md5.o
$(DSTROOT)/perfindex-fault.dylib: $(OBJROOT)/test_fault_helper.o
$(DSTROOT)/perfindex-zfod.dylib: $(OBJROOT)/test_fault_helper.o
$(DSTROOT)/perfindex-file_create.dylib: $(OBJROOT)/test_file_helper.o
$(DSTROOT)/perfindex-file_read.dylib: $(OBJROOT)/test_file_helper.o
$(DSTROOT)/perfindex-file_write.dylib: $(OBJROOT)/test_file_helper.o
$(DSTROOT)/perfindex-ram_file_create.dylib: $(OBJROOT)/test_file_helper.o $(OBJROOT)/ramdisk.o
$(DSTROOT)/perfindex-ram_file_read.dylib: $(OBJROOT)/test_file_helper.o $(OBJROOT)/ramdisk.o
$(DSTROOT)/perfindex-ram_file_write.dylib: $(OBJROOT)/test_file_helper.o $(OBJROOT)/ramdisk.o

$(DSTROOT)/perf_index: $(OBJROOT)/perf_index.o
	$(CC) $(LDFLAGS) $? -o $@

$(DSTROOT)/PerfIndex.bundle: $(SRCROOT)/PerfIndex_COPS_Module/PerfIndex.xcodeproj
	xcodebuild -sdk $(SDKROOT) -target $(TARGET_NAME) OBJROOT=$(OBJROOT) SYMROOT=$(SYMROOT) TARGET_TEMP_DIR=$(OBJROOT) TARGET_BUILD_DIR=$(DSTROOT) -project $? CLANG_ENABLE_MODULES=NO

$(DSTROOT)/%.dylib: $(OBJROOT)/%.o
	$(CC) $(LDFLAGS) -dynamiclib $? -o $@

$(OBJROOT)/%.o: $(SRCROOT)/%.c
	$(CC) $(CFLAGS) $? -o $@

$(DSTROOT):
	mkdir -p $(DSTROOT)

$(OBJROOT):
	mkdir -p $(OBJROOT)

$(SYMROOT):
	mkdir -p $(SYMROOT)

clean:
	rm -rf $(OBJROOT)
	rm -rf $(DSTROOT)
	rm -rf $(SYMROOT)
