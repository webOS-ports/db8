// Copyright (c) 2009-2021 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0


#include "db/MojDb.h"
#include "db/MojDbServiceDefs.h"
#include "db/MojDbKind.h"
#include "core/MojJson.h"
#include "core/MojTime.h"
#include "core/MojObjectBuilder.h"

#include <fcntl.h>
#include <sys/stat.h>

MojErr MojDb::stats(MojObject& objOut, MojDbReqRef req, bool verify, MojString *pKind)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	MojErr err = beginReq(req);
	MojErrCheck(err);
	err = m_kindEngine.stats(objOut, req, verify, pKind);
	MojErrCheck(err);
	err = req->end();
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::quotaStats(MojObject& objOut, MojDbReqRef req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	MojErr err = beginReq(req);
	MojErrCheck(err);
	err = m_quotaEngine.stats(objOut, req);
	MojErrCheck(err);
	err = req->end();
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::updateLocale(const MojChar* locale, MojDbReqRef req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	MojAssert(locale);

	MojErr err = beginReq(req, true);
	MojErrCheck(err);
	MojString oldLocale;
	err = getLocale(oldLocale, req);
	MojErrCheck(err);
	MojString newLocale;
	err = newLocale.assign(locale);
	MojErrCheck(err);
	MojErr updateErr = err = updateLocaleImpl(oldLocale, newLocale, req);
	MojErrCatchAll(err) {
		err = req->abort();
		MojErrCheck(err);
		err = m_kindEngine.close();
		MojErrCheck(err);
		MojDbReq openReq;
		err = beginReq(openReq, true);
		MojErrCheck(err);
		err = m_kindEngine.open(this, openReq);
		MojErrCheck(err);
		err = openReq.end();
		MojErrCheck(err);
		MojErrThrow(updateErr);
	}
	return MojErrNone;
}

MojErr MojDb::compact()
{
    LOG_TRACE("Entering function %s", __FUNCTION__);
    MojThreadGuard guard(m_compact_mutex, false);
    if(guard.tryLock())
    {
		MojErr err = requireOpen();
		MojErrCheck(err);

		LOG_DEBUG("[db_mojodb] compacting...");

		err = m_storageEngine->compact();
		MojErrCheck(err);

		LOG_DEBUG("[db_mojodb] compaction complete");
    }

	return MojErrNone;
}

MojErr MojDb::purgeStatus(MojObject& revOut, MojDbReqRef req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	revOut = -1;
	MojErr err = beginReq(req);
	MojErrCheck(err);
    bool isFound;
	err = getState(LastPurgedRevKey, revOut, isFound, req);
	MojErrCheck(err);
	err = req->end();
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::purge(MojUInt32& countOut, MojInt64 numDays, MojDbReqRef req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

    if (!m_enablePurge) {
        return MojErrNone;
    }

	countOut = 0;
	if (numDays <= -1) {
		numDays = m_purgeWindow;
	}

    //mark obsolete shards records
    shardEngine()->purgeShardObjects(numDays);
#ifdef LMDB_ENGINE_SUPPORT
	MojErr err = beginReq(req, true);
#else
	MojErr err = beginReq(req);
#endif
	MojErrCheck(err);

    LOG_DEBUG("[db_mojodb] purging objects deleted more than %lld days ago...", numDays);

	MojTime time;
	err = MojGetCurrentTime(time);
	MojErrCheck(err);

	// store the revision number to current timestamp mapping
	MojObject revTimeMapping;
	MojInt64 rev;
#ifdef LMDB_ENGINE_SUPPORT
	err = nextId(rev, req->txn());
#else
	err = nextId(rev);
#endif
	MojErrCheck(err);
	err = revTimeMapping.put(RevNumKey, rev);
	MojErrCheck(err);
	err = revTimeMapping.put(TimestampKey, time.microsecs());
	MojErrCheck(err);
	err = revTimeMapping.putString(KindKey, MojDbKindEngine::RevTimestampId);
	MojErrCheck(err);

	err = putImpl(revTimeMapping, MojDbFlagNone, req);
	MojErrCheck(err);

	// find the revision number for numDays prior to now
	MojInt64 purgeTime = time.microsecs() - (MojTime::UnitsPerDay * numDays);
	MojDbQuery query;
	err = query.from(MojDbKindEngine::RevTimestampId);
	MojErrCheck(err);
	query.limit(1);
	err = query.where(TimestampKey, MojDbQuery::OpLessThanEq, purgeTime);
	MojErrCheck(err);
	err = query.order(TimestampKey);
	MojErrCheck(err);
	query.desc(true);

	MojDbCursor cursor;
	err = findImpl(query, cursor, NULL, req, OpDelete);
	MojErrCheck(err);
	bool found = false;
	MojObject obj;
	err = cursor.get(obj, found);
	MojErrCheck(err);
	err = cursor.close();
	MojErrCheck(err);

	MojUInt32 batchCount = 0;
	MojUInt32 totalCount = 0;

	while ((found))
	{
		// Do it in AutoBatchSize batches
		batchCount = 0;
		req->fixmode(true);		// purge even if index mis-matches
		err = purgeImpl(obj, batchCount, req);
		LOG_DEBUG("[db_mojodb] purge batch processed: batch: %d; total: %d; err = %d\n",
            batchCount, (totalCount + batchCount), err);
		MojErrCheck(err);
		totalCount += batchCount;
		countOut = totalCount;
		if (batchCount < AutoBatchSize)	// last batch
			break;
		err = commitBatch(req);
		MojErrCheck(err);
		continue;
	}

	// end request
	err = req->end();
	MojErrCheck(err);

	//compact db
	compact();

    LOG_DEBUG("[db_mojodb] purged %d objects", countOut);

	return MojErrNone;
}

MojErr MojDb::dump(const MojChar* path, MojUInt32& countOut, bool incDel, MojDbReqRef req, bool backup,
		MojUInt32 maxBytes, const MojObject* incrementalKey, MojObject* backupResponse)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);
	MojAssert(path);

	MojErr err = beginReq(req);
	MojErrCheck(err);

	if (!req->admin()) {
        LOG_ERROR(MSGID_DB_ADMIN_ERROR, 2,
        		PMLOGKS("data", req->domain().data()),
        		PMLOGKS("path", path),
        		"access denied: 'data' cannot dump db to path: 'path'");
		MojErrThrow(MojErrDbAccessDenied);
	}

	MojFile file;
	err = file.open(path, MOJ_O_WRONLY | MOJ_O_CREAT | MOJ_O_TRUNC, MOJ_S_IRUSR | MOJ_S_IWUSR);
	MojErrCheck(err);

	// write out kinds first, then existing objects, then deleted objects
	MojSize bytesWritten = 0;
	MojSize totalwarns = 0;
	MojSize newwarns = 0;
	MojDbQuery objQuery;
	MojVector<MojObject> kindVec;
	MojObject revParam = -1;
	MojObject delRevParam = -1;

	// if we were given an incremental key, pull out the revs now
	if (incrementalKey) {
		(void) incrementalKey->get(MojDbServiceDefs::RevKey, revParam);
		(void) incrementalKey->get(MojDbServiceDefs::DeletedRevKey, delRevParam);
	}

    // if 'enableRootKind' flag is true, find subkinds from root
    if (m_enableRootKind) {
        err = m_kindEngine.getKinds(kindVec);
        MojErrCheck(err);
    }
    else {
        err = m_kindEngine.getKind(kindVec, MojDbKindEngine::KindKindId);
        MojErrCheck(err);
        err = m_kindEngine.getKind(kindVec, MojDbKindEngine::PermissionId);
        MojErrCheck(err);
        err = m_kindEngine.getKind(kindVec, MojDbKindEngine::DbStateId);
        MojErrCheck(err);
        err = m_kindEngine.getKind(kindVec, MojDbKindEngine::RevTimestampId);
        MojErrCheck(err);
        err = m_kindEngine.getKind(kindVec, MojDbKindEngine::QuotaId);
        MojErrCheck(err);
    }

	// write kinds - if incremental, only write the kinds that have changed since the respective revs
	MojString countStr;
	for (MojVector<MojObject>::ConstIterator i = kindVec.begin(); i != kindVec.end(); ++i) {
		if (backup) {
			bool backupKind = false;
			i->get(MojDbKind::SyncKey, backupKind);
			if (!backupKind)
				continue;
			MojString id;
			err = i->getRequired(MojDbServiceDefs::IdKey, id);
			MojErrCheck(err);
			MojDbQuery countQuery;
			err = countQuery.from(id);
			MojErrCheck(err);
			MojDbCursor cursor;
			err = find(countQuery, cursor, req);
			MojErrCheck(err);
			MojUInt32 count = 0;
			err = cursor.count(count);
			MojErrCheck(err);
			if (count > 0) {
				if (i != kindVec.begin()) {
					err = countStr.appendFormat(_T(", "));
					MojErrCheck(err);
				}
				err = countStr.appendFormat("%s=%u", id.data(), count);
				MojErrCheck(err);
			}
		}
        // '_del'(DelKey) is only exist when
        // - enableRootKind : true
        // - enablePurge : true
        // - query must contain "purge":false option
        //
		bool deleted = false;
		i->get(DelKey, deleted);
		MojObject kindRev;
		err = i->getRequired(RevKey, kindRev);
		MojErrCheck(err);
		if ((deleted && kindRev > delRevParam) || (!deleted && kindRev > revParam)) {
			err = dumpObj(file, (*i), bytesWritten, maxBytes);
			MojErrCheck(err);
			countOut++;
		}
	}

	// dump all the non-deleted objects
    if (m_enableRootKind) {
        err = dumpImpl(file, backup, false, revParam, delRevParam, true, countOut, req, backupResponse, MojDbServiceDefs::RevKey, bytesWritten, newwarns, maxBytes);
        MojErrCheck(err);
        totalwarns += newwarns;
    }
    else {
        err = dumpImpl(file, backup, false, revParam, delRevParam, true, countOut, req, backupResponse, MojDbServiceDefs::RevKey, bytesWritten, newwarns, kindVec, maxBytes);
        MojErrCheck(err);
        totalwarns += newwarns;
    }
	// If we're supposed to include deleted objects, dump the deleted objects now.
	// There's a chance that we may have run out of space in our backup.  If that's the case,
	// we don't want to try to dump deleted objects - we can detect this by looking for the HasMoreKey
	if (incDel && backupResponse && !backupResponse->contains(MojDbServiceDefs::HasMoreKey)) {
        if (m_enableRootKind) {
            err = dumpImpl(file, backup, true, revParam, delRevParam, false, countOut, req, backupResponse, MojDbServiceDefs::DeletedRevKey, bytesWritten, newwarns, maxBytes);
            MojErrCheck(err);
        }
        else {
            err = dumpImpl(file, backup, true, revParam, delRevParam, false, countOut, req, backupResponse, MojDbServiceDefs::DeletedRevKey, bytesWritten, newwarns, kindVec, maxBytes);
            MojErrCheck(err);
        }
	}
	totalwarns += newwarns;

	// Add the Full and Version keys
	if (backup && backupResponse) {
		bool incremental = (incrementalKey != NULL);
		err = backupResponse->putBool(MojDbServiceDefs::FullKey, !incremental);
		MojErrCheck(err);
		err = backupResponse->put(MojDbServiceDefs::VersionKey, DatabaseVersion);
		MojErrCheck(err);
		err = backupResponse->put(MojDbServiceDefs::WarningsKey, (MojInt32)totalwarns);
		MojErrCheck(err);
		MojString description;
		err = description.format(_T("incremental=%u"), countOut);
		MojErrCheck(err);
		if (!countStr.empty()) {
			err = description.appendFormat(_T(", %s"), countStr.data());
			MojErrCheck(err);
		}
		err = backupResponse->put(_T("description"), description);
		MojErrCheck(err);
	}

	err = req->end();
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::load(const MojChar* path, MojUInt32& countOut, MojUInt32 flags, MojDbReqRef req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	return loadFile(path, countOut, flags, req, NULL, NULL, 0);
}

// Directories the service's load method may read from: the temp
// directories the backup service (woce-backup) works in, the file cache's
// backuptempdir type and its own fallback.
const MojChar* const MojDb::DefaultLoadDirs[] = {
	_T("/var/file-cache/backuptempdir"),
	_T("/media/internal/.woce-backup/tmp"),
};
const MojSize MojDb::DefaultLoadDirCount = sizeof(MojDb::DefaultLoadDirs) / sizeof(MojDb::DefaultLoadDirs[0]);

// System configuration the image ships, which a system service loads at
// start: SettingsService loads /etc/palm/defaultSettings.json. Read only from
// files root alone can have put there (see openNoFollow).
const MojChar* const MojDb::SystemLoadDirs[] = {
	_T("/etc/palm"),
};
const MojSize MojDb::SystemLoadDirCount = sizeof(MojDb::SystemLoadDirs) / sizeof(MojDb::SystemLoadDirs[0]);

namespace {
long dirDepth(const MojChar* path, const MojChar* dir);

bool belowAny(const MojChar* path, const MojChar* const* dirs, MojSize count)
{
	for (MojSize i = 0; dirs && i < count; ++i) {
		if (dirDepth(path, dirs[i]) >= 0)
			return true;
	}
	return false;
}
} // namespace

MojErr MojDb::loadChecked(const MojString& path, MojUInt32& countOut, MojUInt32 flags, MojDbReqRef req)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	// The bus method reads the file as this service's (root) user, so the
	// path is the caller's only to choose within the load directories.
	if (MojStrLen(path.data()) != path.length())
		MojErrThrowMsg(MojErrInvalidArg, _T("db: invalid load path"));
	const bool haveTemp = m_loadDirs && m_loadDirCount > 0;
	const bool haveSystem = m_systemLoadDirs && m_systemLoadDirCount > 0;
	if (!haveTemp && !haveSystem)
		MojErrThrowMsg(MojErrDbAccessDenied, _T("db: no load directories"));
	if (haveSystem && !belowAny(path.data(), m_loadDirs, m_loadDirCount) &&
		belowAny(path.data(), m_systemLoadDirs, m_systemLoadDirCount)) {
		return loadFile(path.data(), countOut, flags, req, NULL, m_systemLoadDirs, m_systemLoadDirCount, &m_systemLoadOwner);
	}
	// never NULL here: a NULL list means "unrestricted" to loadFile
	static const MojChar* const NoDirs[] = {NULL};
	return loadFile(path.data(), countOut, flags, req, NULL, haveTemp ? m_loadDirs : NoDirs, haveTemp ? m_loadDirCount : 0);
}

