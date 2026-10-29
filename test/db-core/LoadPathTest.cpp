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
 *  @file LoadPathTest.cpp
 *  MojDb::loadChecked (the bus "load" method) and its path rules
 ****************************************************************/

#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "gtest/gtest.h"

#include "db/MojDb.h"
#include "db/MojDbKindEngine.h"
#include "core/MojUtil.h"

#include "MojDbCoreTest.h"

namespace {

const char* const Dump =
	"{\"_id\":\"_kinds/LoadPath:1\",\"_kind\":\"Kind:1\",\"id\":\"LoadPath:1\",\"owner\":\"mojodb.admin\"}\n"
	"{\"_kind\":\"LoadPath:1\",\"foo\":\"x\"}\n";

struct LoadPathSuite : public MojDbCoreTest
{
	std::string base;      // <test dir>-load
	std::string allowed;   // base/allowed: the one load directory
	const MojChar* dirs[1] = {NULL};

	void SetUp()
	{
		MojDbCoreTest::SetUp();
		base = path + "-load";
		allowed = base + "/allowed";
		ASSERT_EQ(0, mkdir(base.c_str(), 0700));
		ASSERT_EQ(0, mkdir(allowed.c_str(), 0700));
		dirs[0] = allowed.c_str();
		db.setLoadDirs(dirs, 1);
	}

	void TearDown()
	{
		MojExpectNoErr( MojRmDirRecursive(base.c_str()) );
		MojDbCoreTest::TearDown();
	}

	void writeFile(const std::string& file)
	{
		MojAssertNoErr( MojFileFromString(file.c_str(), Dump) );
	}

	MojErr loadPath(const std::string& p)
	{
		MojString s;
		MojErr err = s.assign(p.data(), p.length());
		MojErrCheck(err);
		MojUInt32 n = 0;
		return db.loadChecked(s, n);
	}

	bool kindLoaded()
	{
		MojDbKind* kind = NULL;
		return db.kindEngine()->getKind(_T("LoadPath:1"), kind) == MojErrNone && kind;
	}
};

} // namespace

// The service's load method reads only from the backup temp directories.
TEST(LoadPathDefaults, onlyBackupTempDirs)
{
	ASSERT_EQ(2u, MojDb::DefaultLoadDirCount);
	EXPECT_STREQ("/var/file-cache/backuptempdir", MojDb::DefaultLoadDirs[0]);
	EXPECT_STREQ("/media/internal/.woce-backup/tmp", MojDb::DefaultLoadDirs[1]);
}

// A file below a load directory, also in a subdirectory, is loaded.
TEST_F(LoadPathSuite, loadsBelowAllowedDir)
{
	ASSERT_EQ(0, mkdir((allowed + "/sub").c_str(), 0700));
	writeFile(allowed + "/sub/load.json");
	MojAssertNoErr( loadPath(allowed + "/sub/load.json") );
	EXPECT_TRUE(kindLoaded());
}

// Outside the load directories, including a sibling sharing the prefix and
// the directory itself, nothing is read.
TEST_F(LoadPathSuite, refusesOutsideAllowedDirs)
{
	writeFile(base + "/load.json");
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(base + "/load.json"));
	ASSERT_EQ(0, mkdir((allowed + "-evil").c_str(), 0700));
	writeFile(allowed + "-evil/load.json");
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "-evil/load.json"));
	EXPECT_EQ(MojErrDbAccessDenied, loadPath("/etc/passwd"));
	EXPECT_NE(MojErrNone, loadPath(allowed));

	// a fresh MojDb uses the defaults, which a test directory is not under
	MojDb other;
	MojString s;
	MojAssertNoErr( s.assign((base + "/load.json").c_str()) );
	MojUInt32 n = 0;
	EXPECT_EQ(MojErrDbAccessDenied, other.loadChecked(s, n));
	// no load directories at all is not "anything goes"
	db.setLoadDirs(NULL, 0);
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(base + "/load.json"));
	EXPECT_FALSE(kindLoaded());
}

// Malformed paths are refused before anything is opened.
TEST_F(LoadPathSuite, refusesMalformedPaths)
{
	writeFile(allowed + "/load.json");
	const std::string bad[] = {
		"", "load.json", "relative/" + allowed.substr(1) + "/load.json",
		allowed + "/../allowed/load.json", allowed + "/./load.json",
		allowed + "//load.json", allowed + "/load.json/",
		allowed + std::string("/load.json\0x", 12),
	};
	for (const std::string& p : bad)
		EXPECT_EQ(MojErrInvalidArg, loadPath(p)) << "'" << p.c_str() << "'";
	EXPECT_FALSE(kindLoaded());
}

// No symbolic link anywhere: not the file, not a directory below the load
// directory, not in the load directory's own path.
TEST_F(LoadPathSuite, refusesSymlinks)
{
	writeFile(base + "/real.json");
	ASSERT_EQ(0, symlink((base + "/real.json").c_str(), (allowed + "/link.json").c_str()));
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/link.json"));

	ASSERT_EQ(0, mkdir((base + "/elsewhere").c_str(), 0700));
	writeFile(base + "/elsewhere/load.json");
	ASSERT_EQ(0, symlink((base + "/elsewhere").c_str(), (allowed + "/sub").c_str()));
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/sub/load.json"));

	std::string viaLink = base + "/alias";
	ASSERT_EQ(0, symlink(allowed.c_str(), viaLink.c_str()));
	writeFile(allowed + "/load.json");
	dirs[0] = viaLink.c_str();
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(viaLink + "/load.json"));
	EXPECT_FALSE(kindLoaded());
}

// Only a regular file: a FIFO or a directory is refused without blocking.
TEST_F(LoadPathSuite, refusesNonRegularFiles)
{
	ASSERT_EQ(0, mkfifo((allowed + "/fifo.json").c_str(), 0600));
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/fifo.json"));
	ASSERT_EQ(0, mkdir((allowed + "/dir.json").c_str(), 0700));
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/dir.json"));
	EXPECT_NE(MojErrNone, loadPath(allowed + "/missing.json"));
}

// A restore walks the path the same way: a symbolic link in a directory
// component of tempDir is refused too.
TEST_F(LoadPathSuite, restoreRefusesSymlinkedDir)
{
	writeFile(allowed + "/backup.json");
	std::string viaLink = base + "/alias";
	ASSERT_EQ(0, symlink(allowed.c_str(), viaLink.c_str()));
	MojObject files(MojObject::TypeArray);
	MojAssertNoErr( files.pushString(_T("backup.json")) );
	MojDb::RestoreStats stats;
	EXPECT_EQ(MojErrDbAccessDenied, db.restore(viaLink.c_str(), files, stats));
	EXPECT_EQ(0u, stats.records);
	MojAssertNoErr( db.restore(allowed.c_str(), files, stats) );
	EXPECT_EQ(2u, stats.records);
}

// The plain MojDb::load() (tool/dbgen, tests) keeps its behaviour.
TEST_F(LoadPathSuite, plainLoadUnrestricted)
{
	writeFile(base + "/load.json");
	MojUInt32 n = 0;
	MojAssertNoErr( db.load((base + "/load.json").c_str(), n) );
	EXPECT_TRUE(kindLoaded());
}
