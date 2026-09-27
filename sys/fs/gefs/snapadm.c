/*
 * NetBSD: snapshot administration and automatic snapshots.
 *
 * 9front's adm/ctl commands and adm/status text (ctl.c), for the
 * kernel's GEFSIOC_CTL and GEFSIOC_STATUS and for gefs(8) on a device
 * that is not mounted; and 9front's snapshot schedule (fs.c's loadautos,
 * loadhist and cronsync), which its tasks proc ran every 5 seconds.
 *
 * Here the schedule is looked at by commits that write anyway, and an
 * interval in which a label did not change takes no snapshot: an idle
 * file system still does no I/O, and the snapshots kept are states that
 * differ.  Snapshots age out as on 9front.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <avl.h>

#include "dat.h"
#include "fns.h"
#include "ops.h"

/* 9front's schedule when neither the label nor the file system sets one */
static char Retaindef[] = "60@m 24@h @d";

static Cron crondef[] = {
	{.cnt=0, .div=60, .tag="minute"},
	{.cnt=0, .div=3600, .tag="hour"},
	{.cnt=0, .div=24*3600, .tag="day"},
};

/* --- time, in the UTC of the snapshot names: YYYY.MM.DD_hh:mm:ss --- */

/* days since 1970-01-01 of a Gregorian date, and back (H. Hinnant) */
static vlong
daysfrom(vlong y, int m, int d)
{
	vlong era, yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y-399) / 400;
	yoe = y - era*400;
	doy = (153*(m > 2 ? m-3 : m+9) + 2)/5 + d-1;
	doe = yoe*365 + yoe/4 - yoe/100 + doy;
	return era*146097 + doe - 719468;
}

static void
civil(vlong z, vlong *yp, int *mp, int *dp)
{
	vlong era, doe, yoe, doy, m5;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era*146097;
	yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
	doy = doe - (365*yoe + yoe/4 - yoe/100);
	m5 = (5*doy + 2)/153;
	*dp = doy - (153*m5 + 2)/5 + 1;
	*mp = m5 < 10 ? m5+3 : m5-9;
	*yp = yoe + era*400 + (*mp <= 2);
}

char*
seprintsnaptime(char *p, char *e, vlong t)
{
	vlong days, y;
	int m, d, s;

	days = t / 86400;
	s = t % 86400;
	if(s < 0){
		s += 86400;
		days--;
	}
	civil(days, &y, &m, &d);
	return seprint(p, e, "%04lld.%02d.%02d_%02d:%02d:%02d",
		y, m, d, s/3600, s/60%60, s%60);
}

int
parsesnaptime(char *s, vlong *t)
{
	static char sep[] = ".._::";
	vlong v[6];
	int i;

	for(i = 0; i < 6; i++){
		if(*s < '0' || *s > '9')
			return -1;
		v[i] = 0;
		while(*s >= '0' && *s <= '9')
			v[i] = v[i]*10 + *s++ - '0';
		if(i < 5 && *s++ != sep[i])
			return -1;
	}
	if(*s != 0 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31)
		return -1;
	*t = daysfrom(v[0], v[1], v[2])*86400 + v[3]*3600 + v[4]*60 + v[5];
	return 0;
}

/* --- words, as 9front's tokenize: blanks separate, 'quotes' group --- */

