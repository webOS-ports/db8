/* @@@LICENSE
*
*      Copyright (c) 2009-2012 Hewlett-Packard Development Company, L.P.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */


#include "core/MojUtil.h"
#include "core/MojAutoPtr.h"
#include "core/MojString.h"
#include "core/MojFile.h"

#ifdef MOJ_UNALIGNED_MEM_ACCESS
#	define MojGet16Bits(d) (*((const guint16 *) (d)))
#else
#	define MojGet16Bits(d) ((((guint32)(((const guint8 *)(d))[1])) << 8)\
                       +(guint32)(((const guint8 *)(d))[0]) )
#endif

static const gsize MojMaxDirDepth = 100;

static MojErr MojRmDirRecursive(const MojChar* path, gsize depth);

static MojErr MojBase64EncodeImpl(const MojChar charset[], const guint8* src, gsize srcSize, MojChar* bufOut, gsize bufLen, gsize& lenOut, bool pad = true);
static MojErr MojBase64DecodeImpl(const guint8 vals[], gsize size, const MojChar* src, gsize srcLen, guint8* bufOut, gsize bufSize, gsize& sizeOut);

// Implementation of SuperFastHash from
// http://www.azillionmonkeys.com/qed/hash.html
gsize MojHash (const void* p, gsize len)
{
	MojAssert(p || len == 0);
	const guint8* src = (const guint8*) p;
	gsize hash = len;
	gsize tmp;
	int rem;

    if (len == 0)
    	return 0;
    rem = len & 3;
    len >>= 2;
    /* Main loop */
    for (;len > 0; len--) {
        hash  += MojGet16Bits(src);
        tmp    = (MojGet16Bits(src + 2) << 11) ^ hash;
        hash   = (hash << 16) ^ tmp;
        src  += 2 * sizeof(guint16);
        hash  += hash >> 11;
    }
    /* Handle end cases */
    switch (rem) {
        case 3: hash += MojGet16Bits(src);
                hash ^= hash << 16;
                hash ^= src[sizeof(guint16)] << 18;
                hash += hash >> 11;
                break;
        case 2: hash += MojGet16Bits(src);
                hash ^= hash << 11;
                hash += hash >> 17;
                break;
        case 1: hash += *src;
                hash ^= hash << 10;
                hash += hash >> 1;
                break;
    }
    /* Force "avalanching" of final 127 bits */
    hash ^= hash << 3;
    hash += hash >> 5;
    hash ^= hash << 4;
    hash += hash >> 17;
    hash ^= hash << 25;
    hash += hash >> 6;
    return hash;
}

gsize MojHash(const MojChar* str)
{
	return MojHash(str, MojStrLen(str) * sizeof(MojChar));
}

MojErr MojBase64Encode(const guint8* src, gsize srcSize, MojChar* bufOut, gsize bufLen, gsize& lenOut, bool pad)
{
	// NOTE: This is not the same set of characters used in mime base-64 encoding.
	// Unlike mime, this set is valid for file names and is ordered by ascii value.
	static const MojChar chars[] = "+0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
	return MojBase64EncodeImpl(chars, src, srcSize, bufOut, bufLen, lenOut, pad);
}

MojErr MojBase64EncodeMIME(const guint8* src, gsize srcSize, MojChar* bufOut, gsize bufLen, gsize& lenOut, bool pad)
{
	static const MojChar chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	return MojBase64EncodeImpl(chars, src, srcSize, bufOut, bufLen, lenOut, pad);
}

MojErr MojBase64EncodeImpl(const MojChar chars[], const guint8* src, gsize srcSize, MojChar* bufOut, gsize bufLen, gsize& lenOut, bool pad)
{
	MojAssert ((src || srcSize == 0) && (bufOut || bufLen == 0));

	gsize maxOut = MojBase64EncodedLenMax(srcSize);
	if (bufLen < maxOut)
		MojErrThrow(MojErrInsufficientBuf);

	// encode in 3 byte chunks
	guint32 chunk;
	MojChar* dest = bufOut;
	while (srcSize > 2) {
		chunk = (src[0] << 16) + (src[1] << 8) + src[2];
		*(dest++) = chars[chunk >> 18];
		*(dest++) = chars[(chunk >> 12) & 0x3F];
		*(dest++) = chars[(chunk >> 6) & 0x3F];
		*(dest++) = chars[chunk & 0x3F];
		src += 3;
		srcSize -= 3;
	}
	// deal with remainder
	switch (srcSize) {
	case 2:
		chunk = (src[0] << 16) + (src[1] << 8);
		*(dest++) = chars[chunk >> 18];
		*(dest++) = chars[(chunk >> 12) & 0x3F];
		*(dest++) = chars[(chunk >> 6) & 0x3F];
		if (pad) {
			*(dest++) = '=';
		}
		break;
	case 1:
		chunk = src[0] << 16;
		*(dest++) = chars[chunk >> 18];
		*(dest++) = chars[(chunk >> 12) & 0x3F];
		if (pad) {
			*(dest++) = '=';
			*(dest++) = '=';
		}
		break;
	default:
		MojAssert(srcSize == 0);
	}
	lenOut = dest - bufOut;
	MojAssert(lenOut <= maxOut && (maxOut == 0 || (lenOut >= maxOut - 2)));

	return MojErrNone;
}

