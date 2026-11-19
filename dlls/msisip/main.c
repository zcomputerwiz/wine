/*
 * Copyright 2008 Juan Lang
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "mssip.h"
#define COBJMACROS
#include "objbase.h"
#include "initguid.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(msisip);

static GUID mySubject = { 0x000c10f1, 0x0000, 0x0000,
 { 0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46 }};

/***********************************************************************
 *              DllRegisterServer (MSISIP.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    static WCHAR msisip[] = L"MSISIP.DLL";
    static WCHAR getSignedDataMsg[] = L"MsiSIPGetSignedDataMsg";
    static WCHAR putSignedDataMsg[] = L"MsiSIPPutSignedDataMsg";
    static WCHAR createIndirectData[] = L"MsiSIPCreateIndirectData";
    static WCHAR verifyIndirectData[] = L"MsiSIPVerifyIndirectData";
    static WCHAR removeSignedDataMsg[] = L"MsiSIPRemoveSignedDataMsg";
    static WCHAR isMyTypeOfFile[] = L"MsiSIPIsMyTypeOfFile";

    SIP_ADD_NEWPROVIDER prov;

    memset(&prov, 0, sizeof(prov));
    prov.cbStruct = sizeof(prov);
    prov.pwszDLLFileName = msisip;
    prov.pgSubject = &mySubject;
    prov.pwszGetFuncName = getSignedDataMsg;
    prov.pwszPutFuncName = putSignedDataMsg;
    prov.pwszCreateFuncName = createIndirectData;
    prov.pwszVerifyFuncName = verifyIndirectData;
    prov.pwszRemoveFuncName = removeSignedDataMsg;
    prov.pwszIsFunctionNameFmt2 = isMyTypeOfFile;
    prov.pwszGetCapFuncName = NULL;
    return CryptSIPAddProvider(&prov) ? S_OK : S_FALSE;
}

/***********************************************************************
 *              DllUnregisterServer (MSISIP.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    CryptSIPRemoveProvider(&mySubject);
    return S_OK;
}

/***********************************************************************
 *              MsiSIPGetSignedDataMsg  (MSISIP.@)
 */
BOOL WINAPI MsiSIPGetSignedDataMsg(SIP_SUBJECTINFO *pSubjectInfo,
 DWORD *pdwEncodingType, DWORD dwIndex, DWORD *pcbSignedDataMsg,
 BYTE *pbSignedDataMsg)
{
    BOOL ret = FALSE;
    IStorage *stg = NULL;
    HRESULT r;
    IStream *stm = NULL;
    BYTE hdr[2], len[sizeof(DWORD)];
    DWORD count, lenBytes, dataBytes;

    TRACE("(%p %p %ld %p %p)\n", pSubjectInfo, pdwEncodingType, dwIndex,
          pcbSignedDataMsg, pbSignedDataMsg);

    r = StgOpenStorage(pSubjectInfo->pwsFileName, NULL,
     STGM_DIRECT|STGM_READ|STGM_SHARE_DENY_WRITE, NULL, 0, &stg);
    if (FAILED(r))
    {
        TRACE("couldn't open %s\n", debugstr_w(pSubjectInfo->pwsFileName));
        goto end;
    }

    r = IStorage_OpenStream(stg, L"\5DigitalSignature", 0,
     STGM_READ|STGM_SHARE_EXCLUSIVE, 0, &stm);
    if (FAILED(r))
    {
        TRACE("couldn't find digital signature stream\n");
        goto freestorage;
    }

    r = IStream_Read(stm, hdr, sizeof(hdr), &count);
    if (FAILED(r) || count != sizeof(hdr))
        goto freestream;
    if (hdr[0] != 0x30)
    {
        WARN("unexpected data in digital sig: 0x%02x%02x\n", hdr[0], hdr[1]);
        goto freestream;
    }

    /* Read the asn.1 length from the stream.  Only supports definite-length
     * values, which DER-encoded signatures should be.
     */
    if (hdr[1] == 0x80)
    {
        WARN("indefinite-length encoding not supported!\n");
        goto freestream;
    }
    else if (hdr[1] & 0x80)
    {
        DWORD temp;
        LPBYTE ptr;

        lenBytes = hdr[1] & 0x7f;
        if (lenBytes > sizeof(DWORD))
        {
            WARN("asn.1 length too long (%ld)\n", lenBytes);
            goto freestream;
        }
        r = IStream_Read(stm, len, lenBytes, &count);
        if (FAILED(r) || count != lenBytes)
            goto freestream;
        dataBytes = 0;
        temp = lenBytes;
        ptr = len;
        while (temp--)
        {
            dataBytes <<= 8;
            dataBytes |= *ptr++;
        }
    }
    else
    {
        lenBytes = 0;
        dataBytes = hdr[1];
    }

    if (!pbSignedDataMsg)
    {
        *pcbSignedDataMsg = 2 + lenBytes + dataBytes;
        ret = TRUE;
    }
    else if (*pcbSignedDataMsg < 2 + lenBytes + dataBytes)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        *pcbSignedDataMsg = 2 + lenBytes + dataBytes;
    }
    else
    {
        LPBYTE ptr = pbSignedDataMsg;

        memcpy(ptr, hdr, sizeof(hdr));
        ptr += sizeof(hdr);
        if (lenBytes)
        {
            memcpy(ptr, len, lenBytes);
            ptr += lenBytes;
        }
        r = IStream_Read(stm, ptr, dataBytes, &count);
        if (SUCCEEDED(r) && count == dataBytes)
        {
            *pdwEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
            *pcbSignedDataMsg = 2 + lenBytes + dataBytes;
            ret = TRUE;
        }
    }