static int
isblank9(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int
ctltokenize(char *s, char **args, int maxargs)
{
	char *w;
	int n, q;

	n = 0;
	while(n < maxargs){
		while(isblank9(*s))
			s++;
		if(*s == 0)
			break;
		args[n++] = w = s;
		q = 0;
		while(*s != 0){
			if(*s == '\''){
				if(q && s[1] == '\''){
					*w++ = '\'';
					s += 2;
				}else{
					q = !q;
					s++;
				}
				continue;
			}
			if(!q && isblank9(*s))
				break;
			*w++ = *s++;
		}
		if(*s != 0)
			s++;
		*w = 0;
	}
	return n;
}

static int
getnum(char **pp, vlong *v)
{
	char *p;

	p = *pp;
	if(*p < '0' || *p > '9')
		return 0;
	*v = 0;
	while(*p >= '0' && *p <= '9' && *v < 1000000000)
		*v = *v*10 + *p++ - '0';
	*pp = p;
	return 1;
}

/* --- the schedule --- */

static void
freeautos(Mount *mnt)
{
	int i;

	for(i = 0; i < nelem(mnt->cron); i++){
		free(mnt->cron[i].lbl);
		mnt->cron[i].lbl = nil;
		mnt->cron[i].cnt = 0;
		mnt->cron[i].div = 0;
	}
}

/*
 * Parse a retention spec, "count@interval unit ...", into crons;
 * with crons nil, only check it.  9front's grammar: the count may be
 * left out (keep them all), and so may the interval (1).
 */
static int
parseretain(char *s, Cron *crons)
{
	vlong cnt, div;
	char *p;
	int i, op;

	p = s;
	while(isblank9(*p))
		p++;
	while(*p){
		cnt = -1;
		div = 1;
		op = -1;
		getnum(&p, &cnt);
		if(*p++ != '@')
			return 0;
		getnum(&p, &div);
		if(*p == 'm' || *p == 'h' || *p == 'd')
			op = *p++;
		if(div <= 0 || (*p != 0 && !isblank9(*p)))
			return 0;
		while(isblank9(*p))
			p++;
		for(i = 0; i < nelem(crondef); i++)
			if(crondef[i].tag[0] == op)
				break;
		if(i == nelem(crondef))
			return 0;
		if(crons != nil){
			crons[i].cnt = cnt;
			crons[i].div = div*crondef[i].div;
		}
	}
	return 1;
}

/* a label's spec: its own, the file system's, or 9front's default */
static char*
retainspec(Mount *mnt, char *buf, int nbuf)
{
	char pfx[16];
	Kvp kv, r;
	int n;

	pfx[0] = Kconf;
	n = snprint(pfx+1, sizeof(pfx)-1, "retain");
	kv.k = pfx;
	kv.nk = n+1;
	if((mnt != nil && btlookup(agetp(&mnt->root), &kv, &r, buf, nbuf-1))
	|| btlookup(&fs->snap, &kv, &r, buf, nbuf-1)){
		r.v[r.nv] = 0;
		return r.v;
	}
	return Retaindef;
}

/*
 * The snapshots a label already has, oldest first (the time in the name
 * sorts), into the ring; the ones beyond the count go, as when the count
 * was lowered.  They are dropped after the scan of the snap tree, which
 * dropping them changes.
 */
static void
loadhist(Mount *mnt, Cron *c)
{
	char pfx[128], buf[128], (*del)[128];
	int i, ns, ndel, maxdel;
	Scan *s;

	i = 0;
	buf[0] = 0;
	pfx[0] = Klabel;
	ns = snprint(pfx+1, sizeof(pfx)-1, "%s@%s.", mnt->name, c->tag);
	ndel = 0;
	maxdel = 0;
	del = nil;
	s = emalloc(sizeof(Scan), 1);	/* 1.5 KiB */
	if(waserror()){
		free(s);
		free(del);
		nexterror();
	}
	btnewscan(s, pfx, ns+1);
	btenter(&fs->snap, s);
	while(1){
		if(!btnext(s, &s->kv))
			break;
		if(s->kv.nk-1 >= sizeof(c->lbl[0])-1)
			continue;
		if(ndel == maxdel){
			maxdel = maxdel ? 2*maxdel : 16;
			del = realloc(del, maxdel*sizeof(del[0]));
		}
		memcpy(buf, s->kv.k+1, s->kv.nk-1);
		buf[s->kv.nk-1] = 0;
		if(c->cnt == 0){
			memcpy(del[ndel++], buf, sizeof(buf));
			continue;
		}
		if(c->lbl[i][0] != 0 && c->cnt > 0)
			memcpy(del[ndel++], c->lbl[i], sizeof(buf));
		memcpy(c->lbl[i], buf, sizeof(buf));
		i = (c->cnt > 0) ? (i+1) % c->cnt : 0;
	}
	btexit(s);
	poperror();
	free(s);
	if(buf[0] == 0 || parsesnaptime(buf+ns, &c->last) == -1)
		c->last = 0;
	c->i = i;
	c->chg = mnt->nchange;
	for(i = 0; i < ndel; i++)
		autodel(del[i]);
	free(del);
}

/*
 * Set up a mutable label's schedule (on getmount, and when it changes).
 * A bad spec takes no snapshots, so that the label can still be mounted
 * and the spec fixed.
 */
void
loadautos(Mount *mnt)
{
	char *spec, *rbuf;
	Cron crons[nelem(mnt->cron)];
	int i, n;

	freeautos(mnt);
	if(!(mnt->flag & Lmut) || mnt == fs->snapmnt)
		return;
	rbuf = emalloc(Kvmax+1, 0);
	if(waserror()){
		free(rbuf);
		nexterror();
	}
	spec = retainspec(mnt, rbuf, Kvmax+1);
	memcpy(crons, crondef, sizeof(crons));
	if(!parseretain(spec, crons)){
		fprint(2, "gefs: %s: ignoring invalid snapshot schedule %q\n",
			mnt->name, spec);
		poperror();
		free(rbuf);
		return;
	}
	poperror();
	free(rbuf);
	for(i = 0; i < nelem(crons); i++){
		mnt->cron[i] = crons[i];
		if(crons[i].div == 0)
			continue;
		n = (crons[i].cnt <= 0) ? 1 : crons[i].cnt;
		mnt->cron[i].lbl = emalloc(n*sizeof(mnt->cron[i].lbl[0]), 1);
		loadhist(mnt, &mnt->cron[i]);
	}
}

/* at the Mount's end, from the limbo list */
void
freemount(Mount *mnt)
{
	freeautos(mnt);
	free(mnt);
}

/* snapshot label as name now; with mutlk */
static int
autotake(char *label, char *name)
{
	if(waserror()){
		fprint(2, "gefs: snapshot %s: %s\n", name, errmsg());
		return 0;
	}
	snapfs(label, name, 0, 0);
	poperror();
	return 1;
}

/*
 * Drop an automatic snapshot.  Its label goes out with the next commit;
 * blocks only it held are swept after that commit.
 */
void
autodel(char *name)
{
	Tree *t;

	if(waserror()){
		fprint(2, "gefs: removing snapshot %s: %s\n", name, errmsg());
		return;
	}
	qlock(&fs->mutlk);
	if(waserror()){
		qunlock(&fs->mutlk);
		nexterror();
	}
	t = snapfs(name, nil, 0, 1);
	qunlock(&fs->mutlk);
	poperror();
	if(t != nil)
		pushsweep(t);
	poperror();
}

/*
 * On a commit that writes: take the automatic snapshots that are due
 * (an interval passed, and the label changed since that schedule's last
 * snapshot), and age out the oldest.
 */
void
autosnaps(void)
{
	char name[128], *p, *e;
	vlong now;
	Mount *mnt;
	Cron *c;
	int i;

	if(agetl(&fs->rdonly))
		return;
	now = nsec()/Nsec;
	for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->mnext){
		if(!(mnt->flag & Lmut) || mnt == fs->snapmnt)
			continue;
		for(i = 0; i < nelem(mnt->cron); i++){
			c = &mnt->cron[i];
			if(c->div == 0 || c->cnt == 0 || c->lbl == nil)
				continue;
			if(now/c->div == c->last/c->div || mnt->nchange == c->chg)
				continue;
			p = name;
			e = name + sizeof(name);
			p = seprint(p, e, "%s@%s.", mnt->name, c->tag);
			p = seprintsnaptime(p, e, now);
			c->last = now;
			c->chg = mnt->nchange;
			if(p - name >= sizeof(c->lbl[0]) - 1)
				continue;
			if(c->cnt > 0 && c->lbl[c->i][0] != 0)
				autodel(c->lbl[c->i]);
			qlock(&fs->mutlk);
			if(autotake(mnt->name, name)){
				memcpy(c->lbl[c->i], name, p - name + 1);
				c->i = (c->cnt > 0) ? (c->i+1) % c->cnt : 0;
			}
			qunlock(&fs->mutlk);
		}
	}
}

