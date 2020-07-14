/*
* Xen keyboard and mouse interfaces frontend
* Copyright (C) 2020 Alexander Sychev
*/
#include	"u.h"
#include	"../port/lib.h"
#include	"mem.h"
#include	"dat.h"
#include	"fns.h"
#include "io.h"
#include "../port/error.h"
#define	Image	IMAGE
#include <draw.h>
#include <memdraw.h>
#include <cursor.h>
#include "screen.h"

#define LOG(a)
//#define LOG(a) a;
//#define P print
#define P dprint

enum {
	Qdir,
	Qscancode,
};

static Dirtab inputtab[] = {
	".",		{Qdir, 0, QTDIR},	0,	0555,
	"scancode",	{Qscancode, 0},		0,	0440,
};

typedef struct xenkbd_page xenkbd_page;
typedef union xenkbd_in_event xenkbd_in_event;

typedef struct input {
	int	backend;
	int	evtchn;
	xenkbd_page *page;
	int pageref;
	Rendez r;
	Ref ref;
	Queue *q;
} input;

static input vinput[1];
static int nvinput = 0;

static void
inputintr(Ureg*, void *a)
{
	input *in = a;
	wakeup(&in->r);
}

static int
inputblock(void *a)
{	
	input *in = a;
	lfence();
	return in->page->in_prod - in->page->in_cons;
}

static void
inputproc(void *a)
{
	input *in = a;
	int buttons = 0;
	int cons;
	int prod;

	LOG(P("inputproc\n"))
	for(;;) {
		sleep(&in->r, inputblock, in);
		lfence();
		prod = in->page->in_prod;
		cons = in->page->in_cons;
		LOG(P("inputproc: prod %d, cons %d\n", prod, cons))
		for(; cons != prod; cons++) {
			xenkbd_in_event *e = &XENKBD_IN_RING_REF(in->page, cons);
			LOG(P("inputproc: t %d\n", e->type))
			switch(e->type) {
				case XENKBD_TYPE_MOTION: {
					LOG(P("inputproc: mm %d, %d\n",
						e->motion.rel_x, e->motion.rel_y))
					/* wheel emulates 4 or 5 button clicks */
					buttons &= ~24;
					if (e->motion.rel_z > 0) {
							LOG(P("inputproc: wheel up\n"))	
							buttons |= 8;
					} else if (e->motion.rel_z < 0) {
							LOG(P("inputproc: wheel down\n"))		
							buttons |= 16;
					}
					mousetrack(e->motion.rel_x, e->motion.rel_y, 
								buttons, TK2MS(MACHP(0)->ticks));
					break;
				}
				case XENKBD_TYPE_POS:{
					LOG(P("inputproc: abs pos %d, %d, %d\n",
						e->pos.abs_x, e->pos.abs_y, e->pos.rel_z ))
					absmousetrack(e->pos.abs_x, e->pos.abs_y, 
								  buttons, TK2MS(MACHP(0)->ticks));
					break;
				}
				case XENKBD_TYPE_KEY: {
					LOG(P("inputproc: key %d, pressed %d\n",
						e->key.keycode, e->key.pressed))
					
					switch(e->key.keycode){
						case 0x110: /*left button*/
							LOG(P("inputproc: left button\n"))
							if(e->key.pressed)
								buttons |= 1;
							else
								buttons &= ~1;
							mousetrack(0, 0, buttons, 
										TK2MS(MACHP(0)->ticks));
							break;
						case 0x111: /*right button*/
							LOG(P("inputproc: right button\n"))
							if(e->key.pressed)
								buttons |= 4;
							else
								buttons &= ~4;
							mousetrack(0, 0, buttons, 
										TK2MS(MACHP(0)->ticks));
							break;
						case 0x112: /*middle button*/
							LOG(P("inputproc: middle button\n"))
							if(e->key.pressed)
								buttons |= 2;
							else
								buttons &= ~2;
							mousetrack(0, 0, buttons, 
										TK2MS(MACHP(0)->ticks));
							break;
						default: {
							LOG(P("inputproc: keyboard type\n"))
							uchar b = e->key.keycode & 0x7f;
							if(!e->key.pressed)
								b |= 0x80;
							qproduce(in->q, &b, sizeof b);
						}
					}
					break;
				}
			}
		}
		LOG(P("inputproc: at end prod %d, cons %d\n", prod, cons))
		mfence();
		in->page->in_cons = cons;
	}
}

static Chan*
_attach(char *spec)
{
	LOG(P("_attach\n"))
	return devattach(L'b', spec);
}

static Walkqid*
_walk(Chan *c, Chan *nc, char **name, int nname)
{
	LOG(P("_walk\n"))
	return devwalk(c, nc, name, nname, inputtab, nelem(inputtab), devgen);
}

