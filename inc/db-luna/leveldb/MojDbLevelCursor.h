/* @@@LICENSE
*
* Copyright (c) 2013 LG Electronics
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

#ifndef MOJDBLEVELCURSOR_H
#define MOJDBLEVELCURSOR_H

#include "leveldb/db.h"
#include "db/MojDbDefs.h"

class MojDbLevelDatabase;
class MojDbLevelItem;

class MojDbLevelCursor : public MojNoCopy
{
public:
    MojDbLevelCursor();
    ~MojDbLevelCursor();

    MojErr open(MojDbLevelDatabase* db, MojDbStorageTxn* txn, guint32 flags);
    MojErr close();
    MojErr del();
    MojErr delPrefix(const MojDbKey& prefix);
    MojErr get(MojDbLevelItem& key, MojDbLevelItem& val, bool& foundOut, guint32 flags);
    MojErr stats(gsize& countOut, gsize& sizeOut);
    MojErr statsPrefix(const MojDbKey& prefix, gsize& countOut, gsize& sizeOut);

    leveldb::Iterator* impl() { return m_it; }

    enum LDB_FLAGS
    {
       e_First = 0, e_Last, e_Next, e_Prev, e_Range, e_Set, e_TotalFlags
    };

private:
    leveldb::Iterator* m_it;
    leveldb::DB* m_db;
    MojDbStorageTxn* m_txn;
    gsize m_warnCount;
};

#endif