/* --- commands --- */

/* a label that a command may create: mountable (fits Mount.name) */
static void
chklabel(char *name)
{
	int i, n;

	n = strlen(name);
	if(n == 0 || n >= sizeof(((Mount*)0)->name))
		error(Elength);
	for(i = 0; i < n; i++)
		if((name[i]&0xff) < 0x20 || name[i] == '/')
			error(Ename);
}

/* 9front's setconf: a Kconf key in a label's tree, or the file system's */
static void
setconf(int op, char *label, char *key, char *val)
{
	char kbuf[64], *xbuf;
	Mount *mnt;
	Tree *t;
	Msg m;
	Kvp x;
	int n;

	mnt = nil;
	t = &fs->snap;
	xbuf = emalloc(Kvmax, 0);
	if(waserror()){
		free(xbuf);
		if(mnt != nil)
			clunkmount(mnt);
		nexterror();
	}
	if(label[0] != 0){
		mnt = getmount(label);
		t = agetp(&mnt->root);
		if((mnt->flag & Lmut) == 0)
			error(Eperm);
	}
	kbuf[0] = Kconf;
	n = snprint(kbuf+1, sizeof(kbuf)-1, "%s", key);
	m.op = op;
	m.k = kbuf;
	m.nk = n+1;
	m.v = val;
	m.nv = strlen(val);
	qlock(&fs->mutlk);
	if(waserror()){
		qunlock(&fs->mutlk);
		nexterror();
	}
	if(op == Oinsert || btlookup(t, &m, &x, xbuf, Kvmax)){
		if(mnt != nil)
			upsert(mnt, &m, 1);
		else
			btupsert(&fs->snap, &m, 1);
	}
	qunlock(&fs->mutlk);
	poperror();
	/* committed while the label is held, or the change is not synced */
	commit(0);
	if(mnt != nil)
		clunkmount(mnt);
	mnt = nil;
	poperror();
	free(xbuf);
}

