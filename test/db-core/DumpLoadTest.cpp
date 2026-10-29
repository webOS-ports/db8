// Copyright (c) 2013-2018 LG Electronics, Inc.
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
 *  @file DumpLoadTest.cpp
 ****************************************************************/

#include "gtest/gtest.h"

#include <set>
#include <string>

#include "db/MojDb.h"
#include "core/MojUtil.h"

#include "MojDbCoreTest.h"

static const MojChar* const MojLoadTestFileName = _T("loadtest.json");
static const MojChar* const MojDumpTestFileName = _T("dumptest.json");
static const MojChar* const MojTestStr =
	_T("{\"_id\":\"_kinds/LoadTest:1\",\"_kind\":\"Kind:1\",\"id\":\"LoadTest:1\",\"owner\":\"mojodb.admin\",")
	_T("\"indexes\":[{\"name\":\"foo\",\"props\":[{\"name\":\"foo\"}]},{\"name\":\"barfoo\",\"props\":[{\"name\":\"bar\"},{\"name\":\"foo\"}]}]}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}")
	_T("{\"_kind\":\"LoadTest:1\",\"foo\":\"hello\",\"bar\":\"world\"}");

struct DatabaseSuite : public MojDbCoreTest
{
    MojString kindId;

	MojString MojLoadTestFilePath;
	MojString MojDumpTestFilePath;

    void SetUp()
    {
        MojDbCoreTest::SetUp();
        MojAssertNoErr( kindId.assign(_T("LoadTest:1")) );

		MojAssertNoErr( MojLoadTestFilePath.format("%s/%s", tempFolder, MojLoadTestFileName) );
		MojAssertNoErr( MojDumpTestFilePath.format("%s/%s", tempFolder, MojDumpTestFileName) );
    }

    void TearDown()
    {
        MojDbCoreTest::TearDown();

        // not every test writes both files
        const MojChar* const paths[] = { MojLoadTestFilePath.data(), MojDumpTestFilePath.data() };
        for (const MojChar* path : paths) {
            const MojErr err = MojUnlink(path);
            if (err != MojErrNotFound) {
                MojExpectNoErr( err );
            }
        }
    }

    void checkCount()
    {
        MojDbQuery query;
        MojAssertNoErr( query.from(_T("LoadTest:1")) );

        MojDbCursor cursor;
        MojAssertNoErr( db.find(query, cursor) );

        MojUInt32 count = 0;
        MojAssertNoErr( cursor.count(count) );

        EXPECT_EQ( 10LL, count );
    }
};

TEST_F(DatabaseSuite, dump_and_load)
{

    // load
    MojAssertNoErr( MojFileFromString(MojLoadTestFilePath, MojTestStr) );

    MojUInt32 count = 0;
    MojAssertNoErr( db.load(MojLoadTestFilePath, count) );
    EXPECT_EQ( 11LL, count );

    checkCount();

    // dump
    count = 0;
    MojAssertNoErr( db.dump(MojDumpTestFilePath, count) );
    EXPECT_EQ( 11LL, count );

    EXPECT_TRUE( db.kindEngine() );

    // verify that we can get this kind
    MojDbKind *pk = 0;
    MojExpectNoErr( db.kindEngine()->getKind(kindId.data(), pk) );
    EXPECT_TRUE( pk );

    // del
    bool found = false;
    MojAssertNoErr( db.delKind(kindId, found) );
    EXPECT_TRUE( found );

    // verify that we can NOT get this kind
    pk = 0;
    EXPECT_EQ( MojErrDbKindNotRegistered, db.kindEngine()->getKind(kindId.data(), pk) );
    EXPECT_FALSE( pk );

    // purge deleted kinds
    MojAssertNoErr( db.purge(count, 0) );
    EXPECT_EQ( 1LL, count ) << "Purged kinds marked for delete";

    // load again. why 12?
    MojAssertNoErr( db.load(MojDumpTestFilePath, count) );
    EXPECT_EQ( 12LL, count );

    checkCount();

    // analyze
    MojObject analysis;
    MojAssertNoErr( db.stats(analysis) );
}

