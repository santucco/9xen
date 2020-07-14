/*
 * Xen vdisplay interface frontend
 * Copyright (C) 2020 Alexander Sychev
 */

#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"
#include "io.h"
#include "../port/error.h"
#define	Image	IMAGE
#include <draw.h>

#define FOURCC_RGB565  0x36314752
#define FOURCC_BGR888  0x34324742
#define FOURCC_RGBA8888 0x34324152
#define FOURCC_ARGB8888 0x34325241
#define FOURCC_XRGB8888 0x34325258
#define FOURCC_ABGR8888 0x34324241
#define FOURCC_XBGR8888 0x34324258


#define LOG(a)
//#define LOG(a) a;
//#define P print
#define P dprint

typedef struct xendispl_evt xendispl_evt;
typedef struct xendispl_event_page xendispl_event_page;
typedef struct xendispl_page_directory xendispl_page_directory;
typedef struct xendispl_resp xendispl_resp;
typedef struct xendispl_req xendispl_req;
typedef struct cookie {
	union {
		struct {
			uint high;
			uint low;
		} parts;
		unsigned long long full;
	};
} cookie;

typedef struct vdisplay {
	int	backend;
	int	evtchn;
	int evtrngref;
	xendispl_event_page *evtrng;
	int	reqchn;
	int reqrngref;
	xen_displif_front_ring_t reqrng;
	uint width;
	uint height;
	uint bpp;
	ulong chan;
	uchar *dbuf;
	uchar *extbuf;
	uchar *intbuf;
	uint bsize;
	Rendez evtr;
	Rendez reqr;
	cookie db_cookie;
	cookie fb_cookie;
	Lock lock;
	long changed;
	int pageref;
	uint fstride;
	uint bstride;
	xendispl_page_directory *pages[1];
// do not place any other memebers here
} vdisplay;

static vdisplay* displ[1];
static int nvdispl = 0;
static int reqid;
static int max_pd = (BY2PG - offsetof(xendispl_page_directory, gref[0])) / sizeof(grant_ref_t);

static int
evtblock(void *a)
{	
	vdisplay *d = a;
	return d->evtrng->in_prod - d->evtrng->in_cons;
}

static void
evtwait(vdisplay *d)
{
	int cons;
	int prod;

	sleep(&d->evtr, evtblock, d);
	lfence();
	prod = d->evtrng->in_prod;
	cons = d->evtrng->in_cons;
	for(; cons != prod; cons++) {
		LOG(P("evtwait cons %d, prod %d\n", cons, prod))
		xendispl_evt *e = &XENDISPL_IN_RING_REF(d->evtrng, cons);
		switch(e->type) {
			case XENDISPL_EVT_PG_FLIP:
				LOG(P("evtwait: flip event: id %d\n", e->id))
				break;
		}
	}
	mfence();
	d->evtrng->in_cons = cons;
	xenchannotify(d->evtchn);
}

static void
evtintr(Ureg*, void *a)
{
	vdisplay *d = a;
	wakeup(&d->evtr);
}

static int
reqblock(void *a)
{
	vdisplay *d = a;
	int avail;

	lfence();
	RING_FINAL_CHECK_FOR_RESPONSES(&d->reqrng, avail);
	return avail;
}

static void
reqintr(Ureg*, void *a)
{
	vdisplay *d = a;
	wakeup(&d->reqr);
}

static void
reqwait(vdisplay *d)
{
	int i;
	xendispl_resp *r;

	sleep(&d->reqr, reqblock, d);
	i = d->reqrng.rsp_cons;
	lfence();
	r = RING_GET_RESPONSE(&d->reqrng, i);
	if(r->status != 0) {
		LOG(P("reqwait response: id %d, op %d, st %d\n", 
			r->id,
			r->operation,
			r->status));
	}
	sfence();
	d->reqrng.rsp_cons = ++i;
	xenchannotify(d->reqchn);
}