namespace {

// Number of path components in dir ("/" is 0, "/a/b" is 2) if path is at
// or below it on a component boundary, else -1.
long dirDepth(const MojChar* path, const MojChar* dir)
{
	if (!dir || dir[0] != _T('/'))
		return -1;
	MojSize len = MojStrLen(dir);
	while (len > 1 && dir[len - 1] == _T('/'))
		--len;
	if (len == 1)
		return 0;
	if (MojStrNCmp(path, dir, len) != 0 || (path[len] != _T('\0') && path[len] != _T('/')))
		return -1;
	long depth = 0;
	for (MojSize i = 0; i < len; ++i) {
		if (dir[i] == _T('/'))
			++depth;
	}
	return depth;
}

} // namespace

// For a system configuration file: owned by root (or the test owner) and
// writable by nobody else, so only root can have put it or its directories
// there. A world-writable directory on the way (/tmp) is fine if it is
// sticky, as nobody else can then rename or replace what root owns in it.
bool MojDb::trustedLoadDir(uid_t uid, mode_t mode, uid_t owner)
{
	if (uid != 0 && uid != owner)
		return false;
	return (mode & (S_IWGRP | S_IWOTH)) == 0 || (mode & S_ISVTX) != 0;
}

bool MojDb::trustedLoadFile(uid_t uid, mode_t mode, uid_t owner)
{
	return (uid == 0 || uid == owner) &&
		(mode & (S_IWGRP | S_IWOTH)) == 0 && (mode & S_IROTH) != 0;
}

