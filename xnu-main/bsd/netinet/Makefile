export MakeInc_cmd=${SRCROOT}/makedefs/MakeInc.cmd
export MakeInc_def=${SRCROOT}/makedefs/MakeInc.def
export MakeInc_rule=${SRCROOT}/makedefs/MakeInc.rule
export MakeInc_dir=${SRCROOT}/makedefs/MakeInc.dir

include $(MakeInc_cmd)
include $(MakeInc_def)

DATAFILES = \
	bootp.h icmp6.h if_ether.h icmp_var.h \
	igmp.h igmp_var.h in.h in_pcb.h \
	in_systm.h in_var.h ip.h ip6.h \
	ip_icmp.h ip_var.h tcp.h \
	tcp_fsm.h tcp_seq.h tcp_timer.h tcp_var.h \
	tcpip.h udp.h udp_var.h

# netinet.modulemap additionally includes in6.h from ../netinet6.
MODULEMAPFILES = \
	netinet.modulemap

KERNELFILES = \
	kpi_ipfilter.h in_arp.h

PRIVATE_DATAFILES = \
	flow_divert_proto.h \
	igmp_var.h \
	in.h \
	in_gif.h \
	in_pcb.h \
	in_tclass.h \
	ip.h \
	ip_compat.h \
	ip_dummynet.h \
	ip_flowid.h \
	mptcp_var.h \
	tcp.h \
	tcp_cc.h \
	tcp_debug.h \
	tcp_var.h \
	tcp_cache.h \
	udp.h \
	in_stat.h

DRIVERKIT_DATAFILES = \
	in.h \
	ip.h \
	ip6.h \
	tcp.h \
	ip_compat.h \
	in_systm.h \
	udp.h

PRIVATE_KERNELFILES = ${KERNELFILES} \
	ip_ecn.h ip_encap.h tcp_log.h

INSTALL_MI_LIST	= ${DATAFILES}

INSTALL_MODULEMAP_MI_LIST = ${MODULEMAPFILES}

INSTALL_DRIVERKIT_MI_LIST = ${DRIVERKIT_DATAFILES}

INSTALL_MI_DIR = netinet

EXPORT_MI_LIST = ${DATAFILES} ${KERNELFILES}

EXPORT_MI_DIR = ${INSTALL_MI_DIR}

INSTALL_SF_MI_LCL_LIST = $(sort ${DATAFILES} ${PRIVATE_DATAFILES})
INSTALL_KF_MI_LCL_LIST = $(sort ${DATAFILES} ${PRIVATE_DATAFILES} ${PRIVATE_KERNELFILES})

include $(MakeInc_rule)
include $(MakeInc_dir)