static void
reqsend(vdisplay *d, xendispl_req *r)
{
	int i;
	int notify;
	xendispl_req *req;

	r->id = reqid++;
 	lfence();
	i = d->reqrng.req_prod_pvt;
	req = RING_GET_REQUEST(&d->reqrng, i);
	memmove(req, r, sizeof(xendispl_req));
	d->reqrng.req_prod_pvt = ++i;
	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&d->reqrng, notify);
	sfence();
	
	if(notify) {
		xenchannotify(d->reqchn);
	}
	LOG(P("reqsend request: id %d, op %d\n", r->id, r->operation))
	reqwait(d);
	LOG(P("reqsend request responded: id %d, op %d\n",
		r->id, r->operation))
}

static void
fbflip(vdisplay *);

static void
flipproc(void *a)
{
	vdisplay *d = a;
	int ch;
	int i;

	for(;;) {
		tsleep(&up->sleep, return0, 0, 10);
		ch = cmpswap(&d->changed, 1, 0);
		if(ch == 0)
			continue;
		ilock(&d->lock);
		if(d->fstride != d->bstride){
			for(i = 0; i < d->height; i++)
			{
				memmove(d->dbuf + i * d->bstride, 
					d->intbuf + i * d->fstride, d->fstride);
			}
		}			
		else		
			memmove(d->dbuf, d->intbuf, d->bsize);
		iunlock(&d->lock);
		fbflip(d);
		evtwait(d);
	}
}

static void
chnalloc(vdisplay *d)
{
	xen_displif_sring_t *reqrng;

	d->evtchn = xenchanalloc(d->backend);
	d->evtrng = xspanalloc(BY2PG, BY2PG, 0);
	memset(d->evtrng, 0, BY2PG);

	d->reqchn = xenchanalloc(d->backend);
	reqrng = xspanalloc(BY2PG, BY2PG, 0);
	reqrng=memset(reqrng, 0, BY2PG);
	SHARED_RING_INIT(reqrng);
	FRONT_RING_INIT(&d->reqrng, reqrng, BY2PG);

	d->evtrngref = shareframe(d->backend, d->evtrng, 1);
	d->reqrngref = shareframe(d->backend, reqrng, 1);
	d->pageref = shareframe(d->backend, d->pages[0], 1);
	intrenable(d->evtchn, evtintr, d, BUSUNKNOWN, "vdispl_evt");
	intrenable(d->reqchn, reqintr, d, BUSUNKNOWN, "vdispl_req");
}

static void
dballoc(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_DBUF_CREATE;
	r.op.dbuf_create.dbuf_cookie = d->db_cookie.full;
	r.op.dbuf_create.width = d->width;
	r.op.dbuf_create.height = d->height;
	r.op.dbuf_create.bpp = d->bpp;
	r.op.dbuf_create.buffer_sz = d->bsize;
	r.op.dbuf_create.gref_directory = d->pageref;
	LOG(P("dballoc: op %d, w %d, h %d, b %d, s %d, ref %d\n",
		r.operation,
		r.op.dbuf_create.width,
		r.op.dbuf_create.height,
		r.op.dbuf_create.bpp,
		r.op.dbuf_create.buffer_sz,
		r.op.dbuf_create.gref_directory))
	reqsend(d, &r);
	LOG(P("dballoc done\n"))
}

static void
dbfree(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_DBUF_DESTROY;
	r.op.dbuf_create.dbuf_cookie = d->db_cookie.full;
	LOG(P("dbfree: op %d\n", r.operation))
	reqsend(d, &r);
	LOG(P("dbfree done\n"))
}

static void
fballoc(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_FB_ATTACH;
	r.op.fb_attach.dbuf_cookie = d->db_cookie.full;
	r.op.fb_attach.fb_cookie = d->fb_cookie.full;
	r.op.fb_attach.width = d->width;
	r.op.fb_attach.height = d->height;
	r.op.fb_attach.pixel_format = d->chan;
	LOG(P("fballoc: op %d\n", r.operation))
	reqsend(d, &r);
	LOG(P("fballoc done\n"))
}