namespace {

MojErr checkTrustedDir(int fd, const uid_t* trustedOwner)
{
	if (!trustedOwner)
		return MojErrNone;
	struct stat dirStat;
	if (::fstat(fd, &dirStat) != 0 || !MojDb::trustedLoadDir(dirStat.st_uid, dirStat.st_mode, *trustedOwner))
		MojErrThrowMsg(MojErrDbAccessDenied, _T("db: load path has a directory that is not root's alone"));
	return MojErrNone;
}

} // namespace

MojErr MojDb::openNoFollow(const MojChar* path, MojSize pathLen, const MojChar* const* allowedDirs, MojSize nAllowedDirs, MojFileT& fdOut,
		const uid_t* trustedOwner)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);

	fdOut = MojInvalidFile;
	if (!path || pathLen == 0 || path[0] != _T('/') || MojStrLen(path) != pathLen || pathLen >= MojPathMax)
		MojErrThrowMsg(MojErrInvalidArg, _T("db: load path must be an absolute path"));

	// split into components: no empty ones, no "." or "..", and a file name last
	MojChar copy[MojPathMax];
	MojStrNCpy(copy, path, pathLen);
	copy[pathLen] = _T('\0');
	MojChar* comps[MojPathMax / 2];
	MojSize nComps = 0;
	for (MojChar* p = copy + 1; ; ) {
		MojChar* slash = const_cast<MojChar*>(MojStrChr(p, _T('/')));
		if (slash)
			*slash = _T('\0');
		if (*p == _T('\0') || MojStrCmp(p, _T(".")) == 0 || MojStrCmp(p, _T("..")) == 0)
			MojErrThrowMsg(MojErrInvalidArg, _T("db: invalid load path"));
		comps[nComps++] = p;
		if (!slash)
			break;
		p = slash + 1;
	}

	// below one of the allowed directories (the deepest that matches)
	long depth = -1;
	for (MojSize i = 0; i < nAllowedDirs; ++i) {
		long d = dirDepth(path, allowedDirs[i]);
		if (d > depth)
			depth = d;
	}
	if (depth < 0 || (MojSize) depth >= nComps)
		MojErrThrowMsg(MojErrDbAccessDenied, _T("db: load path is outside the allowed directories"));

	// Walk from / one component at a time without following a symbolic
	// link anywhere, then read only through the fd we opened, so nothing can
	// be swapped in between the check and the read.
	int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		MojErrThrowErrno(_T("open"));
	MojErr err = checkTrustedDir(fd, trustedOwner);
	if (err != MojErrNone) {
		(void) ::close(fd);
		return err;
	}
	for (MojSize i = 0; i + 1 < nComps; ++i) {
		int next = ::openat(fd, comps[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (next < 0) {
			int e = errno;
			(void) ::close(fd);
			if (e == ELOOP || e == ENOTDIR)
				MojErrThrowMsg(MojErrDbAccessDenied, _T("db: load path has a symbolic link or non-directory component"));
			errno = e;
			MojErrThrowErrno(_T("openat"));
		}
		(void) ::close(fd);
		fd = next;
		err = checkTrustedDir(fd, trustedOwner);
		if (err != MojErrNone) {
			(void) ::close(fd);
			return err;
		}
	}
	// O_NONBLOCK: a FIFO in its place must not block the service
	int file = ::openat(fd, comps[nComps - 1], O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
	int e = errno;
	(void) ::close(fd);
	if (file < 0) {
		if (e == ELOOP)
			MojErrThrowMsg(MojErrDbAccessDenied, _T("db: load file is a symbolic link"));
		errno = e;
		MojErrThrowErrno(_T("openat"));
	}
	struct stat st;
	if (::fstat(file, &st) != 0 || !S_ISREG(st.st_mode)) {
		(void) ::close(file);
		MojErrThrowMsg(MojErrDbAccessDenied, _T("db: load file is not a regular file"));
	}
	if (trustedOwner && !trustedLoadFile(st.st_uid, st.st_mode, *trustedOwner)) {
		(void) ::close(file);
		MojErrThrowMsg(MojErrDbAccessDenied, _T("db: system load file must be root's, read-only to others"));
	}
	fdOut = file;

	return MojErrNone;
}

MojErr MojDb::restorePath(const MojChar* dir, const MojString& fileName, MojString& pathOut)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);
	MojAssert(dir);

	// The names come from the backup manifest, which is as untrusted as the
	// dumps: a bare name inside dir, nothing that walks out of it
	if (dir[0] != _T('/'))
		MojErrThrowMsg(MojErrInvalidArg, _T("db: restore directory must be an absolute path"));
	if (fileName.empty() || MojStrLen(fileName.data()) != fileName.length() ||
		fileName.find(_T('/')) != MojInvalidIndex ||
		fileName == _T(".") || fileName == _T(".."))
		MojErrThrowMsg(MojErrInvalidArg, _T("db: invalid restore file name"));

	// The backup service passes tempDir with a trailing slash
	// ("/media/internal/.woce-backup/tmp/"); joined as it is, that makes an
	// empty component, which openNoFollow refuses. "/" stays "/".
	MojSize dirLen = MojStrLen(dir);
	while (dirLen > 1 && dir[dirLen - 1] == _T('/'))
		--dirLen;
	MojErr err = pathOut.assign(dir, dirLen);
	MojErrCheck(err);
	if (dirLen > 1) {
		err = pathOut.append(_T('/'));
		MojErrCheck(err);
	}
	err = pathOut.append(fileName);
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::restore(const MojChar* dir, const MojObject& fileNames, RestoreStats& statsOut, MojDbReqRef req)
{
	LOG_TRACE("Entering function %s", __FUNCTION__);
	MojAssert(dir);

	if (!req->admin()) {
		LOG_ERROR(MSGID_DB_ADMIN_ERROR, 1,
				PMLOGKS("data", req->domain().data()),
				"access denied: 'data' cannot restore db");
		MojErrThrow(MojErrDbAccessDenied);
	}
	if (fileNames.type() != MojObject::TypeArray)
		MojErrThrowMsg(MojErrInvalidArg, _T("db: restore files must be an array"));

	// check every name before loading anything
	MojVector<MojString> paths;
	for (MojObject::ConstArrayIterator i = fileNames.arrayBegin(); i != fileNames.arrayEnd(); ++i) {
		MojString fileName;
		MojErr err = i->stringValue(fileName);
		MojErrCheck(err);
		MojString path;
		err = restorePath(dir, fileName, path);
		MojErrCheck(err);
		err = paths.push(path);
		MojErrCheck(err);
	}

	statsOut = RestoreStats();
	for (MojVector<MojString>::ConstIterator i = paths.begin(); i != paths.end(); ++i) {
		MojUInt32 count = 0;
		MojErr err = loadFile(i->data(), count, MojDbFlagRestore, req, &statsOut, NULL, 0);
		MojErrCheck(err);
	}

	return MojErrNone;
}

MojErr MojDb::loadFile(const MojChar* path, MojUInt32& countOut, MojUInt32 flags, MojDbReq& req, RestoreStats* stats,
		const MojChar* const* allowedDirs, MojSize nAllowedDirs, const uid_t* trustedOwner)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);
	MojAssert(path);

	const bool restoring = MojFlagGet(flags, MojDbFlagRestore);
	RestoreStats localStats;
	RestoreStats* const st = stats ? stats : &localStats;
	const RestoreStats statsBefore = *st;

	// A restore dump may sit anywhere the caller says, but no component of
	// its path may be a symbolic link; a load through the service must also
	// be below one of the load directories (see loadChecked).
	static const MojChar* const AnyDir[] = {_T("/")};
	const MojChar* const* dirs = allowedDirs;
	MojSize nDirs = nAllowedDirs;
	if (restoring && !dirs) {
		dirs = AnyDir;
		nDirs = 1;
	}
	MojFileT fd = MojInvalidFile;
	MojErr err = MojErrNone;
	if (dirs) {
		err = openNoFollow(path, MojStrLen(path), dirs, nDirs, fd, trustedOwner);
		MojErrCheck(err);
	} else {
		err = MojFileOpen(fd, path, MOJ_O_RDONLY);
		MojErrCheck(err);
	}
	MojFile file(fd);

	// only once the file is open, so a refused path starts no transaction
	err = beginReq(req, true);
	MojErrCheck(err);

	MojJsonParser parser;
	parser.begin();
	MojSize bytesRead = 0;
	MojObjectBuilder visitor;

	int total_mutexes, mutexes_free, mutexes_used, mutexes_used_highwater, mutex_regionsize;
	m_objDb->mutexStats(&total_mutexes, &mutexes_free, &mutexes_used, &mutexes_used_highwater, &mutex_regionsize);

    LOG_DEBUG("[db_mojodb] Starting load of %s, total_mutexes: %d, mutexes_free: %d, mutexes_used: %d, mutexes_used_highwater: %d, &mutex_regionsize: %d\n",
		path,	total_mutexes, mutexes_free, mutexes_used, mutexes_used_highwater, mutex_regionsize);

	int orig_mutexes_used = mutexes_used;

	struct timeval startTime = {0,0}, stopTime = {0,0};

	gettimeofday(&startTime, NULL);

	int total_transaction_time = 0;

	int total = 0;
	int transactions = 0;

	do {
		MojChar buf[MojFile::MojFileBufSize];
		err = file.read(buf, sizeof(buf), bytesRead);
		MojErrCheck(err);

		const MojChar* parseEnd = buf;
		while (parseEnd < (buf + bytesRead)) {
			err = parser.parseChunk(visitor, parseEnd, bytesRead - (parseEnd - buf), parseEnd);
			MojErrCheck(err);
			if (parser.finished()) {
				//store the object
				if (restoring)
					err = loadRestoreImpl(visitor.object(), req, *st);
				else
					err = loadImpl(visitor.object(), flags, req);
				MojErrCheck(err);
				countOut++;
				parser.begin();
				visitor.reset();

				total++;

				if ((total % 10) == 0) {
					// For debugging mutex consumption during load operations, we periodically retrieve the mutex stats.
					m_objDb->mutexStats(&total_mutexes, &mutexes_free, &mutexes_used, &mutexes_used_highwater, &mutex_regionsize);

					LOG_DEBUG("[db_mojodb] Loading %s record %d, total_mutexes: %d, mutexes_free: %d, mutexes_used: %d, mutexes_used_highwater: %d, &mutex_regionsize: %d\n",
						path, total, total_mutexes, mutexes_free, mutexes_used, mutexes_used_highwater, mutex_regionsize);
				}

				// If a loadStepSize is configured, then break up the load into separate transactions.
				// This is intended to prevent run-away mutex consumption in some particular scenarios.
				// The transactions do not reverse or prevent mutex consumption, but seem to reduce the
				// growth and eventually cause it to level off.

				if ((m_loadStepSize > 0) && ((total % m_loadStepSize) == 0)) {
					// Close and reopen transaction, to prevent a very large transaction from building up.
					LOG_DEBUG("[db_mojodb] Loading %s record %d, closing and reopening transaction.\n", path, total);

					struct timeval transactionStartTime = {0,0}, transactionStopTime = {0,0};

					gettimeofday(&transactionStartTime, NULL);

					err = req.end();
					MojErrCheck(err);

					err = req.endBatch();
					MojErrCheck(err);

					req.beginBatch(); // beginBatch() invocation for first transaction happened in MojDbServiceHandlerBase::invokeImpl

					err = beginReq(req, true);
					MojErrCheck(err);

					gettimeofday(&transactionStopTime, NULL);

					long int elapsedTransactionTimeMS = (transactionStopTime.tv_sec - transactionStartTime.tv_sec) * 1000 +
								(transactionStopTime.tv_usec - transactionStartTime.tv_usec) / 1000;

					total_transaction_time += (int)elapsedTransactionTimeMS;

					transactions++;
				}
			}
		}
	} while (bytesRead > 0);

	err = parser.end(visitor);
	MojErrCheck(err);
	if (parser.finished()) {
		if (restoring)
			err = loadRestoreImpl(visitor.object(), req, *st);
		else
			err = loadImpl(visitor.object(), flags, req);
		MojErrCheck(err);
		countOut++;
	} else if (bytesRead > 0) {
		MojErrThrow(MojErrJsonParseEof);
	}

	err = req.end();
	MojErrCheck(err);
	err = req.endBatch(); //double call for commit() isn't a problem
	MojErrCheck(err);

	gettimeofday(&stopTime, NULL);
	long int elapsedTimeMS = (stopTime.tv_sec - startTime.tv_sec) * 1000 +
				(stopTime.tv_usec - startTime.tv_usec) / 1000;

	m_objDb->mutexStats(&total_mutexes, &mutexes_free, &mutexes_used, &mutexes_used_highwater, &mutex_regionsize);

	LOG_DEBUG("[db_mojodb] Finished load of %s, total_mutexes: %d, mutexes_free: %d, mutexes_used: %d, mutexes_used_highwater: %d, &mutex_regionsize: %d\n",
		path, total_mutexes, mutexes_free, mutexes_used, mutexes_used_highwater, mutex_regionsize);

    LOG_DEBUG("[db_mojodb] Loaded %s with %d records in %ldms (%dms of that for %d extra transactions), consuming %d mutexes, afterwards %d are available out of %d\n",
		path, total, elapsedTimeMS, total_transaction_time, transactions, mutexes_used - orig_mutexes_used, mutexes_free, total_mutexes);

	if (restoring) {
		// counts only: the records themselves are user data
		LOG_INFO(MSGID_DB_RESTORE, 9,
			PMLOGKFV("records", "%u", st->records - statsBefore.records),
			PMLOGKFV("restored", "%u", st->restored - statsBefore.restored),
			PMLOGKFV("kinds", "%u", st->kinds - statsBefore.kinds),
			PMLOGKFV("permissions", "%u", st->permissions - statsBefore.permissions),
			PMLOGKFV("tombstones", "%u", st->tombstones - statsBefore.tombstones),
			PMLOGKFV("unknownKind", "%u", st->unknownKind - statsBefore.unknownKind),
			PMLOGKFV("noBackupKind", "%u", st->noBackupKind - statsBefore.noBackupKind),
			PMLOGKFV("existing", "%u", st->existing - statsBefore.existing),
			PMLOGKFV("invalid", "%u", st->invalid - statsBefore.invalid),
			"restore: loaded a backup dump");
	}

	return MojErrNone;
}