freestream:
    IStream_Release(stm);
freestorage:
    IStorage_Release(stg);
end:

    TRACE("returning %d\n", ret);
    return ret;
}

/* The digest of a package, as signed by Authenticode: a walk over the compound file itself (not through
 * IStorage, which hides the directory fields the digest covers).  Per storage, the children in the order of
 * cmp_ent, the contents of every stream (not \5DigitalSignature at the root), then the CLSID of the storage.
 * When the package has a
 * \5MsiDigitalSignatureEx stream, its contents (a digest of the names, sizes, times and flags) are part of the
 * walk like any stream, and are checked against that digest of the directory. */

#define CFB_FREESECT   0xffffffff
#define CFB_ENDOFCHAIN 0xfffffffe
#define CFB_MAXDEPTH   64

struct cfb
{
    HANDLE file;
    DWORD ssize, msize, cutoff;
    BYTE *fat, *minifat;     /* raw little-endian entries */
    DWORD nfat, nminifat;
    BYTE *dir, *mini;
    DWORD ndir, nmini;
    DWORD budget;           /* directory entries still allowed to be visited */
    HCRYPTHASH hash;
};

static DWORD rd32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }

static BOOL cfb_read(struct cfb *c, ULONGLONG pos, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got;

    li.QuadPart = pos;
    return SetFilePointerEx(c->file, li, NULL, FILE_BEGIN) && ReadFile(c->file, buf, len, &got, NULL) && got == len;
}

static BOOL cfb_sector(struct cfb *c, DWORD sec, void *buf)
{
    return cfb_read(c, ((ULONGLONG)sec + 1) * c->ssize, buf, c->ssize);
}

/* the whole chain of sectors starting at sec, up to size bytes (all of it if size is 0xffffffff) */
static BYTE *cfb_chain(struct cfb *c, const BYTE *fat, DWORD nfat, DWORD sec, DWORD ssize, DWORD size, BOOL mini,
                       DWORD *out_size)
{
    DWORD i, n = 0, cap = 0;
    BYTE *buf = NULL, *p;

    for (i = 0; sec != CFB_ENDOFCHAIN && (size == ~0u || n < size); i++)
    {
        if (sec >= nfat || i > nfat) goto fail;
        if (n + ssize > cap)
        {
            cap = cap ? cap * 2 : 4 * ssize;
            if (!(p = realloc(buf, cap))) goto fail;
            buf = p;
        }
        if (mini)
        {
            if ((ULONGLONG)(sec + 1) * ssize > c->nmini) goto fail;
            memcpy(buf + n, c->mini + (ULONGLONG)sec * ssize, ssize);
        }
        else if (!cfb_sector(c, sec, buf + n)) goto fail;
        n += ssize;
        sec = rd32(fat + sec * 4);
    }
    if (size != ~0u && n < size) goto fail;
    *out_size = size == ~0u ? n : size;
    return buf;
fail:
    free(buf);
    return NULL;
}

