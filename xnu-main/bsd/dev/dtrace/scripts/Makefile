export MakeInc_cmd=${SRCROOT}/makedefs/MakeInc.cmd
export MakeInc_def=${SRCROOT}/makedefs/MakeInc.def
export MakeInc_rule=${SRCROOT}/makedefs/MakeInc.rule
export MakeInc_dir=${SRCROOT}/makedefs/MakeInc.dir

include $(MakeInc_cmd)
include $(MakeInc_def)

INSTALL_DTRACE_MI_SCRIPTS_LIST =	\
	darwin.d \
	errno.d \
	io.d \
	ip.d \
	sched.d \
	signal.d \
	socket.d \
	tcp.d \
	unistd.d

INSTALL_DTRACE_LIBEXEC_LIST = \
	log_unnest_badness.d \
	vm_map_delete_permanent.d \
	vm_object_ownership.d

ifneq ($(filter $(SUPPORTED_EMBEDDED_PLATFORMS),$(PLATFORM)),)
INSTALL_DTRACE_MI_SCRIPTS_LIST += mptcp.d
endif


ifeq ($(CURRENT_ARCH_CONFIG),ARM64)
INSTALL_DTRACE_MD_SCRIPTS_LIST = regs_arm64.d ptrauth_arm64.d
INSTALL_DTRACE_MD_DIR = arm64
else ifeq ($(CURRENT_ARCH_CONFIG),ARM)
INSTALL_DTRACE_MD_SCRIPTS_LIST = regs_arm.d
INSTALL_DTRACE_MD_DIR = arm
else
INSTALL_DTRACE_MD_SCRIPTS_LIST = regs_x86_64.d vmx_compat.d
INSTALL_DTRACE_MD_DIR = x86_64
endif

INSTALL_DTRACE_MI_SCRIPTS_FILES = \
	$(addprefix $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)/, $(INSTALL_DTRACE_MI_SCRIPTS_LIST))

INSTALL_DTRACE_MD_SCRIPTS_FILES = \
	$(addprefix $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)/$(INSTALL_DTRACE_MD_DIR)/, $(INSTALL_DTRACE_MD_SCRIPTS_LIST))

$(INSTALL_DTRACE_MD_SCRIPTS_FILES): $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)/$(INSTALL_DTRACE_MD_DIR)/% : %
	$(_v)$(MKDIR) $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)/$(INSTALL_DTRACE_MD_DIR)
	@$(LOG_INSTALL) $(@F)
	$(_v)$(INSTALL) $(DATA_INSTALL_FLAGS) $< $@

$(INSTALL_DTRACE_MI_SCRIPTS_FILES): $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)/% : %
	$(_v)$(MKDIR) $(DSTROOT)/$(INSTALL_DTRACE_SCRIPTS_DIR)
	@$(LOG_INSTALL) $(@F)
	$(_v)$(INSTALL) $(DATA_INSTALL_FLAGS) $< $@

INSTALL_DTRACE_LIBEXEC_FILES = \
	$(addprefix $(DSTROOT)/$(INSTALL_DTRACE_LIBEXEC_DIR)/, $(INSTALL_DTRACE_LIBEXEC_LIST))

$(INSTALL_DTRACE_LIBEXEC_FILES): $(DSTROOT)/$(INSTALL_DTRACE_LIBEXEC_DIR)/% : %
	$(_v)$(MKDIR) $(DSTROOT)/$(INSTALL_DTRACE_LIBEXEC_DIR)
	@$(LOG_INSTALL) $(@F)
	$(_v)$(INSTALL) $(EXEC_INSTALL_FLAGS) $< $@

do_textfiles_install_mi:: $(INSTALL_DTRACE_MI_SCRIPTS_FILES) $(INSTALL_DTRACE_LIBEXEC_FILES)

do_textfiles_install_md:: $(INSTALL_DTRACE_MD_SCRIPTS_FILES)

include $(MakeInc_rule)
include $(MakeInc_dir)