MojErr MojDb::updateLocaleImpl(const MojString& oldLocale, const MojString& newLocale, MojDbReq& req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	if (oldLocale != newLocale) {
		MojErr err = m_kindEngine.updateLocale(newLocale, req);
		MojErrCheck(err);
		err = updateState(LocaleKey, newLocale, req);
		MojErrCheck(err);
	}
	MojErr err = req.end();
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::dumpImpl(MojFile& file, bool backup, bool incDel, const MojObject& revParam, const MojObject& delRevParam, bool skipKinds, MojUInt32& countOut, MojDbReq& req,
        MojObject* response, const MojChar* keyName, MojSize& bytesWritten, MojSize& warns, MojVector<MojObject>& kindVec, MojUInt32 maxBytes)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

    // Store all object to 'idSet'
    // Because we suppose that there is no root kind
    // idSet is treated like Object:1 kind
    MojSet<MojObject> idSet;
    for (MojVector<MojObject>::ConstIterator i = kindVec.begin(); i != kindVec.end(); ++i) {
        MojDbQuery query;
        MojString id;
        bool found;
        MojErr err = i->get(MojDbServiceDefs::IdKey, id, found);
        MojErrCheck(err);
        if (!found)
            continue;
        err = query.from(id);
        MojErrCheck(err);
        err = query.where(MojDb::DelKey, MojDbQuery::OpEq, incDel);
        MojErrCheck(err);

        MojDbCursor cursor;
        err = findImpl(query, cursor, NULL, req, OpRead);
        MojErrCheck(err);
        warns = 0;

        for(;;) {
            bool found = false;
            MojObject obj;
            err = cursor.get(obj, found);
            // So that we can get as much data as possible from a corrupt database
            // We simply skip ghost keys and continue
            if (err == MojErrInternalIndexOnFind) {
                warns++;
                continue;
            }
            MojErrCheck(err);
            if (!found)
                break;

            if (skipKinds) {
                MojString kind;
                err = obj.getRequired(KindKey, kind);
                MojErrCheck(err);
                if (kind == MojDbKindEngine::KindKindId) {
                    continue;
                }
            }
            // find and store 'ALL' object into the set (for removing duplication)
            err = idSet.put(obj);
            MojErrCheck(err);
        }
        err = cursor.close();
        MojErrCheck(err);
    }

    MojObject curRev;
    for (MojSet<MojObject>::ConstIterator i = idSet.begin(); i != idSet.end(); ++i) {
        // write out each object, if the backup is full, insert the appropriate incremental key
        MojErr err = dumpObj(file, (*i), bytesWritten, maxBytes);
        MojErrCatch(err, MojErrDbBackupFull) {
            if (response) {
                MojErr errBackup = MojErrNone;
                if (!curRev.undefined()) {
                    errBackup = insertIncrementalKey(*response, keyName, curRev);
                    MojErrCheck(errBackup);
                } else {
                    errBackup = insertIncrementalKey(*response, keyName, incDel ? delRevParam: revParam);
                    MojErrCheck(errBackup);
                }
                errBackup = handleBackupFull(revParam, delRevParam, *response, keyName);
                MojErrCheck(errBackup);
            }
            return MojErrNone;
        }
        MojErrCheck(err);
        err = (*i).getRequired(MojDb::RevKey, curRev);
        MojErrCheck(err);
        countOut++;
    }

    if (warns > 0) {
        LOG_WARNING(MSGID_MOJ_DB_ADMIN_WARNING, 1,
                PMLOGKFV("warn", "%d", (int)warns),
                "Finished Backup with 'warn' warnings");
    } else {
        LOG_DEBUG("[db_mojodb] Finished Backup with no warnings \n");
    }

    // construct the next incremental key
    if (response && !curRev.undefined()) {
        MojErr err = insertIncrementalKey(*response, keyName, curRev);
        MojErrCheck(err);
    }

    return MojErrNone;
}