static void cfb_free(struct cfb *c)
{
    free(c->fat);
    free(c->minifat);
    free(c->dir);
    free(c->mini);
}

static BOOL cfb_open(struct cfb *c)
{
    BYTE hdr[512], *buf, *root;
    DWORD i, nsec, n = 0, difat_sec, ndifat, *fatsecs = NULL, size;
    LARGE_INTEGER fsize;

    c->fat = c->minifat = NULL;
    c->dir = c->mini = NULL;
    if (!GetFileSizeEx(c->file, &fsize) || !cfb_read(c, 0, hdr, sizeof(hdr))) return FALSE;
    if (memcmp(hdr, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8)) return FALSE;
    if (hdr[0x1e] != 9 && hdr[0x1e] != 12) return FALSE;
    c->ssize = 1u << hdr[0x1e];
    c->msize = 1u << hdr[0x20];
    c->cutoff = rd32(hdr + 0x38);
    if (c->msize != 64 || fsize.QuadPart / c->ssize > 0x10000000) return FALSE;
    nsec = fsize.QuadPart / c->ssize;
    c->nfat = nsec * (c->ssize / 4);

    /* the FAT: the sectors listed in the header and in the DIFAT chain */
    if (!(fatsecs = malloc((109 + (nsec + 1) * (c->ssize / 4)) * sizeof(DWORD))) ||
        !(c->fat = malloc(nsec * c->ssize)) || !(buf = malloc(c->ssize))) goto fail;
    for (i = 0; i < 109; i++)
        if (rd32(hdr + 0x4c + i * 4) != CFB_FREESECT) fatsecs[n++] = rd32(hdr + 0x4c + i * 4);
    difat_sec = rd32(hdr + 0x44);
    ndifat = rd32(hdr + 0x48);
    for (i = 0; i < ndifat && difat_sec < nsec; i++)
    {
        DWORD j;
        if (!cfb_sector(c, difat_sec, buf)) { free(buf); goto fail; }
        for (j = 0; j < c->ssize / 4 - 1; j++)
            if (rd32(buf + j * 4) != CFB_FREESECT && n < nsec) fatsecs[n++] = rd32(buf + j * 4);
        difat_sec = rd32(buf + c->ssize - 4);
    }
    free(buf);
    c->nfat = 0;
    for (i = 0; i < n; i++)
    {
        DWORD j;
        if (fatsecs[i] >= nsec || !(buf = malloc(c->ssize))) goto fail;
        if (!cfb_sector(c, fatsecs[i], buf)) { free(buf); goto fail; }
        for (j = 0; j < c->ssize / 4 && c->nfat < nsec; j++, c->nfat++) memcpy(c->fat + c->nfat * 4, buf + j * 4, 4);
        free(buf);
    }
    free(fatsecs);
    fatsecs = NULL;

    if (!(c->dir = cfb_chain(c, c->fat, c->nfat, rd32(hdr + 0x30), c->ssize, ~0u, FALSE, &size))) goto fail;
    c->ndir = size / 128;
    if (!c->ndir || (c->dir[0x42] != 5)) goto fail;
    c->budget = c->ndir;

    if (rd32(hdr + 0x40) && !(c->minifat = cfb_chain(c, c->fat, c->nfat, rd32(hdr + 0x3c), c->ssize, ~0u, FALSE, &size)))
        goto fail;
    c->nminifat = c->minifat ? size / 4 : 0;
    root = c->dir;
    if (rd32(root + 0x78) && rd32(root + 0x74) != CFB_ENDOFCHAIN &&
        !(c->mini = cfb_chain(c, c->fat, c->nfat, rd32(root + 0x74), c->ssize, rd32(root + 0x78), FALSE, &c->nmini)))
        goto fail;
    return TRUE;
fail:
    free(fatsecs);
    cfb_free(c);
    return FALSE;
}

