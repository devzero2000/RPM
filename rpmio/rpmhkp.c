#include "system.h"

#define _RPMHKP_INTERNAL
#include <rpmhkp.h>

#define	_RPMPGP_INTERNAL
#include <rpmpgp.h>

#include <rpmmacro.h>

#include "debug.h"

/*@unchecked@*/
int _rpmhkp_debug = 0;

/*@unchecked@*/ /*@relnull@*/
rpmhkp _rpmhkpI = NULL;

struct _filter_s _rpmhkp_awol	= { .n = 100000, .e = 1.0e-4 };
struct _filter_s _rpmhkp_crl	= { .n = 100000, .e = 1.0e-4 };

_BAstats _rpmhkp_stats;
/* XXX renaming work-in-progress */
#define	SUM	_rpmhkp_stats

int _rpmhkp_spew;
#define	SPEW(_list)	if (_rpmhkp_spew) fprintf _list
#if 0
#define	DESPEW(_list)	           fprintf _list
#else
#define	DESPEW(_list)	SPEW(_list)
#endif

/*==============================================================*/

static rpmhkp rpmhkpI(void)
	/*@globals _rpmhkpI @*/
	/*@modifies _rpmhkpI @*/
{
    if (_rpmhkpI == NULL)
	_rpmhkpI = rpmhkpNew(NULL, 0);
    return _rpmhkpI;
}

static void rpmhkpFini(void * _hkp)
        /*@globals fileSystem @*/
        /*@modifies *_hkp, fileSystem @*/
{
    rpmhkp hkp = _hkp;

assert(hkp);
    hkp->pkt = _free(hkp->pkt);
    hkp->pktlen = 0;
    hkp->pkts = _free(hkp->pkts);
    hkp->npkts = 0;
    hkp->awol = rpmbfFree(hkp->awol);
    hkp->crl = rpmbfFree(hkp->crl);
}

/*@unchecked@*/ /*@only@*/ /*@null@*/
rpmioPool _rpmhkpPool;

static rpmhkp rpmhkpGetPool(/*@null@*/ rpmioPool pool)
        /*@globals _rpmhkpPool, fileSystem @*/
        /*@modifies pool, _rpmhkpPool, fileSystem @*/
{
    rpmhkp hkp;

    if (_rpmhkpPool == NULL) {
        _rpmhkpPool = rpmioNewPool("hkp", sizeof(*hkp), -1, _rpmhkp_debug,
                        NULL, NULL, rpmhkpFini);
        pool = _rpmhkpPool;
    }
    hkp = (rpmhkp) rpmioGetPool(pool, sizeof(*hkp));
    memset(((char *)hkp)+sizeof(hkp->_item), 0, sizeof(*hkp)-sizeof(hkp->_item));
    return hkp;
}

rpmhkp rpmhkpNew(const rpmuint8_t * keyid, uint32_t flags)
{
    /* XXX watchout for recursive call. */
    rpmhkp hkp = (flags & 0x80000000) ? rpmhkpI() : rpmhkpGetPool(_rpmhkpPool);

    if (keyid) {
	memcpy(hkp->keyid, keyid, sizeof(hkp->keyid));

	/* XXX watchout for recursive call. */
	if (_rpmhkp_awol.bf)
	    hkp->awol = rpmbfLink(_rpmhkp_awol.bf);
	if (_rpmhkp_crl.bf)
	    hkp->crl = rpmbfLink(_rpmhkp_crl.bf);
    }

hkp->pubx = -1;
hkp->uidx = -1;
hkp->subx = -1;
hkp->sigx = -1;

hkp->tvalid = 0;
hkp->uvalidx = -1;

    return rpmhkpLink(hkp);
}

/*==============================================================*/

static const char * _pgpSigType2Name(uint32_t sigtype)
{
    return pgpValStr(pgpSigTypeTbl, (rpmuint8_t)sigtype);
}

static const char * _pgpHashAlgo2Name(uint32_t algo)
{
    return pgpValStr(pgpHashTbl, (rpmuint8_t)algo);
}

static const char * _pgpPubkeyAlgo2Name(uint32_t algo)
{
    return pgpValStr(pgpPubkeyTbl, (rpmuint8_t)algo);
}

struct pgpPkt_s {
    pgpTag tag;
    unsigned int pktlen;
    union {
	const rpmuint8_t * h;
	const pgpPktKeyV3 j;
	const pgpPktKeyV4 k;
	const pgpPktSigV3 r;
	const pgpPktSigV4 s;
	const pgpPktUid * u;
    } u;
    unsigned int hlen;
};

static const rpmuint8_t * pgpGrabSubTagVal(const rpmuint8_t * h, size_t hlen,
		rpmuint8_t subtag, /*@null@*/ size_t * tlenp)
{
    const rpmuint8_t * p = h;
    const rpmuint8_t * pend = h + hlen;
    unsigned plen = 0;
    unsigned len;
    rpmuint8_t stag;

    if (tlenp)
	*tlenp = 0;

    while (p < pend) {
	len = pgpLen(p, &plen);
	p += len;

	stag = (*p & ~PGPSUBTYPE_CRITICAL);

	if (stag == subtag) {
SPEW((stderr, "\tSUBTAG %02X %p[%2u]\t%s\n", stag, p+1, plen-1, pgpHexStr(p+1, plen-1)));
	    if (tlenp)
		*tlenp = plen-1;
	    return p+1;
	}
	p += plen;
    }
    return NULL;
}

