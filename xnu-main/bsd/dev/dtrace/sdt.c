/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <miscfs/devfs/devfs.h>

#if defined(__arm__) || defined(__arm64__)
#include <arm/caches_internal.h>
#endif /* defined(__arm__) || defined(__arm64__) */

#include <sys/dtrace.h>
#include <sys/dtrace_impl.h>

#include <sys/dtrace_glue.h>

#include <sys/sdt_impl.h>
extern int dtrace_kernel_symbol_mode;

#include <ptrauth.h>

/* #include <machine/trap.h */
struct savearea_t; /* Used anonymously */

#if defined(__arm__)
typedef kern_return_t (*perfCallback)(int, struct savearea_t *, __unused int, __unused int);
extern perfCallback tempDTraceTrapHook;
extern kern_return_t fbt_perfCallback(int, struct savearea_t *, __unused int, __unused int);
#define SDT_PATCHVAL    0xdefc
#define SDT_AFRAMES             7
#elif defined(__arm64__)
typedef kern_return_t (*perfCallback)(int, struct savearea_t *, __unused int, __unused int);
extern perfCallback tempDTraceTrapHook;
extern kern_return_t fbt_perfCallback(int, struct savearea_t *, __unused int, __unused int);
#define SDT_PATCHVAL    0xe7eeee7e
#define SDT_AFRAMES             7
#elif defined(__x86_64__)
typedef kern_return_t (*perfCallback)(int, struct savearea_t *, uintptr_t *, int);
extern perfCallback tempDTraceTrapHook;
extern kern_return_t fbt_perfCallback(int, struct savearea_t *, uintptr_t *, int);
#define SDT_PATCHVAL    0xf0
#define SDT_AFRAMES             6
#else
#error Unknown architecture
#endif

#define SDT_PROBETAB_SIZE       0x1000          /* 4k entries -- 16K total */

static int              sdt_verbose = 0;
sdt_probe_t             **sdt_probetab;
int                     sdt_probetab_size;
int                     sdt_probetab_mask;

/*ARGSUSED*/
static void
__sdt_provide_module(void *arg, struct modctl *ctl)
{
#pragma unused(arg)
	char *modname = ctl->mod_modname;
	sdt_probedesc_t *sdpd;
	sdt_probe_t *sdp, *old;
	sdt_provider_t *prov;

	/*
	 * One for all, and all for one:  if we haven't yet registered all of
	 * our providers, we'll refuse to provide anything.
	 */
	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (prov->sdtp_id == DTRACE_PROVNONE) {
			return;
		}
	}

	/* Nothing to do. Module is either invalid or we haven't found any SDT probe descriptions. */
	if (!ctl || ctl->mod_sdtprobecnt != 0 || (sdpd = ctl->mod_sdtdesc) == NULL) {
		return;
	}

	for (sdpd = ctl->mod_sdtdesc; sdpd != NULL; sdpd = sdpd->sdpd_next) {
		dtrace_id_t id;

		/* Validate probe's provider name.  Do not provide probes for unknown providers. */
		for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
			if (strcmp(prov->sdtp_prefix, sdpd->sdpd_prov) == 0) {
				break;
			}
		}

		if (prov->sdtp_name == NULL) {
			printf("Ignoring probes from unsupported provider %s\n", sdpd->sdpd_prov);
			continue;
		}

		if (sdpd->sdpd_func == NULL) {
			/*
			 * Ignore probes for which we don't have any symbol.  That's likely some problem with
			 * __sdt section processing.
			 */
			printf("Ignoring probe %s (no symbol name)\n", sdpd->sdpd_name);
			continue;
		}

		sdp = kmem_zalloc(sizeof(sdt_probe_t), KM_SLEEP);
		sdp->sdp_loadcnt = ctl->mod_loadcnt;
		sdp->sdp_ctl = ctl;
		sdp->sdp_name = kmem_alloc(strlen(sdpd->sdpd_name) + 1, KM_SLEEP);
		(void) strlcpy(sdp->sdp_name, sdpd->sdpd_name, strlen(sdpd->sdpd_name) + 1);
		sdp->sdp_namelen = strlen(sdpd->sdpd_name) + 1;
		sdp->sdp_provider = prov;

		/*
		 * We have our provider.  Now create the probe.
		 */
		if ((id = dtrace_probe_lookup(prov->sdtp_id, modname,
		    sdpd->sdpd_func, sdp->sdp_name)) != DTRACE_IDNONE) {
			old = dtrace_probe_arg(prov->sdtp_id, id);
			ASSERT(old != NULL);

			sdp->sdp_next = old->sdp_next;
			sdp->sdp_id = id;
			old->sdp_next = sdp;
		} else {
			sdp->sdp_id = dtrace_probe_create(prov->sdtp_id,
			    modname, sdpd->sdpd_func, sdp->sdp_name, SDT_AFRAMES, sdp);

			ctl->mod_sdtprobecnt++;
		}

