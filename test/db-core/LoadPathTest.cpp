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
 *  MojDb::loadChecked (the bus "load" method) and its path rules:
 *  the backup temp directories and the system configuration directories
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

// The service's load method reads from the backup temp directories and,
// with stricter rules, from /etc/palm.
TEST(LoadPathDefaults, onlyBackupTempDirs)
{
	ASSERT_EQ(2u, MojDb::DefaultLoadDirCount);
	EXPECT_STREQ("/var/file-cache/backuptempdir", MojDb::DefaultLoadDirs[0]);
	EXPECT_STREQ("/media/internal/.woce-backup/tmp", MojDb::DefaultLoadDirs[1]);
	ASSERT_EQ(1u, MojDb::SystemLoadDirCount);
	EXPECT_STREQ("/etc/palm", MojDb::SystemLoadDirs[0]);
}

// The ownership and mode rules themselves: root's (or the given owner's),
// writable by nobody else; a sticky directory may be; a file must be
// world-readable.
TEST(LoadPathDefaults, systemOwnershipRules)
{
	EXPECT_TRUE(MojDb::trustedLoadDir(0, S_IFDIR | 0755, 0));
	EXPECT_TRUE(MojDb::trustedLoadDir(0, S_IFDIR | 0700, 0));
	EXPECT_TRUE(MojDb::trustedLoadDir(0, S_IFDIR | 01777, 0));
	EXPECT_TRUE(MojDb::trustedLoadDir(1000, S_IFDIR | 0755, 1000));
	EXPECT_FALSE(MojDb::trustedLoadDir(1000, S_IFDIR | 0755, 0));
	EXPECT_FALSE(MojDb::trustedLoadDir(1000, S_IFDIR | 01777, 0));
	EXPECT_FALSE(MojDb::trustedLoadDir(0, S_IFDIR | 0775, 0));
	EXPECT_FALSE(MojDb::trustedLoadDir(0, S_IFDIR | 0757, 0));

	EXPECT_TRUE(MojDb::trustedLoadFile(0, S_IFREG | 0644, 0));
	EXPECT_TRUE(MojDb::trustedLoadFile(0, S_IFREG | 0444, 0));
	EXPECT_TRUE(MojDb::trustedLoadFile(1000, S_IFREG | 0644, 1000));
	EXPECT_FALSE(MojDb::trustedLoadFile(1000, S_IFREG | 0644, 0));
	EXPECT_FALSE(MojDb::trustedLoadFile(0, S_IFREG | 0664, 0));
	EXPECT_FALSE(MojDb::trustedLoadFile(0, S_IFREG | 0646, 0));
	EXPECT_FALSE(MojDb::trustedLoadFile(0, S_IFREG | 01646, 0));
	EXPECT_FALSE(MojDb::trustedLoadFile(0, S_IFREG | 0640, 0));
	EXPECT_FALSE(MojDb::trustedLoadFile(0, S_IFREG | 0600, 0));
}

