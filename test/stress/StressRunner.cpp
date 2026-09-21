// Copyright (c) 2026 LG Electronics, Inc. and webOS-ports project
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

// db8-stress: standalone multi-threaded stress and endurance harness for the
// db8 storage kernel. Hammers a MojDb instance with concurrent writers,
// readers, deleters and periodic maintenance (purge/compact/stats) and checks
// consistency invariants at the end. Runs directly against the storage engine,
// so it works both on a build host and on a device without luna-service2.
//
// Usage:
//   db8-stress [--dir PATH] [--seconds N] [--writers N] [--readers N]
//              [--objects N] [--seed N] [--verbose]
//
// When more than one storage backend is compiled in, select one with
// MOJODB_ENGINE=sandwich (or leveldb) in the environment.
//
// Exit code 0 = all invariants held; non-zero = failure (details on stderr).

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "db/MojDb.h"
#include "db/MojDbQuery.h"
#include "db/MojDbSearchCursor.h"
#include "core/MojUtil.h"

namespace {

const MojChar* const kKindId = _T("StressTest:1");
const MojChar* const kKindDef =
    _T("{\"id\":\"StressTest:1\",\"owner\":\"com.webos.db8.stress\",")
    _T("\"indexes\":[")
        _T("{\"name\":\"bucket\",\"props\":[{\"name\":\"bucket\"}]},")
        _T("{\"name\":\"counter\",\"props\":[{\"name\":\"counter\"}]},")
        _T("{\"name\":\"bucketCounter\",\"props\":[{\"name\":\"bucket\"},{\"name\":\"counter\"}]}")
    _T("]}");

struct Options {
    MojString dir;
    unsigned seconds = 30;
    unsigned writers = 4;
    unsigned readers = 4;
    unsigned objects = 2000;    // live objects to aim for per writer
    unsigned seed = 12345;
    bool verbose = false;
    bool keep = false;          // keep database dir on success
};

struct Stats {
    std::atomic<uint64_t> puts{0};
    std::atomic<uint64_t> updates{0};
    std::atomic<uint64_t> deletes{0};
    std::atomic<uint64_t> finds{0};
    std::atomic<uint64_t> foundObjects{0};
    std::atomic<uint64_t> maintenance{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<bool> failed{false};
};

struct Shared {
    MojDb* db = nullptr;
    Options opts;
    Stats stats;
    std::atomic<bool> stop{false};
    // net live objects each writer thread believes it owns
    std::vector<std::atomic<int64_t>*> liveCount;
};

void reportErr(Shared& sh, const char* what, MojErr err)
{
    sh.stats.errors.fetch_add(1, std::memory_order_relaxed);
    sh.stats.failed.store(true, std::memory_order_relaxed);
    MojString msg;
    (void) MojErrToString(err, msg);
    fprintf(stderr, "[db8-stress] %s failed: %d (%s)\n", what, (int) err, msg.data());
}

// Writer thread: keeps a window of live objects for its own bucket,
// continuously putting, updating and deleting them.
void writerThread(Shared* shp, unsigned tid)
{
    Shared& sh = *shp;
    std::mt19937 rng(sh.opts.seed + tid);
    std::vector<MojObject> ids;
    ids.reserve(sh.opts.objects);
    int64_t live = 0;

    while (!sh.stop.load(std::memory_order_relaxed)) {
        unsigned action = static_cast<unsigned>(rng() % 100);
        // below the target window, always grow; at the window put/update/delete
        // are balanced so the live set stays stable for endurance runs
        if (ids.size() < sh.opts.objects || action < 33) {
            // put a fresh object
            MojObject obj;
            MojErr err = obj.putString(MojDb::KindKey, kKindId);
            if (err == MojErrNone) err = obj.putInt(_T("bucket"), (MojInt64) tid);
            if (err == MojErrNone) err = obj.putInt(_T("counter"), (MojInt64)(rng() % 1000));
            MojString payload;
            if (err == MojErrNone) err = payload.format(_T("payload-%u-%u"), tid, (unsigned) rng());
            if (err == MojErrNone) err = obj.putString(_T("payload"), payload);
            if (err == MojErrNone) err = sh.db->put(obj);
            if (err != MojErrNone) { reportErr(sh, "put", err); break; }
            MojObject id;
            if (!obj.get(MojDb::IdKey, id)) { reportErr(sh, "put/idlookup", MojErrDbInvalidKey); break; }
            ids.push_back(id);
            ++live;
            sh.stats.puts.fetch_add(1, std::memory_order_relaxed);
        } else if (action < 66 && !ids.empty()) {
            // update a random object (merge)
            MojObject& id = ids[rng() % ids.size()];
            MojObject obj;
            MojErr err = obj.put(MojDb::IdKey, id);
            if (err == MojErrNone) err = obj.putString(MojDb::KindKey, kKindId);
            if (err == MojErrNone) err = obj.putInt(_T("counter"), (MojInt64)(rng() % 1000));
            if (err == MojErrNone) err = sh.db->merge(obj);
            if (err == MojErrDbObjectNotFound) {
                // deleted concurrently is impossible here (ids are per-thread), treat as failure
                reportErr(sh, "merge/notfound", err); break;
            } else if (err != MojErrNone) { reportErr(sh, "merge", err); break; }
            sh.stats.updates.fetch_add(1, std::memory_order_relaxed);
        } else if (!ids.empty()) {
            // delete a random object
            size_t idx = rng() % ids.size();
            MojObject id = ids[idx];
            ids[idx] = ids.back();
            ids.pop_back();
            bool found = false;
            MojErr err = sh.db->del(id, found);
            if (err != MojErrNone) { reportErr(sh, "del", err); break; }
            if (!found) { reportErr(sh, "del/lost-object", MojErrDbObjectNotFound); break; }
            --live;
            sh.stats.deletes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    sh.liveCount[tid]->store(live, std::memory_order_relaxed);
}

// Reader thread: runs queries against random buckets and sanity-checks results.
void readerThread(Shared* shp, unsigned tid)
{
    Shared& sh = *shp;
    std::mt19937 rng(sh.opts.seed * 31 + tid);
    while (!sh.stop.load(std::memory_order_relaxed)) {
        MojDbQuery query;
        MojErr err = query.from(kKindId);
        MojInt64 bucket = (MojInt64)(rng() % (sh.opts.writers ? sh.opts.writers : 1));
        if (err == MojErrNone) err = query.where(_T("bucket"), MojDbQuery::OpEq, bucket);
        if (err == MojErrNone) query.limit(64);
        MojDbCursor cursor;
        if (err == MojErrNone) err = sh.db->find(query, cursor);
        if (err != MojErrNone) { reportErr(sh, "find", err); break; }
        for (;;) {
            bool found = false;
            MojObject obj;
            err = cursor.get(obj, found);
            if (err != MojErrNone) { reportErr(sh, "cursor.get", err); break; }
            if (!found)
                break;
            MojInt64 gotBucket = -1;
            if (!obj.get(_T("bucket"), gotBucket) || gotBucket != bucket) {
                reportErr(sh, "query/wrong-bucket", MojErrDbInconsistentIndex);
                break;
            }
            sh.stats.foundObjects.fetch_add(1, std::memory_order_relaxed);
        }
        (void) cursor.close();
        if (sh.stats.failed.load(std::memory_order_relaxed))
            break;
        sh.stats.finds.fetch_add(1, std::memory_order_relaxed);
    }
}

// Maintenance thread: periodic purge, compact and stats like the service does.
void maintenanceThread(Shared* shp)
{
    Shared& sh = *shp;
    unsigned iter = 0;
    while (!sh.stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 20 && !sh.stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (sh.stop.load(std::memory_order_relaxed))
            break;
        MojErr err;
        if (iter % 3 == 0) {
            MojUInt32 count = 0;
            err = sh.db->purge(count, 1 /* purge revs older than one day: no-op but exercises path */);
            if (err != MojErrNone) { reportErr(sh, "purge", err); break; }
        } else if (iter % 3 == 1) {
            err = sh.db->compact();
            if (err != MojErrNone) { reportErr(sh, "compact", err); break; }
        } else {
            MojObject result;
            err = sh.db->stats(result);
            if (err != MojErrNone) { reportErr(sh, "stats", err); break; }
        }
        ++iter;
        sh.stats.maintenance.fetch_add(1, std::memory_order_relaxed);
    }
}

MojErr countObjects(MojDb* db, MojUInt32& countOut)
{
    MojDbQuery query;
    MojErr err = query.from(kKindId);
    MojErrCheck(err);
    MojDbCursor cursor;
    err = db->find(query, cursor);
    MojErrCheck(err);
    err = cursor.count(countOut);
    (void) cursor.close();
    MojErrCheck(err);
    return MojErrNone;
}

void usage(const char* argv0)
{
    fprintf(stderr,
        "Usage: %s [--dir PATH] [--seconds N] [--writers N] [--readers N]\n"
        "          [--objects N] [--seed N] [--keep] [--verbose]\n", argv0);
}

} // namespace

int main(int argc, char** argv)
{
    Options opts;
    MojErr err = opts.dir.assign(_T(""));
    if (err != MojErrNone)
        return 2;

    for (int i = 1; i < argc; ++i) {
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(argv[i], "--dir")) { if (opts.dir.assign(need("--dir")) != MojErrNone) return 2; }
        else if (!strcmp(argv[i], "--seconds")) opts.seconds = (unsigned) atoi(need("--seconds"));
        else if (!strcmp(argv[i], "--writers")) opts.writers = (unsigned) atoi(need("--writers"));
        else if (!strcmp(argv[i], "--readers")) opts.readers = (unsigned) atoi(need("--readers"));
        else if (!strcmp(argv[i], "--objects")) opts.objects = (unsigned) atoi(need("--objects"));
        else if (!strcmp(argv[i], "--seed")) opts.seed = (unsigned) atoi(need("--seed"));
        else if (!strcmp(argv[i], "--keep")) opts.keep = true;
        else if (!strcmp(argv[i], "--verbose")) opts.verbose = true;
        else { usage(argv[0]); return 2; }
    }
    if (opts.writers == 0 || opts.seconds == 0) {
        usage(argv[0]);
        return 2;
    }

    char dirBuf[] = "/tmp/db8-stress-XXXXXX";
    bool madeTemp = false;
    if (opts.dir.empty()) {
        if (!mkdtemp(dirBuf)) {
            perror("mkdtemp");
            return 2;
        }
        madeTemp = true;
        if (opts.dir.assign(dirBuf) != MojErrNone)
            return 2;
    }

    printf("[db8-stress] dir=%s seconds=%u writers=%u readers=%u objects=%u seed=%u\n",
           opts.dir.data(), opts.seconds, opts.writers, opts.readers, opts.objects, opts.seed);

    MojDb db;
    err = db.open(opts.dir.data());
    if (err != MojErrNone) {
        fprintf(stderr, "[db8-stress] db.open(%s) failed: %d\n", opts.dir.data(), (int) err);
        return 1;
    }

    MojObject kind;
    err = kind.fromJson(kKindDef);
    if (err == MojErrNone)
        err = db.putKind(kind);
    if (err != MojErrNone) {
        fprintf(stderr, "[db8-stress] putKind failed: %d\n", (int) err);
        (void) db.close();
        return 1;
    }

    Shared sh;
    sh.db = &db;
    sh.opts = opts;
    std::vector<std::atomic<int64_t>> liveStorage(opts.writers);
    for (auto& v : liveStorage) {
        v.store(0, std::memory_order_relaxed);
        sh.liveCount.push_back(&v);
    }

    std::vector<std::thread> threads;
    for (unsigned t = 0; t < opts.writers; ++t)
        threads.emplace_back(writerThread, &sh, t);
    for (unsigned t = 0; t < opts.readers; ++t)
        threads.emplace_back(readerThread, &sh, t);
    threads.emplace_back(maintenanceThread, &sh);

    for (unsigned s = 0; s < opts.seconds && !sh.stats.failed.load(); ++s) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (opts.verbose) {
            printf("[db8-stress] t=%us puts=%" PRIu64 " updates=%" PRIu64 " dels=%" PRIu64
                   " finds=%" PRIu64 " maint=%" PRIu64 " errs=%" PRIu64 "\n",
                   s + 1,
                   sh.stats.puts.load(), sh.stats.updates.load(), sh.stats.deletes.load(),
                   sh.stats.finds.load(), sh.stats.maintenance.load(), sh.stats.errors.load());
        }
    }
    sh.stop.store(true);
    for (auto& th : threads)
        th.join();