#if 0
		printf("__sdt_provide_module:  sdpd=0x%p  sdp=0x%p  name=%s, id=%d\n", sdpd, sdp,
		    sdp->sdp_name, sdp->sdp_id);
#endif

		sdp->sdp_hashnext =
		    sdt_probetab[SDT_ADDR2NDX(sdpd->sdpd_offset)];
		sdt_probetab[SDT_ADDR2NDX(sdpd->sdpd_offset)] = sdp;

		sdp->sdp_patchval = SDT_PATCHVAL;
		sdp->sdp_patchpoint = (sdt_instr_t *)sdpd->sdpd_offset;
		sdp->sdp_savedval = *sdp->sdp_patchpoint;
	}
}

/*ARGSUSED*/
static void
sdt_destroy(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id)
	sdt_probe_t *sdp = parg, *old, *last, *hash;
	int ndx;

	struct modctl *ctl = sdp->sdp_ctl;

	/*
	 * Decrement SDT probe counts only when a probe being destroyed belongs to the
	 * currently loaded version of a module and not the stale one.
	 */
	if (ctl != NULL && ctl->mod_loadcnt == sdp->sdp_loadcnt && ctl->mod_loaded) {
		ctl->mod_sdtprobecnt--;
	}

	while (sdp != NULL) {
		old = sdp;

		/*
		 * Now we need to remove this probe from the sdt_probetab.
		 */
		ndx = SDT_ADDR2NDX(sdp->sdp_patchpoint);
		last = NULL;
		hash = sdt_probetab[ndx];

		while (hash != sdp) {
			ASSERT(hash != NULL);
			last = hash;
			hash = hash->sdp_hashnext;
		}

		if (last != NULL) {
			last->sdp_hashnext = sdp->sdp_hashnext;
		} else {
			sdt_probetab[ndx] = sdp->sdp_hashnext;
		}

		kmem_free(sdp->sdp_name, sdp->sdp_namelen);
		sdp = sdp->sdp_next;
		kmem_free(old, sizeof(sdt_probe_t));
	}
}

