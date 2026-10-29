// Copyright (c) 2026 webOS-ports project
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

/****************************************************************
 *  @file RestoreTest.cpp
 *  MojDb::restore (internal/postRestore) with crafted backup dumps
 ****************************************************************/

#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "gtest/gtest.h"

#include "db/MojDb.h"
#include "db/MojDbKind.h"
#include "db/MojDbKindEngine.h"
#include "db/MojDbPermissionEngine.h"
#include "core/MojUtil.h"

#include "MojDbCoreTest.h"

namespace {

const MojChar* const BackupKindJson =
	_T("{\"id\":\"RestoreTest:1\",\"owner\":\"mojodb.admin\",\"sync\":true,")
	_T("\"indexes\":[{\"name\":\"foo\",\"props\":[{\"name\":\"foo\"}]}]}");
const MojChar* const NoBackupKindJson =
	_T("{\"id\":\"RestoreNoBackup:1\",\"owner\":\"mojodb.admin\",")
	_T("\"indexes\":[{\"name\":\"foo\",\"props\":[{\"name\":\"foo\"}]}]}");

struct RestoreSuite : public MojDbCoreTest
{
	std::string dir;
	MojString existingId;     // live object of RestoreTest:1
	MojString otherKindId;    // live object of RestoreNoBackup:1

	void SetUp()
	{
		MojDbCoreTest::SetUp();
		dir = path + "-dumps";
		ASSERT_EQ(0, mkdir(dir.c_str(), 0700));

		MojObject kind;
		MojAssertNoErr( kind.fromJson(BackupKindJson) );
		MojAssertNoErr( db.putKind(kind) );
		MojObject kind2;
		MojAssertNoErr( kind2.fromJson(NoBackupKindJson) );
		MojAssertNoErr( db.putKind(kind2) );

		MojObject obj;
		MojAssertNoErr( obj.fromJson(_T("{\"_kind\":\"RestoreTest:1\",\"foo\":\"local\"}")) );
		MojAssertNoErr( db.put(obj) );
		MojAssertNoErr( obj.getRequired(MojDb::IdKey, existingId) );

		MojObject obj2;
		MojAssertNoErr( obj2.fromJson(_T("{\"_kind\":\"RestoreNoBackup:1\",\"foo\":\"local\"}")) );
		MojAssertNoErr( db.put(obj2) );
		MojAssertNoErr( obj2.getRequired(MojDb::IdKey, otherKindId) );
	}

	void TearDown()
	{
		MojExpectNoErr( MojRmDirRecursive(dir.c_str()) );
		MojDbCoreTest::TearDown();
	}

	void writeDump(const char* name, const std::string& content)
	{
		std::string file = dir + "/" + name;
		MojAssertNoErr( MojFileFromString(file.c_str(), content.c_str()) );
	}

	MojErr restoreFiles(std::initializer_list<const char*> names, MojDb::RestoreStats& stats)
	{
		MojObject files(MojObject::TypeArray);
		for (const char* n : names) {
			MojErr err = files.pushString(n);
			MojErrCheck(err);
		}
		return db.restore(dir.c_str(), files, stats);
	}

	MojUInt32 count(const MojChar* kindId, const MojChar* foo = NULL)
	{
		MojDbQuery query;
		if (query.from(kindId) != MojErrNone)
			return 9999;
		if (foo) {
			MojString fooStr;
			if (fooStr.assign(foo) != MojErrNone)
				return 9999;
			MojObject val(fooStr);
			if (query.where(_T("foo"), MojDbQuery::OpEq, val) != MojErrNone)
				return 9999;
		}
		MojDbCursor cursor;
		if (db.find(query, cursor) != MojErrNone)
			return 9999;
		MojUInt32 n = 0;
		if (cursor.count(n) != MojErrNone)
			return 9999;
		(void) cursor.close();
		return n;
	}