/**
 * A backup larger than maxBytes is paged: each page's response carries an
 * incrementalKey that the caller hands back for the next one, and
 * preBackup's schema refuses any rev in it below 1. Pages must fill in
 * both passes - live objects and tombstones - and every key along the way
 * must be one preBackup accepts, without writing any object twice.
 */
TEST_F(DatabaseSuite, backup_pages_carry_valid_incremental_keys)
{
    MojObject kind;
    MojAssertNoErr( kind.fromJson(_T("{\"id\":\"BackupKeyTest:1\",\"owner\":\"mojodb.admin\",\"sync\":true}")) );
    MojAssertNoErr( db.putKind(kind) );

    const int liveCount = 12;
    const int deletedCount = 12;
    MojObject ids[liveCount + deletedCount];
    for (int i = 0; i < liveCount + deletedCount; ++i) {
        MojObject obj;
        MojAssertNoErr( obj.fromJson(_T("{\"_kind\":\"BackupKeyTest:1\",\"pad\":\"0123456789012345678901234567890123456789\"}")) );
        MojAssertNoErr( obj.put(_T("n"), MojObject(i)) );
        MojAssertNoErr( db.put(obj) );
        MojAssertNoErr( obj.getRequired(MojDb::IdKey, ids[i]) );
    }
    for (int i = liveCount; i < liveCount + deletedCount; ++i) {
        bool found = false;
        MojAssertNoErr( db.del(ids[i], found) );
        ASSERT_TRUE( found );
    }

    std::set<MojInt64> liveSeen;
    std::set<std::string> deletedSeen;
    MojObject key;
    bool haveKey = false;
    int pages = 0;
    for (;;) {
        ASSERT_LT( pages, 100 ) << "backup did not terminate";
        ++pages;

        MojObject response;
        MojUInt32 count = 0;
        // Room for two or three objects a page, so both passes overflow.
        MojAssertNoErr( db.dump(MojDumpTestFilePath, count, true, MojDbReq(), true, 300,
                                haveKey ? &key : NULL, &response) );

        MojString dumped;
        MojAssertNoErr( MojFileToString(MojDumpTestFilePath, dumped) );
        const std::string text(dumped.data());
        std::string::size_type start = 0;
        while (start < text.size()) {
            std::string::size_type end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(start, end - start);
            start = end + 1;
            if (line.empty())
                continue;
            MojObject obj;
            MojAssertNoErr( obj.fromJson(line.c_str()) );
            MojString kindName;
            bool found = false;
            MojAssertNoErr( obj.get(MojDb::KindKey, kindName, found) );
            if (!found || kindName != _T("BackupKeyTest:1"))
                continue;
            bool deleted = false;
            obj.get(MojDb::DelKey, deleted);
            MojString id;
            MojAssertNoErr( obj.getRequired(MojDb::IdKey, id) );
            if (deleted) {
                EXPECT_TRUE( deletedSeen.insert(id.data()).second ) << "tombstone dumped twice: " << id.data();
            } else {
                MojInt64 n = -1;
                obj.get(_T("n"), n);
                EXPECT_TRUE( liveSeen.insert(n).second ) << "live object dumped twice: " << n;
            }
        }

        bool hasMore = false;
        response.get(_T("hasMore"), hasMore);
        if (!hasMore)
            break;

        ASSERT_TRUE( response.get(_T("incrementalKey"), key) );
        haveKey = true;
        const MojChar* const revKeys[] = { _T("rev"), _T("deletedRev") };
        for (const MojChar* revKey : revKeys) {
            MojObject rev;
            if (key.get(revKey, rev)) {
                EXPECT_GE( rev.intValue(), 1 ) << "page " << pages << " hands back "
                    << revKey << "=" << rev.intValue() << ", which preBackup's schema refuses";
            }
        }
    }

    EXPECT_GT( pages, 2 ) << "the test needs several pages to mean anything";
    EXPECT_EQ( static_cast<size_t>(liveCount), liveSeen.size() );
    EXPECT_EQ( static_cast<size_t>(deletedCount), deletedSeen.size() );
}