/*ARGSUSED*/
static int
sdt_enable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id)
	sdt_probe_t *sdp = parg;
	struct modctl *ctl = sdp->sdp_ctl;

	ctl->mod_nenabled++;

	/*
	 * If this module has disappeared since we discovered its probes,
	 * refuse to enable it.
	 */
	if (!ctl->mod_loaded) {
		if (sdt_verbose) {
			cmn_err(CE_NOTE, "sdt is failing for probe %s "
			    "(module %s unloaded)",
			    sdp->sdp_name, ctl->mod_modname);
		}
		goto err;
	}

	/*
	 * Now check that our modctl has the expected load count.  If it
	 * doesn't, this module must have been unloaded and reloaded -- and
	 * we're not going to touch it.
	 */
	if (ctl->mod_loadcnt != sdp->sdp_loadcnt) {
		if (sdt_verbose) {
			cmn_err(CE_NOTE, "sdt is failing for probe %s "
			    "(module %s reloaded)",
			    sdp->sdp_name, ctl->mod_modname);
		}
		goto err;
	}

	dtrace_casptr(&tempDTraceTrapHook, NULL, ptrauth_nop_cast(void *, &fbt_perfCallback));
	if (tempDTraceTrapHook != (perfCallback)fbt_perfCallback) {
		if (sdt_verbose) {
			cmn_err(CE_NOTE, "sdt_enable is failing for probe %s "
			    "in module %s: tempDTraceTrapHook already occupied.",
			    sdp->sdp_name, ctl->mod_modname);
		}
		return 0;
	}

	while (sdp != NULL) {
		(void)ml_nofault_copy((vm_offset_t)&sdp->sdp_patchval, (vm_offset_t)sdp->sdp_patchpoint,
		    (vm_size_t)sizeof(sdp->sdp_patchval));

		/*
		 * Make the patched instruction visible via a data + instruction
		 * cache fush on platforms that need it
		 */
		flush_dcache((vm_offset_t)sdp->sdp_patchpoint, (vm_size_t)sizeof(sdp->sdp_patchval), 0);
		invalidate_icache((vm_offset_t)sdp->sdp_patchpoint, (vm_size_t)sizeof(sdp->sdp_patchval), 0);

		sdp = sdp->sdp_next;
	}

err:
	return 0;
}

/*ARGSUSED*/
static void
sdt_disable(void *arg, dtrace_id_t id, void *parg)
{
#pragma unused(arg,id)
	sdt_probe_t *sdp = parg;
	struct modctl *ctl = sdp->sdp_ctl;

	ctl->mod_nenabled--;

	if (!ctl->mod_loaded || ctl->mod_loadcnt != sdp->sdp_loadcnt) {
		goto err;
	}

	while (sdp != NULL) {
		(void)ml_nofault_copy((vm_offset_t)&sdp->sdp_savedval, (vm_offset_t)sdp->sdp_patchpoint,
		    (vm_size_t)sizeof(sdp->sdp_savedval));
		/*
		 * Make the patched instruction visible via a data + instruction
		 * cache flush on platforms that need it
		 */
		flush_dcache((vm_offset_t)sdp->sdp_patchpoint, (vm_size_t)sizeof(sdp->sdp_savedval), 0);
		invalidate_icache((vm_offset_t)sdp->sdp_patchpoint, (vm_size_t)sizeof(sdp->sdp_savedval), 0);
		sdp = sdp->sdp_next;
	}

err:
	;
}

static dtrace_pops_t sdt_pops = {
	.dtps_provide =         NULL,
	.dtps_provide_module =  sdt_provide_module,
	.dtps_enable =          sdt_enable,
	.dtps_disable =         sdt_disable,
	.dtps_suspend =         NULL,
	.dtps_resume =          NULL,
	.dtps_getargdesc =      sdt_getargdesc,
	.dtps_getargval =       sdt_getarg,
	.dtps_usermode =        NULL,
	.dtps_destroy =         sdt_destroy,
};

/*ARGSUSED*/
static int
sdt_attach(dev_info_t *devi)
{
	sdt_provider_t *prov;

	if (ddi_create_minor_node(devi, "sdt", S_IFCHR,
	    0, DDI_PSEUDO, 0) == DDI_FAILURE) {
		cmn_err(CE_NOTE, "/dev/sdt couldn't create minor node");
		ddi_remove_minor_node(devi, NULL);
		return DDI_FAILURE;
	}

	if (sdt_probetab_size == 0) {
		sdt_probetab_size = SDT_PROBETAB_SIZE;
	}

	sdt_probetab_mask = sdt_probetab_size - 1;
	sdt_probetab =
	    kmem_zalloc(sdt_probetab_size * sizeof(sdt_probe_t *), KM_SLEEP);
	dtrace_invop_add(sdt_invop);

	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (dtrace_register(prov->sdtp_name, prov->sdtp_attr,
		    DTRACE_PRIV_KERNEL, NULL,
		    &sdt_pops, prov, &prov->sdtp_id) != 0) {
			cmn_err(CE_WARN, "failed to register sdt provider %s",
			    prov->sdtp_name);
		}
	}

	return DDI_SUCCESS;
}