static int
_stat(Chan *c, uchar *dp, int n)
{
	LOG(P("_stat\n"))
	return devstat(c, dp, n, inputtab, nelem(inputtab), devgen);
}

static Chan*
_open(Chan *c, int omode)
{
	LOG(P("_open: %d\n", vinput[nvinput].ref))
	if(!iseve())
		error(Eperm);
	if(c->qid.path == Qscancode){
		if(waserror()){
			decref(&vinput[nvinput].ref);
			nexterror();
		}
		if(incref(&vinput[nvinput].ref) != 1)
			error(Einuse);
		c = devopen(c, omode, inputtab, nelem(inputtab), devgen);
		poperror();
		return c;
	}
	return devopen(c, omode, inputtab, nelem(inputtab), devgen);
}

static void
_close(Chan *c)
{
	LOG(P("_close\n"))
	if((c->flag & COPEN) && c->qid.path == Qscancode)
		decref(&vinput[nvinput].ref);
}

static Block*
_bread(Chan *c, long n, ulong off)
{
	LOG(P("_bread\n"))
	if(c->qid.path == Qscancode){
		return qbread(vinput[nvinput].q, n);
	}
	return devbread(c, n, off);
}

static long
_read(Chan *c, void *a, long n, vlong)
{
	LOG(P("_read\n"))
	if(c->qid.path == Qscancode){
		return qread(vinput[nvinput].q, a, n);
	}
	if(c->qid.path == Qdir)
		return devdirread(c, a, n, inputtab, nelem(inputtab), devgen);
	error(Egreg);
	return 0;
}

static void
inputinit(input *in, int backend)
{
	LOG(P("inputinit\n"))
	in->q = qopen(0x1000, Qcoalesce, 0, 0);
	if(in->q == nil)
		panic("vinput: qopen");
	qnoblock(in->q, 1);
	
	in->backend = backend;
	in->evtchn = xenchanalloc(in->backend);
	in->page = xspanalloc(BY2PG, BY2PG, 0);
	memset(in->page, 0, BY2PG);
	in->pageref = shareframe(in->backend, in->page, 1);

	intrenable(in->evtchn, inputintr, in, BUSUNKNOWN, "vinput_evt");
	kproc("vinput", inputproc, in);
}

static void
inputdeinit(input *in)
{
	LOG(P("inputdeinit\n"))
	xengrantend(in->backend);
	xfree(in->page);
	qclose(in->q);
}

static void
_init(void)
{
	char dir[64];
	char buf[64];
	char bend[64];
	int backend;

	LOG(P("_init\n"))
	sprint(dir, "device/vkbd/%d/", nvinput);
	if (xenstore_gets(dir, "backend-id", buf, sizeof buf) <= 0) {
		print("kbd: can't obtain backend-id from %s\n", dir);
		return;
	}

	backend = strtol(buf, 0, 0);
	inputinit(&vinput[nvinput], backend);

	xenstore_setd(dir, "page-gref", vinput[nvinput].pageref);
	xenstore_setd(dir, "event-channel", vinput[nvinput].evtchn);
	xenstore_setd(dir, "request-abs-pointer", 1);
	xenstore_setd(dir, "state", XenbusStateInitialised);
	HYPERVISOR_yield();

	for(;;) {
		print("vinput: connecting to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateConnected)
			break;
		print("vinput: waiting for vkbd %s\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}
	print("vinput: connected to %s\n", bend);
	xenstore_setd(dir, "state", XenbusStateConnected);
}

static void
_shutdown(void)
{
	char dir[64];
	char buf[64];
	char bend[64];

	LOG(P("_shutdown\n"))
	sprint(dir, "device/vkbd/%d/", nvinput);
	xenstore_setd(dir, "state", XenbusStateClosing);
	HYPERVISOR_yield();

	for(;;) {
		print("vinput: closing the connection to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateClosing)
			break;
		print("vinput: waiting for vkbd %s\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}

	xenstore_setd(dir, "state", XenbusStateClosed);
	HYPERVISOR_yield();

	for(;;) {
		print("vinput: close the connection to %s\n", dir);
		xenstore_gets(dir, "backend", bend, sizeof bend);
		xenstore_gets(bend, "/state", buf, sizeof buf);
		if(strtol(buf, 0, 0) == XenbusStateClosed)
			break;
		print("vinput: waiting for vkbd %s\n", dir);
		tsleep(&up->sleep, return0, 0, 50);
	}

	inputdeinit(&vinput[nvinput]);
}

Dev inputdevtab = {
	L'b',
	"vinput",

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
	_bread,
	nil,
	devbwrite,
	devremove,
	devwstat,
};