static void
fbfree(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_FB_DETACH;
	r.op.fb_detach.fb_cookie = d->fb_cookie.full;
	LOG(P("fbfree: op %d\n", r.operation))
	reqsend(d, &r);
	LOG(P("fbfree done\n"))
}

static void
setconfig(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_SET_CONFIG;
	r.op.set_config.fb_cookie = d->fb_cookie.full;
	r.op.set_config.x = 0;
	r.op.set_config.y = 0;
	r.op.set_config.width = d->width;
	r.op.set_config.height = d->height;
	r.op.set_config.bpp = d->bpp;
	LOG(P("setconfig: op %d\n", r.operation))
	reqsend(d, &r);
	LOG(P("setconfig done\n"))
}

static void
fbflip(vdisplay *d)
{
	xendispl_req r;

	r.operation = XENDISPL_OP_PG_FLIP;
	r.op.pg_flip.fb_cookie = d->fb_cookie.full;
	LOG(P("fbflip: op %d\n", r.operation))
	reqsend(d, &r);
	LOG(P("fbflip done\n"))
}

static int
stride(int def)
{
	char *p;
	char *f[10];
	int v;

	p = getconf("stride");
	if(p == nil || getfields(p, f, nelem(f), 0, ",") < 1)
		return def;
	v = atoi(f[0]);
	if(v < def)
		v = def;
	return v;
}

static uchar*
dispinit(vdisplay **da, uint be, uint width, uint height, uint depth, ulong cc)
{
	uint fstride = width * ((depth + 7) / 8);
	uint fsize = fstride * height;
	uint fpc = (fsize + BY2PG - 1) / BY2PG;
	uint bsize;
	uint bpc;
	uint pc = (fpc + max_pd - 1) / max_pd - 1;
	uint dsize = sizeof(vdisplay) + sizeof(xendispl_page_directory*) * pc;
	vdisplay *d;
	uint m = 0;
	
	*da = xspanalloc(dsize, 0, 0);
	if(!*da) {
		panic("dispinit: xspanalloc");
	}
	d = *da;
	memset(d, 0, sizeof(vdisplay));
	d->backend = be;
	d->chan = cc;
	d->fstride = fstride;
	d->bstride = stride(fstride);
	bsize = d->bstride * height;
	d->bsize = bsize;
	bpc = (bsize + BY2PG - 1) / BY2PG;
	d->extbuf = xspanalloc(fpc * BY2PG, BY2PG, 0);
	d->intbuf = xspanalloc(fpc * BY2PG, BY2PG, 0);
	d->dbuf = xspanalloc(bpc * BY2PG, BY2PG, 0);
	if(!d->extbuf || !d->intbuf || !d->dbuf) {
		panic("dispinit: xspanalloc");
	}
	d->width = width;
	d->height = height;
	d->bpp = depth;
	d->db_cookie.parts.high = 0xFEEDBEEF;
	d->db_cookie.parts.low = 0xFEEDAFAF;
	d->fb_cookie.parts.high = 0xFEEDAFAF;
	d->fb_cookie.parts.low = 0xFEEDBEEF;

	for(int i = 0; m < bpc; i++) {
		d->pages[i] = xspanalloc(BY2PG, BY2PG, 0);
		memset(d->pages[i], 0, BY2PG);
		for(int j = 0; m < bpc && j < max_pd; j++) {
			d->pages[i]->gref[j] = 
				shareframe(d->backend, d->dbuf + m * BY2PG, 1);
			m++;
		}
	}
	for(int i = 1; i <= pc; i++) {
		d->pages[i - 1]->gref_dir_next_page = 
			shareframe(d->backend, d->pages[i], 1);
	}

	LOG(P("dispinit: dbuf 0x%ux, size %d, refs %d, max_pd %d, np %d\n", d->dbuf, d->bsize, m, max_pd, d->pages[0]->gref_dir_next_page))
	chnalloc(d);

	return d->extbuf;
}