static const rpmuint8_t * ppSigHash(pgpPkt pp, size_t * plen)
{
    const rpmuint8_t * p = NULL;

assert(pp->tag == PGPTAG_SIGNATURE);
    switch (pp->u.h[0]) {
    case 4:
	*plen = pgpGrab(pp->u.s->hashlen, sizeof(pp->u.s->hashlen));
	p = pp->u.h + sizeof(*pp->u.s);
	break;
    }
    return p;
}

static const rpmuint8_t * ppSigUnhash(pgpPkt pp, size_t * plen)
{
    const rpmuint8_t * p = NULL;

assert(pp->tag == PGPTAG_SIGNATURE);
    switch (pp->u.h[0]) {
    case 4:
	p = pp->u.h + sizeof(*pp->u.s);
	p += pgpGrab(pp->u.s->hashlen, sizeof(pp->u.s->hashlen));
	*plen = pgpGrab(p, 2);
	p += 2;
	break;
    }
    return p;
}

static const rpmuint8_t * ppSignid(pgpPkt pp)
{
    const rpmuint8_t * p = NULL;
    size_t nunhash = 0;
    const rpmuint8_t * punhash;
    size_t tlen = 0;

assert(pp->tag == PGPTAG_SIGNATURE);
    switch (pp->u.h[0]) {
    case 3:	 p = pp->u.r->signid;		break;
    case 4:
	punhash = ppSigUnhash(pp, &nunhash);
	p = pgpGrabSubTagVal(punhash, nunhash, PGPSUBTYPE_ISSUER_KEYID, &tlen);
assert(p == NULL || tlen == 8);
	break;
    }
    return p;
}

static rpmuint32_t ppSigTime(pgpPkt pp)
{
    const rpmuint8_t * p = NULL;
    size_t nhash = 0;
    const rpmuint8_t * phash;
    size_t tlen = 0;
    rpmuint32_t sigtime = 0;

assert(pp->tag == PGPTAG_SIGNATURE);
    switch (pp->u.h[0]) {
    case 3:	sigtime = pgpGrab(pp->u.r->time, sizeof(pp->u.r->time)); break;
    case 4:
	phash = ppSigHash(pp, &nhash);
	p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_SIG_CREATE_TIME, &tlen);
	if (p)	sigtime = pgpGrab(p, 4);
	break;
    }
    return sigtime;
}

static rpmuint8_t ppSigType(pgpPkt pp)
{
    rpmuint8_t sigtype = 0;
assert(pp->tag == PGPTAG_SIGNATURE);
    switch (pp->u.h[0]) {
    case 3:	sigtype = pp->u.r->sigtype;	break;
    case 4:	sigtype = pp->u.s->sigtype;	break;
    }
    return sigtype;
}