static const BYTE *ent(struct cfb *c, DWORD id) { return id < c->ndir ? c->dir + id * 128 : NULL; }

static DWORD name_len(const BYTE *e)   /* bytes, without the terminator */
{
    DWORD n = e[0x40] | e[0x41] << 8;
    return n >= 2 && n <= 64 ? n - 2 : 0;
}

/* the order of the signature: bytes of the shorter name decide; with a common prefix the longer name comes first */
static int cmp_ent(const void *a, const void *b)
{
    const BYTE *x = *(const BYTE * const *)a, *y = *(const BYTE * const *)b;
    DWORD lx = name_len(x), ly = name_len(y);
    int r = memcmp(x, y, min(lx, ly));

    return r ? r : (int)ly - (int)lx;
}

/* the children of a storage, sorted; the tree is walked with a budget so that a damaged file cannot loop */
static BOOL collect(struct cfb *c, DWORD id, const BYTE ***list, DWORD *n, DWORD *cap, DWORD depth)
{
    const BYTE *e;

    if (id == CFB_FREESECT) return TRUE;
    if (!(e = ent(c, id)) || !c->budget-- || depth > 4 * CFB_MAXDEPTH) return FALSE;
    if (*n == *cap)
    {
        const BYTE **p = realloc(*list, (*cap ? *cap * 2 : 16) * sizeof(*p));
        if (!p) return FALSE;
        *list = p;
        *cap = *cap ? *cap * 2 : 16;
    }
    (*list)[(*n)++] = e;
    return collect(c, rd32(e + 0x44), list, n, cap, depth + 1) && collect(c, rd32(e + 0x48), list, n, cap, depth + 1);
}

static BOOL children(struct cfb *c, const BYTE *dir, const BYTE ***list, DWORD *n)
{
    DWORD cap = 0;

    *list = NULL;
    *n = 0;
    if (!collect(c, rd32(dir + 0x4c), list, n, &cap, 0)) { free(*list); return FALSE; }
    qsort(*list, *n, sizeof(**list), cmp_ent);
    return TRUE;
}

static BOOL is_name(const BYTE *e, const WCHAR *name)
{
    DWORD i, len = lstrlenW(name);

    if (name_len(e) != len * 2) return FALSE;
    for (i = 0; i < len; i++) if ((e[2 * i] | e[2 * i + 1] << 8) != name[i]) return FALSE;
    return TRUE;
}

static BOOL feed(struct cfb *c, const void *data, DWORD len)
{
    return !len || CryptHashData(c->hash, data, len, 0);
}

static BOOL hash_stream(struct cfb *c, const BYTE *e)
{
    DWORD size = rd32(e + 0x78), start = rd32(e + 0x74), left, got;
    BYTE *buf, *sec;
    BOOL ret = TRUE;

    if (rd32(e + 0x7c) || !size) return !rd32(e + 0x7c);
    if (size < c->cutoff)
    {
        if (!(buf = cfb_chain(c, c->minifat, c->nminifat, start, c->msize, size, TRUE, &got))) return FALSE;
        ret = feed(c, buf, size);
        free(buf);
        return ret;
    }
    if (!(sec = malloc(c->ssize))) return FALSE;
    for (left = size; ret && left; start = rd32(c->fat + start * 4))
    {
        if (start >= c->nfat || !cfb_sector(c, start, sec)) { ret = FALSE; break; }
        got = min(left, c->ssize);
        ret = feed(c, sec, got);
        left -= got;
    }
    free(sec);
    return ret;
}

static BOOL hash_dir(struct cfb *c, const BYTE *dir, BOOL root, DWORD depth)
{
    const BYTE **list;
    DWORD i, n;
    BOOL ret = TRUE;

    if (depth > CFB_MAXDEPTH || !children(c, dir, &list, &n)) return FALSE;
    for (i = 0; ret && i < n; i++)
    {
        if (list[i][0x42] == 1) ret = hash_dir(c, list[i], FALSE, depth + 1);
        else if (list[i][0x42] != 2) continue;
        else if (root && is_name(list[i], L"\5DigitalSignature")) continue;
        else ret = hash_stream(c, list[i]);
    }
    free(list);
    return ret && feed(c, dir + 0x50, 16);
}