static void
displinitdone(vdisplay *d)
{
	dballoc(d);
	fballoc(d);
	setconfig(d);
	kproc("vdispl_flip", flipproc, d);
}

static void
displdeinit(vdisplay *d)
{
	int n = (d->bsize + BY2PG - 1) / BY2PG;
	int pc = (n + max_pd - 1) / max_pd;
	int m = 0;

	intrdisable(d->evtchn, evtintr, d, BUSUNKNOWN, "vdispl_evt");
	intrdisable(d->reqchn, reqintr, d, BUSUNKNOWN, "vdispl_req");
	xengrantend(d->evtrngref);
	xengrantend(d->reqrngref);
	xfree(d->dbuf);
	xfree(d->intbuf);
	xfree(d->extbuf);
	xengrantend(d->pageref);
	for(int i = 0; i < pc; i++) {
		for(int j = 0; m < n && j < max_pd; j++) {
			xengrantend(d->pages[i]->gref[j]); 
			m++;
		}
		if(d->pages[i]->gref_dir_next_page)
			xengrantend(d->pages[i]->gref_dir_next_page);
		xfree(d->pages[i]);
	}
}

static int
fourcc(void)
{
	char *p;
	char *f[10];
	uint c = 0;
	char l;
	p = getconf("fourcc");
	if(p == nil || getfields(p, f, nelem(f), 0, ",") < 1)
		return 0;
	l = strlen(f[0]);
	if(l > sizeof(c)) {
		l = sizeof(c);
	}
	memset(&c, 0x20, sizeof(c));
	memmove(&c, f[0], l);
	return c;
}