/*==============================================================*/
static const char * rpmhkpEscape(const char * keyname)
{
    /* XXX doubles as hex encode string */
    static char ok[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    const char * s;
    char * t, *te;
    size_t nb = 0;

    for (s = keyname; *s; s++)
	nb += (strchr(ok, *s) ? 1 : 4);

    te = t = xmalloc(nb + 1);
    for (s = keyname; *s; s++) {
	if (strchr(ok, *s) == NULL) {
	    *te++ = '%';
	    *te++ = '%';
	    *te++ = ok[(*s >> 4) & 0x0f];
	    *te++ = ok[(*s     ) & 0x0f];
	} else
	    *te++ = *s;
    }
    *te = '\0';
    return t;
}

rpmhkp rpmhkpLookup(const char * keyname)
{
#if 1
    static char _uri[] = "http://keys.rpm5.org:11371";
#else
    static char _uri[] = "http://falmouth.jbj.org:11371";
#endif
    static char _path[] = "/pks/lookup?op=get&search=";
    const char * kn = rpmhkpEscape(keyname);
    const char * fn = rpmExpand(_uri, _path, kn, NULL);
    rpmhkp hkp = NULL;
    pgpArmor pa;
    int rc = 1;	/* assume failure */

SPEW((stderr, "*** %s: %s\n", __FUNCTION__, fn));
    hkp = rpmhkpNew(NULL, 0);

    pa = pgpReadPkts(fn, &hkp->pkt, &hkp->pktlen);
    if (pa == PGPARMOR_ERROR || pa == PGPARMOR_NONE
     || hkp->pkt == NULL || hkp->pktlen == 0)
	goto exit;

    rc = pgpGrabPkts(hkp->pkt, hkp->pktlen, &hkp->pkts, &hkp->npkts);

    /* XXX make sure this works with lazy web-of-trust loading. */
    /* XXX sloppy queries have multiple PUB's */
    if (!rc)
	(void) pgpPubkeyFingerprint(hkp->pkt, hkp->pktlen, hkp->keyid);

exit:
    if (rc && hkp)
	hkp = rpmhkpFree(hkp);
    fn = _free(fn);
    kn = _free(kn);
    return hkp;
}

static int rpmhkpLoadKey(rpmhkp hkp, pgpDig dig,
		int keyx, pgpPubkeyAlgo pubkey_algo)
{
    pgpDigParams pubp = pgpGetPubkey(dig);
    pgpPkt pp = alloca(sizeof(*pp));
    /* XXX "best effort" use primary key if keyx is bogus */
    int ix = (keyx >= 0 && keyx < hkp->npkts) ? keyx : 0;
    size_t pleft = hkp->pktlen - (hkp->pkts[ix] - hkp->pkt);
    int len = pgpPktLen(hkp->pkts[ix], pleft, pp);
    const rpmuint8_t * p;
    int rc = 0;	/* assume success */
len = len;

    if (pp->u.h[0] == 3) {
	pubp->version = pp->u.j->version;
	memcpy(pubp->time, pp->u.j->time, sizeof(pubp->time));
	pubp->pubkey_algo = pp->u.j->pubkey_algo;
	p = ((rpmuint8_t *)pp->u.j) + sizeof(*pp->u.j);
	p = pgpPrtPubkeyParams(dig, pp, pubkey_algo, p);
    } else
    if (pp->u.h[0] == 4) {
	pubp->version = pp->u.k->version;
	memcpy(pubp->time, pp->u.k->time, sizeof(pubp->time));
	pubp->pubkey_algo = pp->u.k->pubkey_algo;
	p = ((rpmuint8_t *)pp->u.k) + sizeof(*pp->u.k);
	p = pgpPrtPubkeyParams(dig, pp, pubkey_algo, p);
    } else
	rc = -1;

    return rc;
}

static int rpmhkpFindKey(rpmhkp hkp, pgpDig dig,
		const rpmuint8_t * signid,
		pgpPubkeyAlgo pubkey_algo)
{
    pgpDigParams sigp = pgpGetSignature(dig);
    int keyx = -1;	/* assume notfound (in this cert) */
int xx;

    if (hkp->pubx >= 0 && hkp->pubx < hkp->npkts
     && !memcmp(hkp->keyid, signid, sizeof(hkp->keyid))) {
	if (!rpmhkpLoadKey(hkp, dig, hkp->pubx, sigp->pubkey_algo))
	    keyx = hkp->pubx;
	goto exit;
    }

    if (hkp->subx >= 0 && hkp->subx < hkp->npkts
     && !memcmp(hkp->subid, signid, sizeof(hkp->subid))) {
	if (!rpmhkpLoadKey(hkp, dig, hkp->subx, sigp->pubkey_algo))
	    keyx = hkp->subx;
	goto exit;
    }

    if (hkp->awol && rpmbfChk(hkp->awol, signid, 8)) {
	keyx = -2;
	SUM.AWOL.good++;
	goto exit;
    }

    {	char * keyname = rpmExpand("0x", pgpHexStr(signid, 8), NULL);
	rpmhkp ohkp = rpmhkpLookup(keyname);

	if (ohkp == NULL) {
	    xx = rpmbfAdd(hkp->awol, signid, 8);
DESPEW((stderr, "\tAWOL\n"));
	    SUM.AWOL.bad++;
	    keyx = -2;
	    goto exit;
	}
	if (rpmhkpLoadKey(ohkp, dig, 0, sigp->pubkey_algo))
	    keyx = -2;		/* XXX skip V2 certs */
	ohkp = rpmhkpFree(ohkp);
	keyname = _free(keyname);
    }

exit:
    return keyx;
}

static int rpmhkpLoadSignature(rpmhkp hkp, pgpDig dig, pgpPkt pp)
{
    pgpDigParams sigp = pgpGetSignature(dig);
    const rpmuint8_t * p = NULL;

    sigp->version = pp->u.h[0];

    if (pp->u.h[0] == 3) {
	sigp->version = pp->u.r->version;
	sigp->pubkey_algo = pp->u.r->pubkey_algo;
	sigp->hash_algo = pp->u.r->hash_algo;
	sigp->sigtype = pp->u.r->sigtype;
	memcpy(sigp->time, pp->u.r->time, sizeof(sigp->time));
	memset(sigp->expire, 0, sizeof(sigp->expire));
	sigp->hash = (const rpmuint8_t *)pp->u.r;
	sigp->hashlen = (size_t)pp->u.r->hashlen;
	memcpy(sigp->signid, pp->u.r->signid, sizeof(sigp->signid));
	memcpy(sigp->signhash16, pp->u.r->signhash16, sizeof(sigp->signhash16));

/* XXX set pointer to signature parameters. */
p = ((rpmuint8_t *)pp->u.r) + sizeof(*pp->u.r);

    }

    if (pp->u.h[0] == 4) {
	const rpmuint8_t * phash;
	size_t nhash;
	const rpmuint8_t * punhash;
	size_t nunhash;
	size_t tlen;

	sigp->pubkey_algo = pp->u.s->pubkey_algo;
	sigp->hash_algo = pp->u.s->hash_algo;
	sigp->sigtype = pp->u.s->sigtype;

	phash = ((const rpmuint8_t *)pp->u.s) + sizeof(*pp->u.s);
	nhash = pgpGrab(pp->u.s->hashlen, sizeof(pp->u.s->hashlen));
	sigp->hash = (const rpmuint8_t *)pp->u.s;
	sigp->hashlen = sizeof(*pp->u.s) + nhash;

	nunhash = pgpGrab(phash+nhash, 2);
	punhash = phash + nhash + 2;
	memcpy(sigp->signhash16, punhash+nunhash, sizeof(sigp->signhash16));

#ifdef	DYING
tlen = 0;
p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_SIG_TARGET, &tlen);
if (p) fprintf(stderr, "*** SIG_TARGET %s\n", pgpHexStr(p, tlen));
tlen = 0;
p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_EMBEDDED_SIG, &tlen);
if (p) fprintf(stderr, "*** EMBEDDED_SIG %s\n", pgpHexStr(p, tlen));
tlen = 0;
p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_REVOKE_KEY, &tlen);
if (p) fprintf(stderr, "*** REVOKE_KEY %02X %02X %s\n", p[0], p[1], pgpHexStr(p+2, tlen-2));
tlen = 0;
p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_REVOKE_REASON, &tlen);
if (p) fprintf(stderr, "*** REVOKE_REASON %02X %s\n", *p, p+1);
#endif

	tlen = 0;
	p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_SIG_CREATE_TIME, &tlen);
	if (p)	memcpy(sigp->time, p, sizeof(sigp->time));
	else	memset(sigp->time, 0, sizeof(sigp->time));

	tlen = 0;
	p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_SIG_EXPIRE_TIME, &tlen);
	if (p)	memcpy(sigp->expire, p, sizeof(sigp->expire));
	else	memset(sigp->expire, 0, sizeof(sigp->expire));

	/* XXX only on self-signature. */
	tlen = 0;
	p = pgpGrabSubTagVal(phash, nhash, PGPSUBTYPE_KEY_EXPIRE_TIME, &tlen);
	if (p)	memcpy(sigp->keyexpire, p, sizeof(sigp->keyexpire));
	else	memset(sigp->keyexpire, 0, sizeof(sigp->keyexpire));

	tlen = 0;
	p = pgpGrabSubTagVal(punhash, nunhash, PGPSUBTYPE_ISSUER_KEYID, &tlen);