MojErr MojDb::dumpImpl(MojFile& file, bool backup, bool incDel, const MojObject& revParam, const MojObject& delRevParam, bool skipKinds, MojUInt32& countOut, MojDbReq& req,
		MojObject* response, const MojChar* keyName, MojSize& bytesWritten, MojSize& warns, MojUInt32 maxBytes)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	// query for objects, adding the backup key and rev key if necessary
	MojDbQuery query;
	MojErr err = query.from(MojDbKindEngine::RootKindId);
	MojErrCheck(err);
	err = query.where(MojDb::DelKey, MojDbQuery::OpEq, incDel);
	MojErrCheck(err);
	if (backup) {
		err = query.where(MojDb::SyncKey, MojDbQuery::OpEq, true);
		MojErrCheck(err);
		if (incDel) {
			err = query.where(MojDb::RevKey, MojDbQuery::OpGreaterThan, delRevParam);
			MojErrCheck(err);
		} else {
			err = query.where(MojDb::RevKey, MojDbQuery::OpGreaterThan, revParam);
			MojErrCheck(err);
		}
	}

	MojDbCursor cursor;
	err = findImpl(query, cursor, NULL, req, OpRead);
	MojErrCheck(err);
	warns = 0;

	MojObject curRev;
	for(;;) {
		bool found = false;
		MojObject obj;
		err = cursor.get(obj, found);
		// So that we can get as much data as possible from a corrupt database
		// We simply skip ghost keys and continue
		if (err == MojErrInternalIndexOnFind) {
			warns++;
			continue;
		}
		MojErrCheck(err);
		if (!found)
			break;

		if (skipKinds) {
			MojString kind;
			err = obj.getRequired(KindKey, kind);
			MojErrCheck(err);
			if (kind == MojDbKindEngine::KindKindId) {
				continue;
			}
		}

		// write out each object, if the backup is full, insert the appropriate incremental key
		err = dumpObj(file, obj, bytesWritten, maxBytes);
		MojErrCatch(err, MojErrDbBackupFull) {
			if (response) {
				MojErr errBackup = MojErrNone;
				if (!curRev.undefined()) {
					errBackup = insertIncrementalKey(*response, keyName, curRev);
					MojErrCheck(errBackup);
				} else {
					errBackup = insertIncrementalKey(*response, keyName, incDel ? delRevParam: revParam);
					MojErrCheck(errBackup);
				}
				errBackup = handleBackupFull(revParam, delRevParam, *response, keyName);
				MojErrCheck(errBackup);
			}
			return MojErrNone;
		}
		MojErrCheck(err);
		err = obj.getRequired(MojDb::RevKey, curRev);
		MojErrCheck(err);
		countOut++;
	}

	err = cursor.close();
	MojErrCheck(err);

    if (warns > 0) {
        LOG_WARNING(MSGID_MOJ_DB_ADMIN_WARNING, 1,
            PMLOGKFV("warn", "%d", (int)warns),
            "Finished Backup with 'warn' warnings");
    } else {
        LOG_DEBUG("[db_mojodb] Finished Backup with no warnings \n");
    }

	// construct the next incremental key
	if (response && !curRev.undefined()) {
		err = insertIncrementalKey(*response, keyName, curRev);
		MojErrCheck(err);
	}
	return MojErrNone;
}