/*
 * APPLE NOTE:  sdt_detach not implemented
 */
#if !defined(__APPLE__)
/*ARGSUSED*/
static int
sdt_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	sdt_provider_t *prov;

	switch (cmd) {
	case DDI_DETACH:
		break;

	case DDI_SUSPEND:
		return DDI_SUCCESS;

	default:
		return DDI_FAILURE;
	}

	for (prov = sdt_providers; prov->sdtp_name != NULL; prov++) {
		if (prov->sdtp_id != DTRACE_PROVNONE) {
			if (dtrace_unregister(prov->sdtp_id) != 0) {
				return DDI_FAILURE;
			}

			prov->sdtp_id = DTRACE_PROVNONE;
		}
	}

	dtrace_invop_remove(sdt_invop);
	kmem_free(sdt_probetab, sdt_probetab_size * sizeof(sdt_probe_t *));

	return DDI_SUCCESS;
}
#endif /* __APPLE__ */

d_open_t _sdt_open;

int
_sdt_open(dev_t dev, int flags, int devtype, struct proc *p)
{
#pragma unused(dev,flags,devtype,p)
	return 0;
}

#define SDT_MAJOR  -24 /* let the kernel pick the device number */

static const struct cdevsw sdt_cdevsw =
{
	.d_open = _sdt_open,
	.d_close = eno_opcl,
	.d_read = eno_rdwrt,
	.d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl,
	.d_stop = (stop_fcn_t *)nulldev,
	.d_reset = (reset_fcn_t *)nulldev,
	.d_select = eno_select,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
};


#include <mach-o/nlist.h>
#include <libkern/kernel_mach_header.h>

/*
 * Represents single record in __DATA,__sdt section.
 */
typedef struct dtrace_sdt_def {
	uintptr_t      dsd_addr;    /* probe site location */
	const char     *dsd_prov;   /* provider's name */
	const char     *dsd_name;   /* probe's name */
} __attribute__((__packed__))  dtrace_sdt_def_t;

/*
 * Creates a copy of name and unescapes '-' characters.
 */
static char *
sdt_strdup_name(const char *name)
{
	size_t len = strlen(name) + 1;
	size_t i, j;
	char *nname = kmem_alloc(len, KM_SLEEP);

	for (i = 0, j = 0; name[j] != '\0'; i++) {
		if (name[j] == '_' && name[j + 1] == '_') {
			nname[i] = '-';
			j += 2;
		} else {
			nname[i] = name[j++];
		}
	}

	nname[i] = '\0';
	return nname;
}

/*
 * Returns Mach-O header that should be used for given modctl.
 */
static kernel_mach_header_t *
sdt_get_module_mh(struct modctl *ctl)
{
	kernel_mach_header_t *mh = (kernel_mach_header_t *)ctl->mod_address;

	/* Static KEXTs have their __sdt section merged into kernel's __sdt. */
	if (MOD_IS_STATIC_KEXT(ctl)) {
		mh = &_mh_execute_header;
	}

	if (mh->magic != MH_MAGIC_KERNEL) {
		return NULL;
	}

	return mh;
}

/*
 * Finds symbol table for given kernel module.
 */