/*
 * Certain (some @pgp.com) signatures are missing signatire keyid packet.
 * 0CD70535
 * 2F51309F
 * AE7696CA
 * BC98E63D
 * C0604B2D
 * C5595FE6
 */
if (p == NULL || tlen != 8) {
#if 0
return -1;
#else
p = hkp->keyid;
#endif
}

	if (p)	memcpy(sigp->signid, p, sizeof(sigp->signid));
	else	memset(sigp->signid, 0, sizeof(sigp->signid));

/* XXX set pointer to signature parameters. */
p = punhash + nunhash + 2;

    }

    /* XXX Load signature paramaters. */
    pgpPrtSigParams(dig, pp, sigp->pubkey_algo, sigp->sigtype, p);

    return 0;
}

static int rpmhkpUpdate(/*@null@*/DIGEST_CTX ctx, const void * data, size_t len)
{
    int rc = rpmDigestUpdate(ctx, data, len);
SPEW((stderr, "*** Update(%5d): %s\n", len, pgpHexStr(data, len)));
    return rc;
}

static DIGEST_CTX rpmhkpHashKey(rpmhkp hkp, int ix, pgpHashAlgo dalgo)
{
    DIGEST_CTX ctx = rpmDigestInit(dalgo, RPMDIGEST_NONE);
pgpPkt pp = alloca(sizeof(*pp));
assert(ix >= 0 && ix < hkp->npkts);
#ifdef NOTYET
assert(*hkp->pkts[ix] == 0x99);
#else
switch (*hkp->pkts[ix]) {
default: fprintf(stderr, "*** %s: %02X\n", __FUNCTION__, *hkp->pkts[ix]);
case 0x99: case 0x98: case 0xb9: break;
}
#endif
(void) pgpPktLen(hkp->pkts[ix], hkp->pktlen, pp);

    hkp->goop[0] = 0x99;	/* XXX correct for revocation? */
    hkp->goop[1] = (pp->hlen >>  8) & 0xff;
    hkp->goop[2] = (pp->hlen      ) & 0xff;
    rpmhkpUpdate(ctx, hkp->goop, 3);
    rpmhkpUpdate(ctx, pp->u.h, pp->hlen);
    return ctx;
}

static DIGEST_CTX rpmhkpHashUid(rpmhkp hkp, int ix, pgpHashAlgo dalgo)
{
    DIGEST_CTX ctx = rpmhkpHashKey(hkp, hkp->pubx, dalgo);
pgpPkt pp = alloca(sizeof(*pp));
assert(ix > 0 && ix < hkp->npkts);
#ifdef NOTYET
assert(*hkp->pkts[ix] == 0xb4 || *hkp->pkts[ix] == 0xd1);
#else
switch (*hkp->pkts[ix]) {
default: fprintf(stderr, "*** %s: %02X\n", __FUNCTION__, *hkp->pkts[ix]);
case 0xb4: break;
}
#endif
(void) pgpPktLen(hkp->pkts[ix], hkp->pktlen, pp);

    hkp->goop[0] = *hkp->pkts[ix];
    hkp->goop[1] = (pp->hlen >> 24) & 0xff;
    hkp->goop[2] = (pp->hlen >> 16) & 0xff;
    hkp->goop[3] = (pp->hlen >>  8) & 0xff;
    hkp->goop[4] = (pp->hlen      ) & 0xff;
    rpmhkpUpdate(ctx, hkp->goop, 5);
    rpmhkpUpdate(ctx, pp->u.h, pp->hlen);
    return ctx;
}