MojErr MojDb::dumpObj(MojFile& file, MojObject obj, MojSize& bytesWrittenOut, MojUInt32 maxBytes)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	// remove the rev key before dumping the object
	bool found = false;
	MojErr err = obj.del(RevKey, found);
	MojErrCheck(err);

	MojString str;
	err = obj.toJson(str);
	MojErrCheck(err);
	err = str.append(_T("\n"));
	MojErrCheck(err);
	MojSize len = str.length() * sizeof(MojChar);
	// if writing this object will put us over the max length, throw an error
	if (maxBytes && bytesWrittenOut + len > maxBytes) {
		MojErrThrow(MojErrDbBackupFull);
	}
	err = file.writeString(str, bytesWrittenOut);
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::handleBackupFull(const MojObject& revParam, const MojObject& delRevParam, MojObject& response, const MojChar* keyName)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	// if we filled up the allocated space for our backup, we need to construct the appropriate
	// response with incremental key so that we are called back to continue where we left off
	MojString keyStr;
	MojErr err = keyStr.assign(keyName);
	MojErrCheck(err);
	MojObject incremental;
	(void) response.get(MojDbServiceDefs::IncrementalKey, incremental);

	err = response.put(MojDbServiceDefs::HasMoreKey, true);
	MojErrCheck(err);
	if (keyStr == MojDbServiceDefs::RevKey) {
		err = incremental.put(MojDbServiceDefs::DeletedRevKey, delRevParam);
		MojErrCheck(err);
	} else if (keyStr == MojDbServiceDefs::DeletedRevKey) {
		err = incremental.put(MojDbServiceDefs::RevKey, revParam);
		MojErrCheck(err);
	}
	err = response.put(MojDbServiceDefs::IncrementalKey, incremental);
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::insertIncrementalKey(MojObject& response, const MojChar* keyName, const MojObject& curRev)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	// get the incremental key if it already exists
	MojObject incremental;
	(void) response.get(MojDbServiceDefs::IncrementalKey, incremental);

	MojErr err = incremental.put(keyName, curRev);
	MojErrCheck(err);
	err = response.put(MojDbServiceDefs::IncrementalKey, incremental);
	MojErrCheck(err);

	return MojErrNone;
}