static uint32_t
sdt_find_symbol_table(struct modctl *ctl, kernel_nlist_t **sym, char **strings)
{
	kernel_mach_header_t        *mh = sdt_get_module_mh(ctl);
	struct load_command         *cmd = (struct load_command *)&mh[1];
	kernel_segment_command_t    *orig_le = NULL;
	struct symtab_command       *orig_st = NULL;

	for (int i = 0; i < mh->ncmds; i++) {
		if (cmd->cmd == LC_SEGMENT_KERNEL) {
			kernel_segment_command_t *orig_sg = (kernel_segment_command_t *) cmd;

			if (LIT_STRNEQL(orig_sg->segname, SEG_LINKEDIT)) {
				orig_le = orig_sg;
			}
		} else if (cmd->cmd == LC_SYMTAB) {
			orig_st = (struct symtab_command *) cmd;
		}

		cmd = (struct load_command *) ((uintptr_t) cmd + cmd->cmdsize);
	}

	if ((orig_st == NULL) || (orig_le == NULL)) {
		return 0;
	}

	*sym = (kernel_nlist_t *)(orig_le->vmaddr + orig_st->symoff - orig_le->fileoff);
	*strings = (char *)(orig_le->vmaddr + orig_st->stroff - orig_le->fileoff);

	return orig_st->nsyms;
}

/* Last kernel address. */
static SECURITY_READ_ONLY_LATE(vm_address_t) kern_end = (vm_address_t)-1;

void
sdt_early_init(void)
{
	kernel_mach_header_t        *mh = &_mh_execute_header;
	kernel_section_t            *sec_ks = NULL;
	kc_format_t                 kc_format;

	if (!PE_get_primary_kc_format(&kc_format)) {
		kc_format = KCFormatUnknown;
	}

	/*
	 * Detects end of kernel's text in static kernel cache. It is the last text address before
	 * the first kext text section start.
	 */
	if (kc_format == KCFormatStatic) {
		if ((sec_ks = getsectbynamefromheader(mh, "__PRELINK_INFO", "__kmod_start")) == NULL) {
			printf("SDT: unable to find prelink info\n");
			return;
		}

		/* find the MIN(start_address) of all kexts in this image. */
		const uint64_t *start_addr = (const uint64_t *)sec_ks->addr;
		for (int i = 0; i < sec_ks->size / sizeof(uint64_t); i++) {
			if (kern_end > start_addr[i]) {
				kern_end = start_addr[i];
			}
		}
	}
}

/*
 * Finds TEXT range that belongs to given module.
 */
static int
sdt_find_module_text_range(struct modctl *ctl, vm_address_t *start, vm_address_t *end)
{
	kc_format_t                 kc_format;

	if (!PE_get_primary_kc_format(&kc_format)) {
		kc_format = KCFormatUnknown;
	}

	/* Adjust kernel region for static kernel cache. */
	*start = ctl->mod_address;

	if (MOD_IS_MACH_KERNEL(ctl) && kc_format == KCFormatStatic) {
		*end = kern_end;
	} else {
		*end = ctl->mod_address + ctl->mod_size;
	}

	return 1;
}

/*
 * Processes SDT section in given Mach-O header
 */