static DIGEST_CTX rpmhkpHashSubkey(rpmhkp hkp, int ix, pgpHashAlgo dalgo)
{
    DIGEST_CTX ctx = rpmhkpHashKey(hkp, hkp->pubx, dalgo);
pgpPkt pp = alloca(sizeof(*pp));
assert(ix > 0 && ix < hkp->npkts);
#ifdef NOTYET
assert(*hkp->pkts[ix] == 0xb9);
#else
switch (*hkp->pkts[ix]) {
default: fprintf(stderr, "*** %s: %02X\n", __FUNCTION__, *hkp->pkts[ix]);
case 0xb9: case 0xb8: break;
}
#endif
(void) pgpPktLen(hkp->pkts[ix], hkp->pktlen, pp);

    hkp->goop[0] = 0x99;
    hkp->goop[1] = (pp->hlen >>  8) & 0xff;
    hkp->goop[2] = (pp->hlen      ) & 0xff;
    rpmhkpUpdate(ctx, hkp->goop, 3);
    rpmhkpUpdate(ctx, pp->u.h, pp->hlen);
    return ctx;
}

static DIGEST_CTX rpmhkpHash(rpmhkp hkp, int keyx,
		pgpSigType sigtype, pgpHashAlgo dalgo)
{
    DIGEST_CTX ctx = NULL;

    switch (sigtype) {
    case PGPSIGTYPE_BINARY:
    case PGPSIGTYPE_TEXT:
    case PGPSIGTYPE_STANDALONE:
    default:
	break;
    case PGPSIGTYPE_GENERIC_CERT:
    case PGPSIGTYPE_PERSONA_CERT:
    case PGPSIGTYPE_CASUAL_CERT:
    case PGPSIGTYPE_POSITIVE_CERT:
	if (hkp->pubx >= 0 && hkp->uidx >= 0)
	    ctx = rpmhkpHashUid(hkp, hkp->uidx, dalgo);
	break;
    case PGPSIGTYPE_SUBKEY_BINDING:
	if (hkp->pubx >= 0 && hkp->subx >= 0)
	    ctx = rpmhkpHashSubkey(hkp, hkp->subx, dalgo);
	break;
    case PGPSIGTYPE_KEY_BINDING:
	if (hkp->pubx >= 0)
	    ctx = rpmhkpHashSubkey(hkp, hkp->pubx, dalgo);
	break;
    case PGPSIGTYPE_SIGNED_KEY:
	/* XXX search for signid amongst the packets? */
	break;
    case PGPSIGTYPE_KEY_REVOKE:
	/* XXX only primary key */
	/* XXX authorized revocation key too. */
	if (hkp->pubx >= 0)
	    ctx = rpmhkpHashKey(hkp, hkp->pubx, dalgo);
	break;
    case PGPSIGTYPE_SUBKEY_REVOKE:
	/* XXX only primary key */
	/* XXX authorized revocation key too. */
	if (hkp->pubx >= 0 && hkp->subx >= 0)
	    ctx = rpmhkpHashKey(hkp, hkp->subx, dalgo);
	break;
    case PGPSIGTYPE_CERT_REVOKE:
    case PGPSIGTYPE_TIMESTAMP:
    case PGPSIGTYPE_CONFIRM:
	break;
    }
    return ctx;
}

#ifdef	DYING
static void dumpDigParams(const char * msg, pgpDigParams sigp)
{
fprintf(stderr, "%s: %p\n", msg, sigp);
fprintf(stderr, "\t     userid: %s\n", sigp->userid);
fprintf(stderr, "\t       hash: %p[%u]\n", sigp->hash, sigp->hashlen);
fprintf(stderr, "\t        tag: %02X\n", sigp->tag);
fprintf(stderr, "\t    version: %02X\n", sigp->version);
fprintf(stderr, "\t       time: %08X\n", pgpGrab(sigp->time, sizeof(sigp->time)));
fprintf(stderr, "\tpubkey_algo: %02X\n", sigp->pubkey_algo);
fprintf(stderr, "\t  hash_algo: %02X\n", sigp->hash_algo);
fprintf(stderr, "\t    sigtype: %02X\n", sigp->sigtype);
fprintf(stderr, "\t signhash16: %04X\n", pgpGrab(sigp->signhash16, sizeof(sigp->signhash16)));
fprintf(stderr, "\t     signid: %08X %08X\n", pgpGrab(sigp->signid+4, 4), pgpGrab(sigp->signid, 4));
fprintf(stderr, "\t      saved: %02X\n", sigp->saved);
}