// SettingsService's own call, load {"path":"/etc/palm/defaultSettings.json"},
// is not refused for its path (here the file is missing, or the MojDb is not
// open, but it gets past the path rules), and /etc next to it is.
TEST(LoadPathDefaults, settingsServicePathAllowed)
{
	MojDb fresh;
	MojString s;
	MojAssertNoErr( s.assign(_T("/etc/palm/defaultSettings.json")) );
	MojUInt32 n = 0;
	MojErr err = fresh.loadChecked(s, n);
	EXPECT_NE(MojErrNone, err);
	EXPECT_NE(MojErrDbAccessDenied, err);
	EXPECT_NE(MojErrInvalidArg, err);
	MojAssertNoErr( s.assign(_T("/etc/palmx/defaultSettings.json")) );
	EXPECT_EQ(MojErrDbAccessDenied, fresh.loadChecked(s, n));
	MojAssertNoErr( s.assign(_T("/etc/passwd")) );
	EXPECT_EQ(MojErrDbAccessDenied, fresh.loadChecked(s, n));
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

namespace {

// Two records of SettingsService's /etc/palm/defaultSettings.json, and its kind
// (settingsservice /etc/palm/db/kinds/com.webos.settings.default, indexes left out).
const char* const SettingsKind =
	"{\"id\":\"com.webos.settings.default:1\",\"owner\":\"com.webos.settingsservice\",\"sync\":true}";
const char* const SettingsDefaults =
	"{\"_kind\":\"com.webos.settings.default:1\",\"app_id\":\"\",\"category\":\"\","
	"\"value\":{\"localeInfo\":{\"clock\":\"locale\",\"keyboards\":[\"en\"],\"timezone\":\"\"}}}\n"
	"{\"_kind\":\"com.webos.settings.default:1\",\"app_id\":\"\",\"category\":\"option\","
	"\"value\":{\"country\":\"none\"}}\n";

// A stand-in for /etc/palm below the test directory. Root does not own it,
// so the test's own uid is the trusted owner; /tmp above it is sticky.
struct SystemLoadSuite : public LoadPathSuite
{
	std::string etc;    // base/etc
	std::string palm;   // base/etc/palm: the system load directory
	const MojChar* sysDirs[1] = {NULL};

	void SetUp()
	{
		LoadPathSuite::SetUp();
		etc = base + "/etc";
		palm = etc + "/palm";
		ASSERT_EQ(0, mkdir(etc.c_str(), 0755));
		ASSERT_EQ(0, mkdir(palm.c_str(), 0755));
		sysDirs[0] = palm.c_str();
		db.setSystemLoadDirs(sysDirs, 1, getuid());
		MojObject kind;
		MojAssertNoErr( kind.fromJson(SettingsKind) );
		MojAssertNoErr( db.putKind(kind) );
	}

	void writeSystemFile(const std::string& file, const char* content, mode_t mode = 0644)
	{
		MojAssertNoErr( MojFileFromString(file.c_str(), content) );
		ASSERT_EQ(0, chmod(file.c_str(), mode));
	}

	MojErr loadMerge(const std::string& p, MojUInt32& n)
	{
		MojString s;
		MojErr err = s.assign(p.data(), p.length());
		MojErrCheck(err);
		n = 0;
		return db.loadChecked(s, n, MojDbFlagMerge);
	}
};

} // namespace

// SettingsService's defaults, root's and read-only to others, load as before.
TEST_F(SystemLoadSuite, loadsSettingsServiceDefaults)
{
	writeSystemFile(palm + "/defaultSettings.json", SettingsDefaults);
	MojUInt32 n = 0;
	MojAssertNoErr( loadMerge(palm + "/defaultSettings.json", n) );
	EXPECT_EQ(2u, n);
	ASSERT_EQ(0, mkdir((palm + "/settings").c_str(), 0755));
	writeSystemFile(palm + "/settings/more.json", SettingsDefaults, 0444);
	MojAssertNoErr( loadMerge(palm + "/settings/more.json", n) );
	EXPECT_EQ(2u, n);
	// a directory on the way may be world-writable if it is sticky, like /tmp
	ASSERT_EQ(0, chmod(base.c_str(), 01777));
	MojAssertNoErr( loadMerge(palm + "/defaultSettings.json", n) );
	EXPECT_EQ(2u, n);
}

// A file others can write, or cannot read, is refused.
TEST_F(SystemLoadSuite, refusesWritableSystemFiles)
{
	const mode_t bad[] = {0664, 0646, 0666, 0640, 0600};
	for (mode_t m : bad) {
		writeSystemFile(palm + "/defaultSettings.json", SettingsDefaults, m);
		MojUInt32 n = 0;
		EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n)) << std::oct << m;
		EXPECT_EQ(0u, n);
	}
}

// So is one in, or below, a directory others can write: the load directory,
// one above it, or one below it.
TEST_F(SystemLoadSuite, refusesWritableSystemDirs)
{
	writeSystemFile(palm + "/defaultSettings.json", SettingsDefaults);
	MojUInt32 n = 0;
	const std::string dirsOnTheWay[] = {palm, etc, base};
	for (const std::string& d : dirsOnTheWay) {
		const mode_t bad[] = {0775, 0757};
		for (mode_t m : bad) {
			ASSERT_EQ(0, chmod(d.c_str(), m));
			EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n)) << d.c_str() << " " << std::oct << m;
		}
		ASSERT_EQ(0, chmod(d.c_str(), 0755));
	}
	ASSERT_EQ(0, mkdir((palm + "/sub").c_str(), 0777));
	ASSERT_EQ(0, chmod((palm + "/sub").c_str(), 0777));
	writeSystemFile(palm + "/sub/load.json", SettingsDefaults);
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/sub/load.json", n));
	MojAssertNoErr( loadMerge(palm + "/defaultSettings.json", n) );
}

