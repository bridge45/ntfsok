export MakeInc_cmd=${SRCROOT}/makedefs/MakeInc.cmd
export MakeInc_def=${SRCROOT}/makedefs/MakeInc.def
export MakeInc_rule=${SRCROOT}/makedefs/MakeInc.rule
export MakeInc_dir=${SRCROOT}/makedefs/MakeInc.dir

include $(MakeInc_cmd)
include $(MakeInc_def)

DATAFILES = \
	nfs.h nfs_lock.h nfs_ioctl.h \
	nfsproto.h rpcv2.h xdr_subs.h

KERNELFILES = \
	krpc.h nfs.h nfsproto.h rpcv2.h \
	xdr_subs.h

INSTALL_MI_LIST	= ${DATAFILES}

INSTALL_KF_MI_LIST = ${KERNELFILES}

INSTALL_SF_MI_LCL_LIST = ${DATAFILES}

INSTALL_KF_MI_LCL_LIST = ${KERNELFILES}

INSTALL_MI_DIR = nfs

EXPORT_MI_LIST	=

EXPORT_MI_DIR = nfs

include $(MakeInc_rule)
include $(MakeInc_dir)