uchar*
fbinit(int *width, int *height, int *depth, ulong *chan)
{
	char dir[64];
	char bend[64];
	char conn[64];
	char buf[64];
	char *f[2];
	char cc[5];
	int backend;
	int fcc = fourcc();

	sprint(dir, "device/vdispl/%d/", nvdispl);
	if(xenstore_gets(dir, "backend-id", buf, sizeof buf) <= 0) {
		print("displ: can't obtain backend-id from %s\n", dir);
		return 0;
	}
	backend = strtol(buf, 0, 0);

	sprint(conn, "%s%d/", dir, nvdispl);
	if(xenstore_gets(conn, "resolution", buf, sizeof buf) <= 0) {
		print("displ: can't obtain resolution from %s\n", conn);
		return 0;
	}
	if(getfields(buf, f, nelem(f), 0, "x") != nelem(f) ||
	    (*width = atoi(f[0])) < 16 || 
		(*height = atoi(f[1])) <= 0) {
		print("displ: can't obtain resolution from %s\n", buf);
		return 0;
	}

	switch(fcc){
	default:
		print("displ: unsupported screen depth %d\n", fcc);
		*chan = FOURCC_RGB565;
		/* fall through */
	case FOURCC_RGB565:
		*chan = RGB16;
		*depth = 16;
		break;
	case FOURCC_BGR888:
		*chan = BGR24;
		*depth = 24;
		break;
	case FOURCC_RGBA8888:
		*chan = RGBA32;
		*depth = 32;
		break;
	case FOURCC_ARGB8888:
		*chan = ARGB32;
		*depth = 32;
		break;
	case FOURCC_XRGB8888:
		*chan = XRGB32;
		*depth = 32;
		break;
	case FOURCC_ABGR8888:
		*chan = ABGR32;
		*depth = 32;
		break;
	case FOURCC_XBGR8888:
		*chan = XBGR32;
		*depth = 32;
		break;
	}

	memmove(cc, &fcc, sizeof(fcc));
	cc[4] = 0;
	print("displ: resolution %dx%dx%d, color schema %s\n", 
		*width, *height, *depth, cc);

	dispinit(&displ[nvdispl], backend, *width, *height, *depth, fcc);

	sprint(buf, "%u x %u", width, height);
	xenstore_sets(conn, "resolution", buf);
	xenstore_setd(conn, "evt-event-channel", displ[nvdispl]->evtchn);
	xenstore_setd(conn, "evt-ring-ref", displ[nvdispl]->evtrngref);
	xenstore_setd(conn, "req-event-channel", displ[nvdispl]->reqchn);
	xenstore_setd(conn, "req-ring-ref", displ[nvdispl]->reqrngref);

	xenstore_setd(dir, "state", XenbusStateInitialised);
	HYPERVISOR_yield();

	for(;;) {
		print("displ: connecting to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateConnected)
			break;
		print("displ: waiting for vdispl %s to connect\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}
	print("displ: connected to %s\n", bend);
	xenstore_setd(dir, "state", XenbusStateConnected);

	displinitdone(displ[nvdispl]);
	return displ[nvdispl]->extbuf;
}

int
fbblank(int)
{
	ilock(&displ[nvdispl]->lock);
	memset(displ[nvdispl]->intbuf, 0, displ[nvdispl]->bsize);
	iunlock(&displ[nvdispl]->lock);
	cmpswap(&displ[nvdispl]->changed, 0, 1);
	return 1;
}

void
fbupdate(void)
{
	ilock(&displ[nvdispl]->lock);
	memmove(displ[nvdispl]->intbuf, displ[nvdispl]->extbuf,
		displ[nvdispl]->bsize);
	iunlock(&displ[nvdispl]->lock);
	cmpswap(&displ[nvdispl]->changed, 0, 1);
}

enum {
	Qdir,
};

static Dirtab displtab[] = {
	".",	{Qdir, 0, QTDIR},	0,	0555,
};

static Chan*
_attach(char *spec)
{
	return devattach(L'v', spec);
}

static Walkqid*
_walk(Chan *c, Chan *nc, char **name, int nname)
{
	return devwalk(c, nc, name, nname, displtab, nelem(displtab), devgen);
}

static int
_stat(Chan *c, uchar *dp, int n)
{
	return devstat(c, dp, n, displtab, nelem(displtab), devgen);
}

static Chan*
_open(Chan *c, int omode)
{
	if(!iseve())
		error(Eperm);
	return devopen(c, omode, displtab, nelem(displtab), devgen);
}

static void
_close(Chan*)
{}

static long
_read(Chan *c, void *a, long n, vlong)
{
	if(c->qid.path == Qdir)
		return devdirread(c, a, n, displtab, nelem(displtab), devgen);
	error(Egreg);
	return 0;
}

static void
_init(void)
{
	screeninit();
}

static void
_shutdown(void)
{
	char dir[64];
	char buf[64];
	char bend[64];

	sprint(dir, "device/vdispl/%d/", nvdispl);
	xenstore_setd(dir, "state", XenbusStateClosing);
	HYPERVISOR_yield();

	for(;;) {
		print("vdispl: closing the connection to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateClosing)
			break;
		print("vdispl: waiting for vdispl %s\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}

	xenstore_setd(dir, "state", XenbusStateClosed);
	HYPERVISOR_yield();

	for(;;) {
		print("vdispl: close the connection to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateClosed)
			break;
		print("vdispl: waiting for vdispl %s\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}

	fbfree(displ[0]);
	dbfree(displ[0]);
	displdeinit(displ[0]);
}

Dev displdevtab = {
	'v',
	"displ",

	devreset,
	_init,
	_shutdown,
	_attach,
	_walk,
	_stat,
	_open,
	devcreate,
	_close,
	_read,
	devbread,
	0,
	devbwrite,
	devremove,
	devwstat,
};