    // Invariant: object count in db equals sum of per-writer live counts.
    int64_t expected = 0;
    for (auto* v : sh.liveCount)
        expected += v->load();
    MojUInt32 actual = 0;
    err = countObjects(&db, actual);
    if (err != MojErrNone) {
        fprintf(stderr, "[db8-stress] final count query failed: %d\n", (int) err);
        sh.stats.failed.store(true);
    } else if ((int64_t) actual != expected) {
        fprintf(stderr, "[db8-stress] INVARIANT VIOLATION: expected %" PRId64
                " live objects, database reports %u\n", expected, actual);
        sh.stats.failed.store(true);
    }

    err = db.close();
    if (err != MojErrNone) {
        fprintf(stderr, "[db8-stress] db.close failed: %d\n", (int) err);
        sh.stats.failed.store(true);
    }

    bool failed = sh.stats.failed.load();
    printf("[db8-stress] done: puts=%" PRIu64 " updates=%" PRIu64 " deletes=%" PRIu64
           " finds=%" PRIu64 " found=%" PRIu64 " maint=%" PRIu64 " errors=%" PRIu64 " live=%" PRId64 " => %s\n",
           sh.stats.puts.load(), sh.stats.updates.load(), sh.stats.deletes.load(),
           sh.stats.finds.load(), sh.stats.foundObjects.load(), sh.stats.maintenance.load(),
           sh.stats.errors.load(), expected, failed ? "FAILED" : "PASSED");

    if (!failed && madeTemp && !opts.keep)
        (void) MojRmDirRecursive(dirBuf);
    return failed ? 1 : 0;
}