MojErr MojBase64Decode(const MojChar* src, gsize srcLen, guint8* bufOut, gsize bufSize, gsize& sizeOut)
{
	static const guint8 vals[] = {
		/*       0,  1   2   3   4   5   6   7   8   9   a   b   c   d   e   f */
		/* 0 */255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
		/* 1 */255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
		/* 2 */255,255,255,255,255,255,255,255,255,255,255,  0,255,255,255,255,
		/* 3 */  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,255,255,255,255,255,255,
		/* 4 */255, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
		/* 5 */ 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,255,255,255,255, 37,
		/* 6 */255, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52,
		/* 7 */ 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
	};

	return MojBase64DecodeImpl(vals, sizeof(vals), src, srcLen, bufOut, bufSize, sizeOut);
}

MojErr MojBase64DecodeMIME(const MojChar* src, gsize srcLen, guint8* bufOut, gsize bufSize, gsize& sizeOut)
{
	static const guint8 vals[] = {
		/*       0,  1   2   3   4   5   6   7   8   9   a   b   c   d   e   f */
		/* 0 */255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
		/* 1 */255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
		/* 2 */255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63,
		/* 3 */ 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,255,255,255,
		/* 4 */255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		/* 5 */ 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255,
		/* 6 */255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		/* 7 */ 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
	};

	return MojBase64DecodeImpl(vals, sizeof(vals), src, srcLen, bufOut, bufSize, sizeOut);
}

MojErr MojBase64DecodeImpl(const guint8 vals[], gsize size, const MojChar* src, gsize srcLen, guint8* bufOut, gsize bufSize, gsize& sizeOut)
{
	MojAssert ((src || srcLen == 0) && (bufOut || bufSize == 0));

	sizeOut =  MojBase64DecodedSizeMax(srcLen);
	if (bufSize < sizeOut)
		MojErrThrow(MojErrInsufficientBuf);

	// trim padding
	for (gsize i = 0; i < 2; ++i) {
		if (srcLen > 0 && src[srcLen - 1] == _T('='))
			--srcLen;
	}
	// decode in 4 byte chunks
	guint32 chunk = 0;
	guint8* dest = bufOut;
	for (gsize i = 0; i < srcLen; ++i) {
		guint32 b;
		guint32 c = (guint32) src[i];
		if (c >= size || (b = vals[c]) == 255)
			MojErrThrow(MojErrInvalidBase64Data);
		switch (i & 3) {
		case 0:
			chunk = b << 18;
			break;
		case 1:
			chunk |= b << 12;
			break;
		case 2:
			chunk |= b << 6;
			break;
		case 3:
			chunk |= b;
			(*dest++) = (guint8) (chunk >> 16);
			(*dest++) = (guint8) ((chunk >> 8) & 0xFF);
			(*dest++) = (guint8) (chunk & 0xFF);
			break;
		}
	}
	// deal with remainder
	switch (srcLen & 3) {
	case 0:
		break;
	case 2:
		(*dest++) = (guint8) (chunk >> 16);
		sizeOut -= 2;
		break;
	case 3:
		(*dest++) = (guint8) (chunk >> 16);
		(*dest++) = (guint8) ((chunk >> 8) & 0xFF);
		--sizeOut;
		break;
	default:
		MojErrThrow(MojErrInvalidBase64Data);
		break;
	}
	MojAssert((gsize)(dest - bufOut) == sizeOut);

	return MojErrNone;
}

int MojLexicalCompare(const guint8* data1, gsize size1, const guint8* data2, gsize size2)
{
	// lexical comparison of two keys
	const guint8* end = data1 + MojMin(size1, size2);
	while (data1 != end) {
		if (*data1 != *data2)
			return *data1 - *data2;
		++data1;
		++data2;
	}
	return (int) (size1 - size2);
}