void
sdt_load_machsect(struct modctl *ctl)
{
	kernel_mach_header_t        *mh = sdt_get_module_mh(ctl);
	kernel_section_t            *sec_sdt = NULL;
	char                        *strings = NULL;
	kernel_nlist_t              *sym = NULL;
	vm_address_t                text_start, text_end;
	unsigned int                len;
	uint32_t                    nsyms = 0;

	const bool ctl_has_kernel_symbols = MOD_HAS_KERNEL_SYMBOLS(ctl);

	if (mh == NULL) {
		return;
	}

	/* Ignore SDT definitions if we don't know where they belong. */
	if (!sdt_find_module_text_range(ctl, &text_start, &text_end)) {
		printf("SDT: Unable to determine text range for %s\n", ctl->mod_modname);
		return;
	}

	/* Do not load SDTs when asked to use kernel symbols but symbol table is not available. */
	if (ctl_has_kernel_symbols && (nsyms = sdt_find_symbol_table(ctl, &sym, &strings)) == 0) {
		printf("SDT: No kernel symbols for %s\n", ctl->mod_modname);
		return;
	}

	/* Locate DTrace SDT section in the object. */
	if ((sec_sdt = getsectbynamefromheader(mh, "__DATA", "__sdt")) == NULL) {
		return;
	}

	/*
	 * Iterate over SDT section and establish all SDT probe descriptions.
	 */
	dtrace_sdt_def_t *sdtdef = (dtrace_sdt_def_t *)(sec_sdt->addr);
	for (size_t k = 0; k < sec_sdt->size / sizeof(dtrace_sdt_def_t); k++, sdtdef++) {
		const char *funcname;
		unsigned long best = 0;

#if defined(__arm__)
		/* PR8353094 - mask off thumb-bit */
		sdtdef->dsd_addr &= ~0x1U;
#elif defined(__arm64__)
		sdtdef->dsd_addr &= ~0x1LU;
#endif  /* __arm__ */

		/*
		 * Static KEXTs share __sdt section with kernel after linking. It is required
		 * to filter out description and pick only those that belong to requested
		 * module or kernel itself.
		 */
		if (MOD_IS_STATIC_KEXT(ctl) || MOD_IS_MACH_KERNEL(ctl)) {
			if ((sdtdef->dsd_addr < text_start) || (sdtdef->dsd_addr > text_end)) {
				continue;
			}
		} else {
			/* Skip over probe descripton that do not belong to current module. */
			if (!dtrace_addr_in_module((void *)sdtdef->dsd_addr, ctl)) {
				continue;
			}
		}

		sdt_probedesc_t *sdpd = kmem_alloc(sizeof(sdt_probedesc_t), KM_SLEEP);

		/* Unescape probe name and keep a note of the size of original memory allocation. */
		sdpd->sdpd_name = sdt_strdup_name(sdtdef->dsd_name);
		sdpd->sdpd_namelen = strlen(sdtdef->dsd_name) + 1;

		/* Used only for provider structure lookup so there is no need to make dynamic copy. */
		sdpd->sdpd_prov = sdtdef->dsd_prov;

		/*
		 * Find the symbol immediately preceding the sdt probe site just discovered,
		 * that symbol names the function containing the sdt probe.
		 */
		sdpd->sdpd_func = NULL;

		if (ctl_has_kernel_symbols) {
			funcname = NULL;
			for (int i = 0; i < nsyms; i++) {
				uint8_t jn_type = sym[i].n_type & N_TYPE;
				char *jname = strings + sym[i].n_un.n_strx;

				if ((N_SECT != jn_type && N_ABS != jn_type)) {
					continue;
				}

				if (0 == sym[i].n_un.n_strx) { /* iff a null, "", name. */
					continue;
				}

				if (*jname == '_') {
					jname += 1;
				}

				if (sdtdef->dsd_addr <= (unsigned long)sym[i].n_value) {
					continue;
				}

				if ((unsigned long)sym[i].n_value > best) {
					best = (unsigned long)sym[i].n_value;
					funcname = jname;
				}
			}

			if (funcname) {
				len = strlen(funcname) + 1;
				sdpd->sdpd_func = kmem_alloc(len, KM_SLEEP);
				(void) strlcpy(sdpd->sdpd_func, funcname, len);
			}
		}

		sdpd->sdpd_offset = sdtdef->dsd_addr;

		sdpd->sdpd_next = (sdt_probedesc_t *)ctl->mod_sdtdesc;
		ctl->mod_sdtdesc = sdpd;
	}
}

void
sdt_init( void )
{
	int majdevno = cdevsw_add(SDT_MAJOR, &sdt_cdevsw);

	if (majdevno < 0) {
		printf("sdt_init: failed to allocate a major number!\n");
		return;
	}

	if (dtrace_sdt_probes_restricted()) {
		return;
	}

	sdt_attach((dev_info_t*)(uintptr_t)majdevno);
}

#undef SDT_MAJOR