static BOOL prehash_meta(struct cfb *c, const BYTE *e, BOOL root)
{
    BOOL ret = TRUE;

    if (!root) ret = feed(c, e, name_len(e));
    ret = ret && feed(c, e[0x42] == 2 ? e + 0x78 : e + 0x50, e[0x42] == 2 ? 4 : 16);
    ret = ret && feed(c, e + 0x60, 4);
    if (!root) ret = ret && feed(c, e + 0x64, 16);
    return ret;
}

static BOOL prehash_dir(struct cfb *c, const BYTE *dir, BOOL root, DWORD depth)
{
    const BYTE **list;
    DWORD i, n;
    BOOL ret;

    if (depth > CFB_MAXDEPTH || !children(c, dir, &list, &n)) return FALSE;
    ret = prehash_meta(c, dir, root);
    for (i = 0; ret && i < n; i++)
    {
        if (list[i][0x42] == 1) ret = prehash_dir(c, list[i], FALSE, depth + 1);
        else if (list[i][0x42] != 2) continue;
        else if (root && (is_name(list[i], L"\5DigitalSignature") || is_name(list[i], L"\5MsiDigitalSignatureEx")))
            continue;
        else ret = prehash_meta(c, list[i], FALSE);
    }
    free(list);
    return ret;
}

static const BYTE *find_root_stream(struct cfb *c, const WCHAR *name)
{
    const BYTE **list, *found = NULL;
    DWORD i, n;

    if (!children(c, c->dir, &list, &n)) return NULL;
    for (i = 0; i < n; i++) if (list[i][0x42] == 2 && is_name(list[i], name)) found = list[i];
    free(list);
    return found;
}

/* the digest of the package in subject, with the algorithm of alg_oid (sha1 when NULL); out must hold 64 bytes.
 * Fails (ERROR_INVALID_DATA) when the package has a directory digest that does not match its directory. */
static BOOL msi_digest(SIP_SUBJECTINFO *subject, const char *alg_oid, BYTE *out, DWORD *out_len)
{
    struct cfb c = { 0 };
    HCRYPTPROV prov;
    ALG_ID alg = alg_oid ? CertOIDToAlgId(alg_oid) : CALG_SHA1;
    const BYTE *ex;
    BOOL ret = FALSE, own = FALSE;
    DWORD size;

    c.file = subject->hFile;
    if (!c.file || c.file == INVALID_HANDLE_VALUE)
    {
        c.file = CreateFileW(subject->pwsFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, 0);
        own = TRUE;
    }
    if (c.file == INVALID_HANDLE_VALUE) return FALSE;
    SetLastError(ERROR_INVALID_DATA);
    if (!alg || !CryptAcquireContextW(&prov, NULL, MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!cfb_open(&c)) goto release;

    if ((ex = find_root_stream(&c, L"\5MsiDigitalSignatureEx")))
    {
        BYTE pre[64], *stored;
        DWORD prelen = sizeof(pre);

        if (!CryptCreateHash(prov, alg, 0, 0, &c.hash)) goto close;
        c.budget = c.ndir;
        ret = prehash_dir(&c, c.dir, TRUE, 0) && CryptGetHashParam(c.hash, HP_HASHVAL, pre, &prelen, 0);
        CryptDestroyHash(c.hash);
        c.hash = 0;
        if (!ret) goto close;
        ret = FALSE;
        if (rd32(ex + 0x78) != prelen || rd32(ex + 0x7c)) goto close;
        if (!(stored = cfb_chain(&c, c.minifat, c.nminifat, rd32(ex + 0x74), c.msize, prelen, TRUE, &size))) goto close;
        ret = !memcmp(stored, pre, prelen);
        free(stored);
        if (!ret) goto close;
    }

    ret = FALSE;
    if (!CryptCreateHash(prov, alg, 0, 0, &c.hash)) goto close;
    c.budget = c.ndir;
    if (hash_dir(&c, c.dir, TRUE, 0))
    {
        *out_len = 64;
        ret = CryptGetHashParam(c.hash, HP_HASHVAL, out, out_len, 0);
    }
    CryptDestroyHash(c.hash);
close:
    cfb_free(&c);
release:
    CryptReleaseContext(prov, 0);
done:
    if (own) CloseHandle(c.file);
    return ret;
}

/***********************************************************************
 *              MsiSIPVerifyIndirectData  (MSISIP.@)
 */
BOOL WINAPI MsiSIPVerifyIndirectData(SIP_SUBJECTINFO *subject, SIP_INDIRECT_DATA *indirect)
{
    BYTE digest[64];
    DWORD len;

    TRACE("(%p %p)\n", subject, indirect);

    if (!subject || !indirect || !indirect->DigestAlgorithm.pszObjId)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!msi_digest(subject, indirect->DigestAlgorithm.pszObjId, digest, &len)) return FALSE;
    if (len != indirect->Digest.cbData || memcmp(digest, indirect->Digest.pbData, len))
    {
        SetLastError(TRUST_E_BAD_DIGEST);
        return FALSE;
    }
    return TRUE;
}