static void dumpDig(const char * msg, pgpDig dig)
{
fprintf(stderr, "%s: dig %p\n", msg, dig);

fprintf(stderr, "\t    sigtag: 0x%08x\n", dig->sigtag);
fprintf(stderr, "\t   sigtype: 0x%08x\n", dig->sigtype);
fprintf(stderr, "\t       sig: %p[%u]\n", dig->sig, dig->siglen);
fprintf(stderr, "\t   vsflags: 0x%08x\n", dig->vsflags);
fprintf(stderr, "\tfindPubkey: %p\n", dig->findPubkey);
fprintf(stderr, "\t       _ts: %p\n", dig->_ts);
fprintf(stderr, "\t     ppkts: %p[%u]\n", dig->ppkts, dig->npkts);
fprintf(stderr, "\t    nbytes: 0x%08x\n", dig->nbytes);

fprintf(stderr, "\t   sha1ctx: %p\n", dig->sha1ctx);
fprintf(stderr, "\thdrsha1ctx: %p\n", dig->hdrsha1ctx);
fprintf(stderr, "\t      sha1: %p[%u]\n", dig->sha1, dig->sha1len);

fprintf(stderr, "\t    md5ctx: %p\n", dig->md5ctx);
fprintf(stderr, "\t    hdrctx: %p\n", dig->hdrctx);
fprintf(stderr, "\t       md5: %p[%u]\n", dig->md5, dig->md5len);
fprintf(stderr, "\t      impl: %p\n", dig->impl);

dumpDigParams("PUB", pgpGetPubkey(dig));
dumpDigParams("SIG", pgpGetSignature(dig));
}
#endif

static int rpmhkpVerifyHash(rpmhkp hkp, pgpDig dig, DIGEST_CTX ctx)
{
    pgpDigParams sigp = pgpGetSignature(dig);
    const char * dname = xstrdup(rpmDigestName(ctx));
    rpmuint8_t * digest = NULL;
    size_t digestlen = 0;
    int rc = rpmDigestFinal(ctx, &digest, &digestlen, 0);

    rc = memcmp(sigp->signhash16, digest, sizeof(sigp->signhash16));

if (rc)
SPEW((stderr, "\t%s\t%s\n", dname, pgpHexStr(digest, digestlen)));
SPEW((stderr, "%s\t%s\n", (!rc ? "\tGOOD" : "------> BAD"), pgpHexStr(sigp->signhash16, sizeof(sigp->signhash16))));

    if (rc)
	SUM.HASH.bad++;
    else
	SUM.HASH.good++;

    digest = _free(digest);
    digestlen = 0;
    dname = _free(dname);

    return rc;
}

static int rpmhkpVerifySignature(rpmhkp hkp, pgpDig dig, DIGEST_CTX ctx)
{
    pgpDigParams sigp = pgpGetSignature(dig);
    int rc = 0;		/* XXX assume failure */

    switch (sigp->pubkey_algo) {

    case PGPPUBKEYALGO_DSA:
	if (pgpImplSetDSA(ctx, dig, sigp)) {
DESPEW((stderr, "------> BAD\t%s\n", pgpHexStr(sigp->signhash16, 2)));
	    SUM.HASH.bad++;
	    goto exit;
	}
	if (!pgpImplVerifyDSA(dig)) {
DESPEW((stderr, "------> BAD\tV%u %s-%s\n",
		sigp->version,
		_pgpPubkeyAlgo2Name(sigp->pubkey_algo),
		_pgpHashAlgo2Name(sigp->hash_algo)));
	    SUM.DSA.bad++;
	} else {
DESPEW((stderr, "\tGOOD\tV%u %s-%s\n",
		sigp->version,
		_pgpPubkeyAlgo2Name(sigp->pubkey_algo),
		_pgpHashAlgo2Name(sigp->hash_algo)));
	    SUM.DSA.good++;
	    rc = 1;
	}
	break;
    case PGPPUBKEYALGO_RSA:
	if (pgpImplSetRSA(ctx, dig, sigp)) {
DESPEW((stderr, "------> BAD\t%s\n", pgpHexStr(sigp->signhash16, 2)));
	    SUM.HASH.bad++;
	    goto exit;
	}
	if (!pgpImplVerifyRSA(dig)) {
DESPEW((stderr, "------> BAD\tV%u %s-%s\n",
		sigp->version,
		_pgpPubkeyAlgo2Name(sigp->pubkey_algo),
		_pgpHashAlgo2Name(sigp->hash_algo)));
	    SUM.RSA.bad++;
	} else {
DESPEW((stderr, "\tGOOD\tV%u %s-%s\n",
		sigp->version,
		_pgpPubkeyAlgo2Name(sigp->pubkey_algo),
		_pgpHashAlgo2Name(sigp->hash_algo)));
	    SUM.RSA.good++;
	    rc = 1;
	}
	break;
    }

exit:
    return rc;
}