gsize MojPrefixSize(const guint8* data1, gsize size1, const guint8* data2, gsize size2)
{
	// find length of shared prefix
	gsize prefixSize = 0;
	const guint8* end = data1 + MojMin(size1, size2);
	while (data1 != end) {
		if (*data1 != *data2)
			break;
		++data1;
		++data2;
		++prefixSize;
	}
	return prefixSize;
}

MojErr MojCreateDirIfNotPresent(const MojChar* path)
{
	MojErr err = MojMkDir(path, MOJ_S_IRWXU);
	MojErrCatch(err, MojErrExists);
	MojErrCatch(err, MojErrNotFound) {
		MojString parentPath;
		err = parentPath.assign(path);
		MojErrCheck(err);
		gsize sep = parentPath.rfind(MojPathSeparator);
		if (sep == MojInvalidSize)
			MojErrThrow(MojErrNotFound);
		err = parentPath.truncate(sep);
		MojErrCheck(err);
		err = MojCreateDirIfNotPresent(parentPath);
		MojErrCheck(err);
	}
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojRmDirRecursive(const MojChar* path)
{
	return MojRmDirRecursive(path, 0);
}

MojErr MojRmDirRecursive(const MojChar* path, gsize depth)
{
	MojAssert(path);
	MojErr err = MojErrNone;

	// check for cycles
	if (depth >= MojMaxDirDepth)
		MojErrThrow(MojErrDirCycle);
	// validate args
	gsize pathLen = MojStrLen(path);
	if (pathLen == 0)
		MojErrThrow(MojErrInvalidArg);

	// setup buf for names
	MojAutoArrayPtr<MojChar> entName (new MojChar[pathLen + NAME_MAX + 2 /* for '/' and '\0' */]);
	MojAllocCheck(entName.get());
	MojStrCpy(entName.get(), path);
	if (entName[pathLen - 1] != MojPathSeparator) {
		entName[pathLen++] = MojPathSeparator;
	}
	// open dir
	MojDirT dir = MojInvalidDir;
	err = MojDirOpen(dir, path);
	MojErrCheck(err);
	// read entries
	for (;;) {
		gsize nameLen = 0;
		bool entRead = false;
		MojDirentT ent;
		err = MojDirRead(dir, &ent, entRead);
		MojErrGoto(err, Done);
		if (!entRead)
			break;
		if (MojStrCmp(ent.d_name, _T(".")) == 0 ||
			MojStrCmp(ent.d_name, _T("..")) == 0)
			continue;
		nameLen = MojStrLen(ent.d_name);
		MojAssert(nameLen <= NAME_MAX);
		MojStrNCpy(entName.get() + pathLen, ent.d_name, nameLen);
		entName[pathLen + nameLen] = '\0';
		if (ent.d_type == DT_DIR) {
			err = MojRmDirRecursive(entName.get(), depth + 1);
			MojErrGoto(err, Done);
		} else {
			err = MojUnlink(entName.get());
			MojErrGoto(err, Done);
		}
	}
	// remove dir
	err = MojRmDir(path);
	MojErrGoto(err, Done);
Done:
	if (dir != MojInvalidDir)
		(void) MojDirClose(dir);
	return err;
}

MojErr MojFileToString(const MojChar* path, MojString& strOut)
{
	MojFile file = MojInvalidFile;
	MojErr err = file.open(path, MOJ_O_RDONLY);
	MojErrCheck(err);

	err = file.readString(strOut);
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojFileFromString(const MojChar* path, const MojChar* data)
{
	MojFile file;
	MojErr err = file.open(path, MOJ_O_WRONLY | MOJ_O_CREAT | MOJ_O_TRUNC, MOJ_S_IRUSR | MOJ_S_IWUSR);
	MojErrCheck(err);
	gsize size;
	err = file.writeString(data, size);
	MojErrCheck(err);

	return MojErrNone;
}

const MojChar* MojFileNameFromPath(const MojChar* path)
{
	MojAssert(path);
	const MojChar* sep = MojStrChrReverse(path, MojPathSeparator);
	return sep ? sep + 1 : path;
}

MojErr MojUInt8ArrayToHex(const guint8 *bytes, gsize len, MojChar *s) 
{
	// Convert a byte array to Hex String for printing 
	char hexval[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
	for(gsize j = 0; j < len; j++){
		s[j*2] = hexval[((bytes[j] >> 4) & 0xF)];
		s[(j*2) + 1] = hexval[(bytes[j]) & 0x0F];
	}
	s[len *2] = 0x0;
	return MojErrNone;
}