static void
setretain(char *label, char **spec, int nspec)
{
	char val[128], *p, *e;
	Mount *mnt;
	int i, op;

	val[0] = 0;
	if(nspec == 1 && strcmp(spec[0], "default") == 0)
		op = Odelete;
	else{
		op = Oinsert;
		p = val;
		e = val + sizeof(val);
		if(nspec != 1 || strcmp(spec[0], "none") != 0)
			for(i = 0; i < nspec; i++)
				p = seprint(p, e, "%s%s", i ? " " : "", spec[i]);
		if(p >= e-1)
			error(Elength);
		if(!parseretain(val, nil))
			error(Esyntax);
	}
	if(strcmp(label, "-") == 0)
		label = "";
	setconf(op, label, "retain", val);
	/*
	 * The labels it applies to follow it now, which ages out what the
	 * new spec does not keep; unmounted labels follow the default when
	 * they are next mounted.
	 */
	if(label[0] != 0){
		mnt = getmount(label);
		if(waserror()){
			clunkmount(mnt);
			nexterror();
		}
		loadautos(mnt);
		commit(0);
		poperror();
		clunkmount(mnt);
	}else{
		for(mnt = agetp(&fs->mounts); mnt != nil; mnt = mnt->mnext)
			loadautos(mnt);
		commit(0);
	}
}

/*
 * One command of 9front's adm/ctl (see gefs.h); errors are raised.
 * The check's findings go to fd.  For snap and fork of a mounted label,
 * the caller first puts the label's dirty pages into its tree.
 */