/***********************************************************************
 *              MsiSIPCreateIndirectData  (MSISIP.@)
 */
BOOL WINAPI MsiSIPCreateIndirectData(SIP_SUBJECTINFO *subject, DWORD *size, SIP_INDIRECT_DATA *indirect)
{
    static const char sipinfo[] = "1.3.6.1.4.1.311.2.1.30";
    const char *alg = subject && subject->DigestAlgorithm.pszObjId ? subject->DigestAlgorithm.pszObjId : szOID_OIWSEC_sha1;
    DWORD need, len;
    BYTE digest[64];
    char *p;

    TRACE("(%p %p %p)\n", subject, size, indirect);

    if (!subject || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!msi_digest(subject, alg, digest, &len)) return FALSE;
    need = sizeof(*indirect) + sizeof(sipinfo) + strlen(alg) + 1 + len;
    if (!indirect || *size < need)
    {
        *size = need;
        if (indirect) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return !indirect;
    }
    memset(indirect, 0, sizeof(*indirect));
    p = (char *)(indirect + 1);
    memcpy(p, sipinfo, sizeof(sipinfo));
    indirect->Data.pszObjId = p;
    p += sizeof(sipinfo);
    strcpy(p, alg);
    indirect->DigestAlgorithm.pszObjId = p;
    p += strlen(alg) + 1;
    memcpy(p, digest, len);
    indirect->Digest.cbData = len;
    indirect->Digest.pbData = (BYTE *)p;
    *size = need;
    return TRUE;
}

DEFINE_GUID(CLSID_MsiTransform, 0x000c1082,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_GUID(CLSID_MsiDatabase,  0x000c1084,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);
DEFINE_GUID(CLSID_MsiPatch,     0x000c1086,0x0000,0x0000,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46);

/***********************************************************************
 *              MsiSIPIsMyTypeOfFile (MSISIP.@)
 */
BOOL WINAPI MsiSIPIsMyTypeOfFile(WCHAR *name, GUID *subject)
{
    BOOL ret = FALSE;
    IStorage *stg = NULL;
    HRESULT r;

    TRACE("(%s, %p)\n", debugstr_w(name), subject);

    r = StgOpenStorage(name, NULL, STGM_DIRECT|STGM_READ|STGM_SHARE_DENY_WRITE,
     NULL, 0, &stg);
    if (SUCCEEDED(r))
    {
        STATSTG stat;

        r = IStorage_Stat(stg, &stat, STATFLAG_NONAME);
        if (SUCCEEDED(r))
        {
            if (IsEqualGUID(&stat.clsid, &CLSID_MsiDatabase) ||
             IsEqualGUID(&stat.clsid, &CLSID_MsiPatch) ||
             IsEqualGUID(&stat.clsid, &CLSID_MsiTransform))
            {
                ret = TRUE;
                *subject = mySubject;
            }
        }
        IStorage_Release(stg);
    }
    return ret;
}
