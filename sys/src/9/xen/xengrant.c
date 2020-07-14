/*
 * Sharing page frames with other domains
 */
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"

#define LOG(a) // a;

enum {
	Nframes = 4,
};

static struct {
	Lock;
	ushort free;
	ushort *refs;
} refalloc;

static grant_entry_t * granttab;

void
xengrantinit(void)
{
	gnttab_setup_table_t setup;
	ulong frames[Nframes];
	int nrefs, i;

	setup.dom = DOMID_SELF;
	setup.nr_frames = Nframes;
	set_xen_guest_handle(setup.frame_list, frames);
	if (HYPERVISOR_grant_table_op(GNTTABOP_setup_table, &setup, 1) != 0 || setup.status != 0)
		panic("xen grant table setup");
	for (i = 0; i < Nframes; i++)
		mmumapframe(XENGRANTTAB+BY2PG*i, frames[i]);
	granttab = (grant_entry_t*)XENGRANTTAB;
	nrefs = Nframes * BY2PG / sizeof(grant_entry_t);
	refalloc.refs = (ushort*)malloc(nrefs*sizeof(ushort));
	for (i = 0; i < nrefs; i++)
		refalloc.refs[i] = i-1;
	refalloc.free = nrefs-1;
	LOG(dprint("xengrantinit %d %d\n", nrefs, refalloc.free))
}

static int
allocref(void)
{
	int ref;

	ilock(&refalloc);
	ref = refalloc.free;
	if (ref > 0)
		refalloc.free = refalloc.refs[ref];
	iunlock(&refalloc);
	LOG(dprint("allocref %d\n", ref))
	return ref;
}

static void
freeref(int ref)
{
	ilock(&refalloc);
	refalloc.refs[ref] = refalloc.free;
	refalloc.free = ref;
	iunlock(&refalloc);
	LOG(dprint("freeref %d\n", ref))
}

int
xengrant(domid_t domid, ulong frame, int flags)
{
	int ref;
	grant_entry_t *gt;

	if ((ref = allocref()) < 0)
		panic("out of xengrant refs");
	gt = &granttab[ref];
	gt->frame = frame;
	gt->domid = domid;
	coherence();
	gt->flags = flags;
	LOG(dprint("xengrant %lux %d\n", frame, ref))
	return ref;
}

int
xengrantend(int ref)
{
	grant_entry_t *gt;
	int frame;

	gt = &granttab[ref];
	coherence();
	if (gt->flags&GTF_accept_transfer) {
		if ((gt->flags&GTF_transfer_completed) == 0)
			panic("xengrantend transfer in progress");
	} else {
		if (gt->flags&(GTF_reading|GTF_writing))
			panic("xengrantend frame in use");
	}
	coherence();
	frame = gt->frame;
	gt->flags = GTF_invalid;
	freeref(ref);
	LOG(dprint("xengrantend %d\n", frame, ref))
	return frame;
}