void
ctlcmd(char *cmd, int fd)
{
	char buf[256], *sp[16];
	vlong v;
	int nsp;

	if(strlen(cmd) >= sizeof(buf))
		error(Elength);
	strecpy(buf, buf+sizeof(buf), cmd);
	nsp = ctltokenize(buf, sp, nelem(sp));
	if(nsp == 0)
		error(Ebadctl);
	if(strcmp(sp[0], "sync") == 0 && nsp == 1)
		commit(Clazy);
	else if(strcmp(sp[0], "check") == 0 && nsp == 1){
		qlock(&fs->mutlk);
		v = checkfs(fd);
		qunlock(&fs->mutlk);
		if(!v)
			error(Echeck);
	}else if((strcmp(sp[0], "snap") == 0 || strcmp(sp[0], "fork") == 0)
	&& nsp == 3){
		chklabel(sp[2]);
		snapcmd(sp[1], sp[2], (sp[0][0] == 'f') ? Lmut : 0);
	}else if(strcmp(sp[0], "delsnap") == 0 && nsp == 2)
		snapcmd(sp[1], nil, 0);
	else if(strcmp(sp[0], "retain") == 0 && nsp >= 3)
		setretain(sp[1], sp+2, nsp-2);
	else if(strcmp(sp[0], "usereserve") == 0 && nsp == 2
	&& (v = -1, getnum(&sp[1], &v)) && *sp[1] == 0)
		usereserve = v != 0;
	else
		error(Ebadctl);
}

/* --- status, as 9front's adm/status --- */

static char*
seprintsize(char *p, char *e, uvlong n)
{
	static char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
	uvlong d;
	int u;

	d = 1;
	for(u = 0; u < nelem(units)-1 && n/d >= 500; u++)
		d *= 1024;
	return seprint(p, e, "%llud.%02llud%s", n/d, (n%d)*100/d, units[u]);
}

static char*
showconf(char *p, char *e, Tree *t, char *name)
{
	char pfx[1];
	Scan *s;
	int shown;

	shown = 0;
	pfx[0] = Kconf;
	s = emalloc(sizeof(Scan), 1);
	if(waserror()){
		free(s);
		nexterror();
	}
	btnewscan(s, pfx, 1);
	btenter(t, s);
	while(1){
		if(!btnext(s, &s->kv))
			break;
		p = seprint(p, e, "conf %q %.*q %.*q\n", name,
			(int)s->kv.nk-1, s->kv.k+1, (int)s->kv.nv, s->kv.v);
		shown = 1;
	}
	btexit(s);
	poperror();
	free(s);
	if(name[0] == 0 && !shown)
		p = seprint(p, e, "conf '' retain %q\n", Retaindef);
	return p;
}

/*
 * The status text: usage, the retention settings, and every label, as
 * "fork" (mutable) or "snap".  Returns where the text ends; it is cut
 * off at e.
 */
char*
seprintstatus(char *p, char *e)
{
	char name[Keymax], pfx[1];
	uvlong size, used;
	Arena *a;
	Scan *s;
	Tree *t;
	uint flg;
	int i, pass;

	size = used = 0;
	for(i = 0; i < fs->narena; i++){
		a = &fs->arenas[i];
		qlock(a);
		size += a->size;
		used += a->used;
		qunlock(a);
	}
	p = seprint(p, e, "gefs netbsd\n");
	p = seprint(p, e, "usage \t%llud.%02llud%%\t",
		size ? used*100/size : 0, size ? used*10000/size%100 : 0);
	p = seprintsize(p, e, used);
	p = seprint(p, e, "\t");
	p = seprintsize(p, e, size);
	p = seprint(p, e, "\n");
	p = showconf(p, e, &fs->snap, "");

	s = emalloc(sizeof(Scan), 1);
	if(waserror()){
		free(s);
		nexterror();
	}
	/* first the settings of the mutable labels, then all the labels */
	for(pass = 0; pass < 2; pass++){
		pfx[0] = Klabel;
		btnewscan(s, pfx, 1);
		btenter(&fs->snap, s);
		while(1){
			if(!btnext(s, &s->kv))
				break;
			flg = UNPACK32(s->kv.v+1+8);
			memcpy(name, s->kv.k+1, s->kv.nk-1);
			name[s->kv.nk-1] = 0;
			if(pass == 1){
				p = seprint(p, e, "%s %q\n",
					(flg & Lmut) ? "fork" : "snap", name);
				continue;
			}
			if(!(flg & Lmut) || strcmp(name, "adm") == 0)
				continue;
			if((t = opensnap(name, nil)) == nil)
				continue;
			if(waserror()){
				closesnap(t);
				nexterror();
			}
			p = showconf(p, e, t, name);
			poperror();
			closesnap(t);
		}
		btexit(s);
	}
	poperror();
	free(s);
	return p;
}