static int rpmhkpVerify(rpmhkp hkp, pgpDig dig)
{
    pgpDigParams sigp = pgpGetSignature(dig);
    pgpDigParams pubp = pgpGetPubkey(dig);
    DIGEST_CTX ctx = NULL;
    int keyx;
    int rc = 1;		/* assume failure */

    SUM.sigs++;

    /* XXX Load signature paramaters. */

    /* Ignore expired signatures. */
    {	time_t expire = pgpGrab(sigp->expire, sizeof(sigp->expire));
	if ((expire = pgpGrab(sigp->expire, sizeof(sigp->expire)))
	&&  (expire + (int)pgpGrab(sigp->time, sizeof(sigp->time))) < time(NULL)) {
	    SUM.expired++;
	    goto exit;
	}
    }

    /*
     * Skip PGP Global Directory Verification signatures.
     * http://www.kfwebs.net/articles/article/17/GPG-mass-cleaning-and-the-PGP-Corp.-Global-Directory
     */
    if (pgpGrab(sigp->signid+4, 4) == 0xCA57AD7C
     || (hkp->crl && rpmbfChk(hkp->crl, sigp->signid, sizeof(sigp->signid)))) {
	SUM.filtered++;
	goto exit;
    }

    fprintf(stderr, "  SIG: %08X %08X V%u %s-%s %s\n",
		pgpGrab(sigp->signid, 4), pgpGrab(sigp->signid+4, 4),
		sigp->version,
		_pgpPubkeyAlgo2Name(sigp->pubkey_algo),
		_pgpHashAlgo2Name(sigp->hash_algo),
		_pgpSigType2Name(sigp->sigtype)
    );

    /* XXX Load pubkey paramaters. */
    keyx = rpmhkpFindKey(hkp, dig, sigp->signid, sigp->pubkey_algo);
    if (keyx == -2)	/* XXX AWOL */
	goto exit;

    /* Ignore expired keys (self-certs only). */
    {	time_t expire = pgpGrab(sigp->keyexpire, sizeof(sigp->keyexpire));
	if ((expire = pgpGrab(sigp->keyexpire, sizeof(sigp->keyexpire)))
	&&  (expire + (int)pgpGrab(pubp->time, sizeof(pubp->time))) < time(NULL)) {
	    SUM.keyexpired++;
	    goto exit;
	}
    }

    ctx = rpmhkpHash(hkp, keyx, sigp->sigtype, sigp->hash_algo);

    if (ctx) {

	/* XXX something fishy here with V3 signatures. */
	if (sigp->hash)
	    rpmhkpUpdate(ctx, sigp->hash, sigp->hashlen);

	if (sigp->version == 4) {
	    hkp->goop[0] = sigp->version;
	    hkp->goop[1] = (rpmuint8_t)0xff;
	    hkp->goop[2] = (sigp->hashlen >> 24) & 0xff;
	    hkp->goop[3] = (sigp->hashlen >> 16) & 0xff;
	    hkp->goop[4] = (sigp->hashlen >>  8) & 0xff;
	    hkp->goop[5] = (sigp->hashlen      ) & 0xff;
	    rpmhkpUpdate(ctx, hkp->goop, 6);
	}

	switch (sigp->pubkey_algo) {
	/* XXX handle only RSA/DSA? */
	case PGPPUBKEYALGO_DSA:
	case PGPPUBKEYALGO_RSA:
	    /* XXX only V4 certs for now. */
	    if (sigp->version == 4) {
		rc = rpmhkpVerifySignature(hkp, dig, ctx);
		break;
	    }
	    /*@fallthrough@*/
	default:
	    rc = rpmhkpVerifyHash(hkp, dig, ctx);
	    break;
	}
	ctx = NULL;
    }

exit:

    return rc;	/* XXX 1 on success */
}