	std::string fooOf(const MojString& id)
	{
		MojObject idObj(id);
		MojObject obj;
		bool found = false;
		if (db.get(idObj, obj, found) != MojErrNone || !found)
			return "<missing>";
		bool deleted = false;
		if (obj.get(MojDb::DelKey, deleted) && deleted)
			return "<deleted>";
		MojString foo;
		if (obj.getRequired(_T("foo"), foo) != MojErrNone)
			return "<no foo>";
		return std::string(foo.data());
	}

	static std::string q(const MojString& s) { return std::string("\"") + s.data() + "\""; }
};

} // namespace

// A legitimate dump: objects of a registered, backup-enabled kind come back,
// with or without an _id of their own.
TEST_F(RestoreSuite, restoresObjectsOfBackupKind)
{
	MojObject newId;
	MojAssertNoErr( db.reserveId(newId) );
	MojString newIdStr;
	MojAssertNoErr( newId.stringValue(newIdStr) );

	writeDump("backup-1.json",
		"{\"_id\":" + q(newIdStr) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\",\"_sync\":true}\n"
		"{\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(2u, stats.records);
	EXPECT_EQ(2u, stats.restored);
	EXPECT_EQ(2u, count(_T("RestoreTest:1"), _T("restored")));
	EXPECT_EQ("restored", fooOf(newIdStr));
	EXPECT_EQ("local", fooOf(existingId));
}

// Kind: records are never applied: not a new kind, not a changed one, not a
// deleted one. Objects of a kind that only the dump defined are skipped.
TEST_F(RestoreSuite, skipsKindRecords)
{
	writeDump("backup-1.json",
		"{\"_id\":\"_kinds/RestoreEvil:1\",\"_kind\":\"Kind:1\",\"id\":\"RestoreEvil:1\",\"owner\":\"mojodb.admin\",\"sync\":true}\n"
		"{\"_id\":\"_kinds/RestoreTest:1\",\"_kind\":\"Kind:1\",\"id\":\"RestoreTest:1\",\"owner\":\"com.evil\"}\n"
		"{\"_id\":\"_kinds/RestoreNoBackup:1\",\"_kind\":\"Kind:1\",\"id\":\"RestoreNoBackup:1\",\"_del\":true}\n"
		"{\"_kind\":\"RestoreEvil:1\",\"foo\":\"evil\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(3u, stats.kinds);
	EXPECT_EQ(1u, stats.unknownKind);
	EXPECT_EQ(0u, stats.restored);

	MojDbKind* kind = NULL;
	EXPECT_EQ(MojErrDbKindNotRegistered, db.kindEngine()->getKind(_T("RestoreEvil:1"), kind));
	kind = NULL;
	MojAssertNoErr( db.kindEngine()->getKind(_T("RestoreTest:1"), kind) );
	ASSERT_TRUE(kind);
	EXPECT_STREQ("mojodb.admin", kind->owner().data());
	EXPECT_TRUE(kind->backup());
	kind = NULL;
	MojExpectNoErr( db.kindEngine()->getKind(_T("RestoreNoBackup:1"), kind) );
	EXPECT_TRUE(kind);
}

// Permission: records are never applied, so a dump cannot grant an app
// access to another app's kind.
TEST_F(RestoreSuite, skipsPermissionRecords)
{
	ASSERT_NE(MojDbPermissionEngine::ValueAllow,
		db.permissionEngine()->check(_T("db.kind"), _T("RestoreTest:1"), _T("com.evil"), _T("read")));

	writeDump("backup-1.json",
		"{\"_kind\":\"Permission:1\",\"type\":\"db.kind\",\"object\":\"RestoreTest:1\",\"caller\":\"com.evil\","
		"\"operations\":{\"create\":\"allow\",\"read\":\"allow\",\"update\":\"allow\",\"delete\":\"allow\"}}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(1u, stats.permissions);
	EXPECT_EQ(0u, stats.restored);
	EXPECT_NE(MojDbPermissionEngine::ValueAllow,
		db.permissionEngine()->check(_T("db.kind"), _T("RestoreTest:1"), _T("com.evil"), _T("read")));
}

// Objects of a kind that is not registered here are skipped, not an error.
TEST_F(RestoreSuite, skipsUnregisteredKind)
{
	writeDump("backup-1.json",
		"{\"_kind\":\"RestoreUnknown:1\",\"foo\":\"x\"}\n"
		"{\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(1u, stats.unknownKind);
	EXPECT_EQ(1u, stats.restored);
	EXPECT_EQ(1u, count(_T("RestoreTest:1"), _T("restored")));
}

// Objects of a kind without the backup flag, or of a built-in kind, are
// skipped: preBackup never dumps those, so a dump that has them is crafted.
TEST_F(RestoreSuite, skipsNonBackupKind)
{
	writeDump("backup-1.json",
		"{\"_kind\":\"RestoreNoBackup:1\",\"foo\":\"crafted\"}\n"
		"{\"_kind\":\"Quota:1\",\"owner\":\"com.evil\",\"size\":1000000000}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(2u, stats.noBackupKind);
	EXPECT_EQ(0u, stats.restored);
	EXPECT_EQ(0u, count(_T("RestoreNoBackup:1"), _T("crafted")));
	EXPECT_EQ(0u, count(_T("Quota:1")));
}

// A tombstone does not delete the device's object with that _id. The old
// load path deleted by the record's "id" property, so carry both.
TEST_F(RestoreSuite, tombstoneDoesNotDeleteExisting)
{
	writeDump("backup-1.json",
		"{\"_id\":" + q(existingId) + ",\"id\":" + q(existingId) + ",\"_kind\":\"RestoreTest:1\",\"_del\":true}\n"
		"{\"_id\":" + q(otherKindId) + ",\"id\":" + q(otherKindId) + ",\"_kind\":\"RestoreTest:1\",\"_del\":true}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(2u, stats.tombstones);
	EXPECT_EQ("local", fooOf(existingId));
	EXPECT_EQ("local", fooOf(otherKindId));
}

// A record cannot replace an existing object, not with the matching _rev,
// and not under a different (backup-enabled) _kind.
TEST_F(RestoreSuite, doesNotOverwriteExisting)
{
	MojObject idObj(existingId);
	MojObject cur;
	bool found = false;
	MojAssertNoErr( db.get(idObj, cur, found) );
	ASSERT_TRUE(found);
	MojInt64 rev = 0;
	MojAssertNoErr( cur.getRequired(MojDb::RevKey, rev) );

	writeDump("backup-1.json",
		"{\"_id\":" + q(existingId) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"_id\":" + q(existingId) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\",\"_rev\":" + std::to_string(rev) + "}\n"
		"{\"_id\":" + q(otherKindId) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(3u, stats.existing);
	EXPECT_EQ(0u, stats.restored);
	EXPECT_EQ("local", fooOf(existingId));
	EXPECT_EQ("local", fooOf(otherKindId));
	EXPECT_EQ(0u, count(_T("RestoreTest:1"), _T("evil")));
}

// A tombstone left on this device by the same kind may be brought back.
TEST_F(RestoreSuite, restoresOverOwnTombstone)
{
	MojObject idObj(existingId);
	bool found = false;
	MojAssertNoErr( db.del(idObj, found, MojDbFlagForceNoPurge) );
	ASSERT_TRUE(found);
	ASSERT_EQ("<deleted>", fooOf(existingId));

	writeDump("backup-1.json",
		"{\"_id\":" + q(existingId) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(1u, stats.restored);
	EXPECT_EQ("restored", fooOf(existingId));
}

// A tombstone of another kind keeps its _id: a backup-kind record cannot
// take over an id that a non-backup kind used.
TEST_F(RestoreSuite, tombstoneOfOtherKindNotReused)
{
	MojObject idObj(otherKindId);
	bool found = false;
	MojAssertNoErr( db.del(idObj, found, MojDbFlagForceNoPurge) );
	ASSERT_TRUE(found);

	writeDump("backup-1.json",
		"{\"_id\":" + q(otherKindId) + ",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(1u, stats.existing);
	EXPECT_EQ(0u, stats.restored);
	EXPECT_EQ(0u, count(_T("RestoreTest:1"), _T("evil")));
}

// Internal ids (kinds, permissions, quotas, db state) are not object ids.
TEST_F(RestoreSuite, skipsReservedIds)
{
	writeDump("backup-1.json",
		"{\"_id\":\"_kinds/RestoreLater:1\",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"_id\":\"_permissions/x\",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"_id\":\"_quotas/x\",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"_id\":\"_internal/dbstate\",\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"_id\":42,\"_kind\":\"RestoreTest:1\",\"foo\":\"evil\"}\n"
		"{\"foo\":\"no kind\"}\n");

	MojDb::RestoreStats stats;
	MojAssertNoErr( restoreFiles({"backup-1.json"}, stats) );
	EXPECT_EQ(6u, stats.invalid);
	EXPECT_EQ(0u, stats.restored);
	EXPECT_EQ(0u, count(_T("RestoreTest:1"), _T("evil")));
}

// File names are bare names inside tempDir; a bad one rejects the whole
// call before anything is loaded.
TEST_F(RestoreSuite, rejectsBadFileNames)
{
	writeDump("good.json", "{\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");
	std::string sub = dir + "/a";
	ASSERT_EQ(0, mkdir(sub.c_str(), 0700));
	writeDump("a/b", "{\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");

	const char* const bad[] = {"../x", "a/b", "", ".", "..", "/etc/passwd"};
	for (const char* name : bad) {
		MojDb::RestoreStats stats;
		EXPECT_EQ(MojErrInvalidArg, restoreFiles({"good.json", name}, stats)) << "'" << name << "'";
		EXPECT_EQ(0u, stats.records) << "'" << name << "'";
	}

	// an embedded NUL
	MojString nul;
	MojAssertNoErr( nul.assign(_T("good.json\0x"), 11) );
	MojObject files(MojObject::TypeArray);
	ASSERT_EQ(11u, nul.length());
	MojAssertNoErr( files.push(MojObject(nul)) );
	MojDb::RestoreStats stats;
	EXPECT_EQ(MojErrInvalidArg, db.restore(dir.c_str(), files, stats));

	// a relative tempDir
	MojObject files2(MojObject::TypeArray);
	MojAssertNoErr( files2.pushString(_T("good.json")) );
	EXPECT_EQ(MojErrInvalidArg, db.restore(_T("relative/dir"), files2, stats));

	EXPECT_EQ(0u, count(_T("RestoreTest:1"), _T("restored")));
}

// The dump itself must be a regular file, not a symlink to one or a FIFO.
TEST_F(RestoreSuite, rejectsSymlinkAndFifo)
{
	writeDump("real.json", "{\"_kind\":\"RestoreTest:1\",\"foo\":\"restored\"}\n");
	std::string linkPath = dir + "/link.json";
	ASSERT_EQ(0, symlink((dir + "/real.json").c_str(), linkPath.c_str()));
	std::string fifo = dir + "/fifo.json";
	ASSERT_EQ(0, mkfifo(fifo.c_str(), 0600));

	MojDb::RestoreStats stats;
	EXPECT_NE(MojErrNone, restoreFiles({"link.json"}, stats));
	EXPECT_NE(MojErrNone, restoreFiles({"fifo.json"}, stats));
	EXPECT_EQ(0u, count(_T("RestoreTest:1"), _T("restored")));

	MojAssertNoErr( restoreFiles({"real.json"}, stats) );
	EXPECT_EQ(1u, count(_T("RestoreTest:1"), _T("restored")));
}

// The plain load() keeps its behaviour: Kind: records are applied.
TEST_F(RestoreSuite, plainLoadUnchanged)
{
	writeDump("load.json",
		"{\"_id\":\"_kinds/RestoreLoad:1\",\"_kind\":\"Kind:1\",\"id\":\"RestoreLoad:1\",\"owner\":\"mojodb.admin\"}\n"
		"{\"_kind\":\"RestoreLoad:1\",\"foo\":\"x\"}\n");
	MojUInt32 n = 0;
	MojAssertNoErr( db.load((dir + "/load.json").c_str(), n) );
	EXPECT_EQ(2u, n);
	EXPECT_EQ(1u, count(_T("RestoreLoad:1")));
}