// Files of anyone but root (here: the trusted test owner) are refused.
TEST_F(SystemLoadSuite, refusesOtherOwners)
{
	writeSystemFile(palm + "/defaultSettings.json", SettingsDefaults);
	db.setSystemLoadDirs(sysDirs, 1, getuid() + 1);
	MojUInt32 n = 0;
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n));
	// root only, the default
	db.setSystemLoadDirs(sysDirs, 1);
	if (getuid() != 0) {
		EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n));
	}
}

// No symbolic link, not as the file and not as a directory, even when it
// points at a file the rules would accept.
TEST_F(SystemLoadSuite, refusesSymlinkedSystemFiles)
{
	writeSystemFile(palm + "/real.json", SettingsDefaults);
	ASSERT_EQ(0, symlink((palm + "/real.json").c_str(), (palm + "/defaultSettings.json").c_str()));
	MojUInt32 n = 0;
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n));
	ASSERT_EQ(0, mkdir((etc + "/other").c_str(), 0755));
	writeSystemFile(etc + "/other/load.json", SettingsDefaults);
	ASSERT_EQ(0, symlink((etc + "/other").c_str(), (palm + "/sub").c_str()));
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/sub/load.json", n));
	EXPECT_EQ(0u, n);
	ASSERT_EQ(0, mkfifo((palm + "/fifo.json").c_str(), 0644));
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/fifo.json", n));
}

// The system rules are only for the system directories: a temp-dir file
// keeps the temp-dir rules, outside both is refused, and with no system
// directories /etc/palm is outside too.
TEST_F(SystemLoadSuite, rulesStaySeparate)
{
	writeFile(allowed + "/load.json");   // 0600, fine in a temp directory
	MojAssertNoErr( loadPath(allowed + "/load.json") );
	writeSystemFile(etc + "/load.json", SettingsDefaults);
	MojUInt32 n = 0;
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(etc + "/load.json", n));
	EXPECT_NE(MojErrNone, loadMerge(palm, n));
	writeSystemFile(palm + "/defaultSettings.json", SettingsDefaults);
	EXPECT_EQ(MojErrInvalidArg, loadMerge(palm + "/../palm/defaultSettings.json", n));
	db.setSystemLoadDirs(NULL, 0);
	EXPECT_EQ(MojErrDbAccessDenied, loadMerge(palm + "/defaultSettings.json", n));
	db.setLoadDirs(NULL, 0);
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/load.json"));
	// a system directory alone still restricts the temp-dir paths
	db.setSystemLoadDirs(sysDirs, 1, getuid());
	EXPECT_EQ(MojErrDbAccessDenied, loadPath(allowed + "/load.json"));
	MojAssertNoErr( loadMerge(palm + "/defaultSettings.json", n) );
}

// A restore keeps its own rules wherever the dump is: Kind:, Permission: and
// tombstone records are not applied (more in RestoreSuite).
TEST_F(SystemLoadSuite, restoreRulesUnchanged)
{
	writeSystemFile(palm + "/backup.json",
		"{\"_id\":\"_kinds/LoadPath:1\",\"_kind\":\"Kind:1\",\"id\":\"LoadPath:1\",\"owner\":\"mojodb.admin\",\"sync\":true}\n"
		"{\"_kind\":\"Permission:1\",\"object\":\"com.webos.settings.default:1\",\"type\":\"db.kind\",\"caller\":\"com.evil\",\"operations\":{\"read\":\"allow\"}}\n"
		"{\"_id\":\"settings-1\",\"_kind\":\"com.webos.settings.default:1\",\"id\":\"settings-1\",\"_del\":true}\n"
		"{\"_kind\":\"com.webos.settings.default:1\",\"app_id\":\"\",\"category\":\"r\",\"value\":{}}\n");
	MojObject files(MojObject::TypeArray);
	MojAssertNoErr( files.pushString(_T("backup.json")) );
	MojDb::RestoreStats stats;
	MojAssertNoErr( db.restore(palm.c_str(), files, stats) );
	EXPECT_EQ(4u, stats.records);
	EXPECT_EQ(1u, stats.kinds);
	EXPECT_EQ(1u, stats.permissions);
	EXPECT_EQ(1u, stats.tombstones);
	EXPECT_EQ(1u, stats.restored);
	EXPECT_FALSE(kindLoaded());
}