rpmRC rpmhkpValidate(rpmhkp hkp, const char * keyname)
{
    pgpPkt pp = alloca(sizeof(*pp));
    size_t pleft;
    rpmRC rc = RPMRC_FAIL;		/* assume failure */
    int xx;
    int i;

    /* Do a lazy lookup before validating. */
    if (hkp == NULL && keyname && *keyname) {
	if ((hkp = rpmhkpLookup(keyname)) == NULL) {
	    rc = RPMRC_NOTFOUND;
	    return rc;
	}
    } else
    if ((hkp = rpmhkpLink(hkp)) == NULL)
	return rc;

    SUM.certs++;
assert(hkp->pkts);

    pleft = hkp->pktlen;
    for (i = 0; i < hkp->npkts; i++) {
	xx = pgpPktLen(hkp->pkts[i], pleft, pp);
assert(xx > 0);
	pleft -= pp->pktlen;
SPEW((stderr, "%6d %p[%3u] %02X %s\n", i, hkp->pkts[i], (unsigned)pp->pktlen, *hkp->pkts[i], pgpValStr(pgpTagTbl, (rpmuint8_t)pp->tag)));
SPEW((stderr, "\t%s\n", pgpHexStr(hkp->pkts[i], pp->pktlen)));

	switch (pp->tag) {
	default:
	    break;
	case PGPTAG_PUBLIC_KEY:
	    hkp->pubx = i;
{
/* XXX sloppy hkp:// queries can/will have multiple PUB's */
xx = pgpPubkeyFingerprint(hkp->pkts[i], pp->pktlen, hkp->keyid);
fprintf(stderr, "  PUB: %08X %08X", pgpGrab(hkp->keyid, 4), pgpGrab(hkp->keyid+4, 4));
if (pp->u.h[0] == 3) {
fprintf(stderr, " V%u %s", pp->u.j->version, _pgpPubkeyAlgo2Name(pp->u.j->pubkey_algo));
    if (pp->u.j->valid[0] || pp->u.j->valid[1]) {
	rpmuint32_t days = pgpGrab(pp->u.j->valid, sizeof(pp->u.j->valid));
	time_t expired = pgpGrab(pp->u.j->time, 4) + (24 * 60 * 60 * days);
	if (expired < time(NULL))
	    fprintf(stderr, " EXPIRED");
    }
}
if (pp->u.h[0] == 4) {
fprintf(stderr, " V%u %s", pp->u.k->version, _pgpPubkeyAlgo2Name(pp->u.k->pubkey_algo));
}
fprintf(stderr, "\n");
}

	    break;
	case PGPTAG_USER_ID:
	    hkp->uidx = i;
	    break;
	case PGPTAG_PUBLIC_SUBKEY:
	    hkp->subx = i;
{
xx = pgpPubkeyFingerprint(hkp->pkts[i], pp->pktlen, hkp->subid);
fprintf(stderr, "  SUB: %08X %08X", pgpGrab(hkp->keyid, 4), pgpGrab(hkp->keyid+4, 4));
if (pp->u.h[0] == 3) {
fprintf(stderr, " V%u %s", pp->u.j->version, _pgpPubkeyAlgo2Name(pp->u.j->pubkey_algo));
    if (pp->u.j->valid[0] || pp->u.j->valid[1]) {
	rpmuint32_t days = pgpGrab(pp->u.j->valid, sizeof(pp->u.j->valid));
	time_t expired = pgpGrab(pp->u.j->time, 4) + (24 * 60 * 60 * days);
	if (expired < time(NULL))
	    fprintf(stderr, " EXPIRED");
    }
}
if (pp->u.h[0] == 4) {
fprintf(stderr, " V%u %s", pp->u.k->version, _pgpPubkeyAlgo2Name(pp->u.k->pubkey_algo));
}
fprintf(stderr, "\n");
}

	    break;
	case PGPTAG_SIGNATURE:
	  { pgpDig dig = NULL;
	    pgpDigParams sigp;
	    const rpmuint8_t * p;
	    rpmuint32_t thistime;

	    /* XXX don't fuss V3 signatures for now. */
	    if (pp->u.h[0] != 4) {
SPEW((stderr, "  SIG: V%u\n", pp->u.h[0]));
SPEW((stderr, "\tSKIP(V%u != V3 | V4)\t%s\n", pp->u.h[0], pgpHexStr(pp->u.h, pp->pktlen)));
		SUM.SKIP.bad++;
		break;
	    }

	    dig = pgpDigNew(0);
	    sigp = pgpGetSignature(dig);

	    /* XXX Load signature paramaters. */
	    xx = rpmhkpLoadSignature(hkp, dig, pp);

assert(sigp->sigtype == ppSigType(pp));
	    switch (ppSigType(pp)) {
	    case PGPSIGTYPE_BINARY:
	    case PGPSIGTYPE_TEXT:
	    case PGPSIGTYPE_STANDALONE:
	    default:
		break;
	    case PGPSIGTYPE_GENERIC_CERT:
	    case PGPSIGTYPE_PERSONA_CERT:
	    case PGPSIGTYPE_CASUAL_CERT:
		break;
	    case PGPSIGTYPE_POSITIVE_CERT:
		p = ppSignid(pp);
		/* XXX treat missing issuer as "this" pubkey signature. */
		if (p && memcmp(hkp->keyid, p, sizeof(hkp->keyid)))
		    break;
		hkp->sigx = i;
		if (!rpmhkpVerify(hkp, dig))	/* XXX 1 on success */
		    break;
assert(pgpGrab(sigp->time, 4) == ppSigTime(pp));
		thistime = ppSigTime(pp);
		if (thistime < hkp->tvalid)
		    break;
		hkp->tvalid = thistime;
		hkp->uvalidx = hkp->uidx;
		break;
	    case PGPSIGTYPE_SUBKEY_BINDING:
		if (!rpmhkpVerify(hkp, dig))	/* XXX 1 on success */
		    break;
		SUM.subbound++;
		break;
	    case PGPSIGTYPE_KEY_BINDING:
		if (!rpmhkpVerify(hkp, dig))	/* XXX 1 on success */
		    break;
		SUM.pubbound++;
		break;
	    case PGPSIGTYPE_KEY_REVOKE:
		if (!rpmhkpVerify(hkp, dig))	/* XXX 1 on success */
		    break;
		SUM.pubrevoked++;
		if (hkp->crl)
		    xx = rpmbfAdd(hkp->crl, hkp->keyid, sizeof(hkp->keyid));
		dig = pgpDigFree(dig);
		goto exit;	/* XXX stop validating revoked cert. */
		/*@notreached@*/ break;
	    case PGPSIGTYPE_SUBKEY_REVOKE:
		if (!rpmhkpVerify(hkp, dig))	/* XXX 1 on success */
		    break;
		SUM.subrevoked++;
#ifdef	NOTYET	/* XXX subid not loaded correctly yet. */
		if (hkp->crl)
		    xx = rpmbfAdd(hkp->crl, hkp->subid, sizeof(hkp->subid));
#endif
		break;
	    case PGPSIGTYPE_SIGNED_KEY:
	    case PGPSIGTYPE_CERT_REVOKE:
	    case PGPSIGTYPE_TIMESTAMP:
	    case PGPSIGTYPE_CONFIRM:
		break;
	    }

	    dig = pgpDigFree(dig);

	  } break;
	}
    }

exit:
    /* XXX more precise returns. gud enuf */
    if (hkp->tvalid > 0) {
	pgpPktUid * u;
	xx = pgpPktLen(hkp->pkts[hkp->uvalidx], hkp->pktlen, pp);
	u = (pgpPktUid *) pp->u.h;
	fprintf(stderr, "  UID: %.*s\n", pp->hlen, u->userid);
	rc = RPMRC_OK;
    } else
	rc = RPMRC_NOTFOUND;

    hkp = rpmhkpFree(hkp);

    return rc;
}