MojErr MojDb::loadImpl(MojObject& obj, MojUInt32 flags, MojDbReq& req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	bool found = false;
	MojString kindName;
	MojErr err = obj.get(MojDb::KindKey, kindName, found);
	MojErrCheck(err);
	if (!found)
		MojErrThrow(MojErrDbKindNotSpecified);

	bool deleted = false;
	obj.get(MojDb::DelKey, deleted);

	// when loading objects, if the object is deleted, call delKind/del
	// otherwise, call putKind, putPermissions or put depending on the kind
	if (deleted) {
		MojObject id;
		if (obj.get(MojDbServiceDefs::IdKey, id)) {
			if (kindName.startsWith(MojDbKindEngine::KindKindIdPrefix)) {
				err = delKind(id, found, flags, req);
				MojErrCheck(err);
			} else {
				err = del(id, found, flags, req);
				MojErrCheck(err);
			}
		}
	} else {
		if (kindName.startsWith(MojDbKindEngine::KindKindIdPrefix)) {
			err = putKind(obj, flags, req);
			MojErrCheck(err);
		} else if (kindName.startsWith(MojDbKindEngine::PermissionIdPrefix)) {
			err = putPermissions(&obj, &obj + 1, req);
			MojErrCatch(err, MojErrDbKindNotRegistered); // kind may not have been backed up
			MojErrCheck(err);
		} else {
           		err = putImpl(obj, flags, req, false /*checkSchema*/);
			MojErrCatch(err, MojErrDbKindNotRegistered); // kind may not have been backed up
			MojErrCheck(err);
		}
	}
	return MojErrNone;
}