/*
 * Provide SDT modules with userspace symbols.
 *
 * A module contains only partially filled in SDT probe descriptions because symbols were
 * not available at the time when __sdt section was loaded. Fixup descriptons before providing
 * the probes.
 */
static void
sdt_provide_module_user_syms(void *arg, struct modctl *ctl)
{
	sdt_probedesc_t *sdpd;
	dtrace_module_symbols_t *mod_sym = ctl->mod_user_symbols;

	if (mod_sym == NULL) {
		printf("DTrace missing userspace symbols for module %s\n", ctl->mod_modname);
		return;
	}

	/* Fixup missing probe description parts. */
	for (sdpd = ctl->mod_sdtdesc; sdpd != NULL; sdpd = sdpd->sdpd_next) {
		ASSERT(sdpd->sdpd_func == NULL);

		/* Look for symbol that contains SDT probe offset. */
		for (int i = 0; i < mod_sym->dtmodsyms_count; i++) {
			dtrace_symbol_t *symbol = &mod_sym->dtmodsyms_symbols[i];
			char *name = symbol->dtsym_name;

			/*
			 * Every function symbol gets extra '_' prepended in the Mach-O symbol table.
			 * Strip it away to make a probe's function name match source code.
			 */
			if (*name == '_') {
				name += 1;
			}

			if (!symbol->dtsym_addr) {
				continue;
			}

			/* Ignore symbols that do not belong to this module. */
			if (!dtrace_addr_in_module((void *)symbol->dtsym_addr, ctl)) {
				continue;
			}

			/* Pick symbol name when we found match. */
			if ((symbol->dtsym_addr <= sdpd->sdpd_offset) &&
			    (sdpd->sdpd_offset < symbol->dtsym_addr + symbol->dtsym_size)) {
				size_t len = strlen(name) + 1;
				sdpd->sdpd_func = kmem_alloc(len, KM_SLEEP);
				(void) strlcpy(sdpd->sdpd_func, name, len);
				break;
			}
		}
	}

	/* Probe descriptionds are now fixed up.  Provide them as usual. */
	__sdt_provide_module(arg, ctl);
}

/*ARGSUSED*/
void
sdt_provide_module(void *arg, struct modctl *ctl)
{
	ASSERT(ctl != NULL);
	ASSERT(dtrace_kernel_symbol_mode != DTRACE_KERNEL_SYMBOLS_NEVER);
	LCK_MTX_ASSERT(&mod_lock, LCK_MTX_ASSERT_OWNED);

	if (MOD_SDT_DONE(ctl)) {
		return;
	}

	if (MOD_HAS_KERNEL_SYMBOLS(ctl)) {
		__sdt_provide_module(arg, ctl);
		ctl->mod_flags |= MODCTL_SDT_PROBES_PROVIDED;
		return;
	}

	if (MOD_HAS_USERSPACE_SYMBOLS(ctl)) {
		sdt_provide_module_user_syms(arg, ctl);
		ctl->mod_flags |= MODCTL_SDT_PROBES_PROVIDED;
		return;
	}

	/*
	 * The SDT provider's module is not detachable so we don't have to re-provide SDT
	 * probes if that happens.  After succesfull providing, the probe descriptions are
	 * no longer required.  If module gets re-loaded it will get a new set of probe
	 * descriptions from its __sdt section.
	 */
	if (MOD_SDT_PROBES_PROVIDED(ctl)) {
		sdt_probedesc_t *sdpd = ctl->mod_sdtdesc;
		while (sdpd) {
			sdt_probedesc_t *this_sdpd = sdpd;
			kmem_free((void *)sdpd->sdpd_name, sdpd->sdpd_namelen);
			if (sdpd->sdpd_func) {
				kmem_free((void *)sdpd->sdpd_func, strlen(sdpd->sdpd_func) + 1);
			}
			sdpd = sdpd->sdpd_next;
			kmem_free((void *)this_sdpd, sizeof(sdt_probedesc_t));
		}
		ctl->mod_sdtdesc = NULL;
	}
}