MojErr MojDb::loadRestoreImpl(MojObject& obj, MojDbReq& req, RestoreStats& stats)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	// A backup dump is unauthenticated input from user-writable storage, and
	// the restore runs as admin. Unlike loadImpl, it therefore never touches
	// the schema (Kind:/Permission: records), never deletes, and never writes
	// over an object that already exists: kinds and permissions come from the
	// device's own configuration, and a restore only adds the user's objects
	// back into the kinds that are marked for backup.
	stats.records++;

	bool found = false;
	MojString kindName;
	MojErr err = obj.get(MojDb::KindKey, kindName, found);
	MojErrCheck(err);
	if (!found) {
		stats.invalid++;
		return MojErrNone;
	}
	if (kindName.startsWith(MojDbKindEngine::KindKindIdPrefix)) {
		stats.kinds++;
		return MojErrNone;
	}
	if (kindName.startsWith(MojDbKindEngine::PermissionIdPrefix)) {
		stats.permissions++;
		return MojErrNone;
	}
	// A tombstone can only take something away from this device. On a fresh
	// device there is nothing for it to delete; on a used one it would delete
	// whatever local object has that _id.
	bool deleted = false;
	if (obj.get(MojDb::DelKey, deleted) && deleted) {
		stats.tombstones++;
		return MojErrNone;
	}

	MojDbKind* kind = NULL;
	err = m_kindEngine.getKind(kindName.data(), kind);
	MojErrCatchNoLog(err, MojErrDbKindNotRegistered) {
		stats.unknownKind++;
		return MojErrNone;
	}
	MojErrCheck(err);
	MojAssert(kind);
	if (kind->isBuiltin() || !kind->backup()) {
		stats.noBackupKind++;
		return MojErrNone;
	}

	MojObject id;
	if (obj.get(MojDb::IdKey, id)) {
		MojString idStr;
		if (id.type() != MojObject::TypeString || id.stringValue(idStr) != MojErrNone || idStr.empty() ||
			idStr.startsWith(MojDb::KindIdPrefix) || idStr.startsWith(MojDb::PermissionIdPrefix) ||
			idStr.startsWith(MojDb::QuotaIdPrefix) || idStr.startsWith(MojDb::DbStateObjId)) {
			stats.invalid++;
			return MojErrNone;
		}

		MojDbShardId shardId;
		err = MojDbIdGenerator::extractShard(id, shardId);
		MojErrCheck(err);
		MojRefCountedPtr<MojDbStorageItem> prevItem;
		err = m_objDb->get(shardId, id, req.txn(), false, prevItem);
		MojErrCheck(err);
		if (prevItem.get()) {
			// Only a tombstone of the same kind may be brought back; a live
			// object, or one of any other kind, keeps what this device has.
			MojObject prev;
			err = prevItem->toObject(prev, m_kindEngine);
			MojErrCheck(err);
			err = prevItem->close();
			MojErrCheck(err);
			bool prevDeleted = false;
			MojString prevKind;
			bool prevKindFound = false;
			err = prev.get(MojDb::KindKey, prevKind, prevKindFound);
			MojErrCheck(err);
			if (!prev.get(MojDb::DelKey, prevDeleted) || !prevDeleted ||
				!prevKindFound || prevKind != kindName) {
				stats.existing++;
				return MojErrNone;
			}
		}
	}

	// No MojDbFlagForce: the dump carries no _rev, so without force putImpl
	// refuses to replace a live object. The checks above already skip those,
	// and dropping any _rev keeps a guessed revision from getting past them.
	found = false;
	err = obj.del(MojDb::RevKey, found);
	MojErrCheck(err);
	err = putImpl(obj, MojDbFlagRestore, req, false /*checkSchema*/);
	MojErrCheck(err);
	stats.restored++;

	return MojErrNone;
}

MojErr MojDb::purgeImpl(MojObject& obj, MojUInt32& countOut, MojDbReq& req)
{
    LOG_TRACE("Entering function %s", __FUNCTION__);

	MojObject val;
	MojErr err = obj.getRequired(RevNumKey, val);
	MojErrCheck(err);
	MojObject timestamp;
	err = obj.getRequired(TimestampKey, timestamp);
	MojErrCheck(err);

	// purge all objects that were deleted on or prior to this rev num

	// query for objects in two passes - once where backup is true and once where backup is false
	MojDbQuery objQuery;
	err = objQuery.from(MojDbKindEngine::RootKindId);
	MojErrCheck(err);
	err = objQuery.where(DelKey, MojDbQuery::OpEq, true);
	MojErrCheck(err);
	err = objQuery.where(SyncKey, MojDbQuery::OpEq, true);
	MojErrCheck(err);
	err = objQuery.where(RevKey, MojDbQuery::OpLessThanEq, val);
	MojErrCheck(err);

	MojUInt32 backupCount = 0;
	req.autobatch(true);
	req.fixmode(true);
	objQuery.limit(AutoBatchSize);
	err = delImpl(objQuery, backupCount, req, MojDbFlagPurge);
	MojErrCheck(err);

	MojDbQuery objQuery2;
	err = objQuery2.from(MojDbKindEngine::RootKindId);
	MojErrCheck(err);
	err = objQuery2.where(DelKey, MojDbQuery::OpEq, true);
	MojErrCheck(err);
	err = objQuery2.where(SyncKey, MojDbQuery::OpEq, false);
	MojErrCheck(err);
	err = objQuery2.where(RevKey, MojDbQuery::OpLessThanEq, val);
	MojErrCheck(err);
	MojUInt32 count = 0;
	MojUInt32 batchRemain = 0;
	if (backupCount <= AutoBatchSize)
		batchRemain = AutoBatchSize - backupCount;
	req.autobatch(true);		// enable auto batch
	req.fixmode(true);		// force deletion of bad entries

	if (batchRemain > 0) {
		objQuery2.limit(batchRemain);
		err = delImpl(objQuery2, count, req, MojDbFlagPurge);
		MojErrCheck(err);
	}

	countOut = count + backupCount;

	req.autobatch(false);

	// if we actually deleted objects, store this rev num as the last purge rev
	if (countOut > 0) {
		err = updateState(LastPurgedRevKey, val, req);
		MojErrCheck(err);
	}

	// delete all the RevTimestamp entries prior to this one
	MojDbQuery revTimestampQuery;
	err = revTimestampQuery.from(MojDbKindEngine::RevTimestampId);
	MojErrCheck(err);
	err = revTimestampQuery.where(TimestampKey, MojDbQuery::OpLessThanEq, timestamp);
	MojErrCheck(err);

	count = 0;
	err = delImpl(revTimestampQuery, count, req, MojDbFlagPurge);
	MojErrCheck(err);


	return MojErrNone;
}
