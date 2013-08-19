/* @@@LICENSE
*
*      Copyright (c) 2013 LG Electronics, Inc.
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


#ifndef MOJDBSHARDMANAGERTEST_H_
#define MOJDBSHARDMANAGERTEST_H_

#include "MojDbTestRunner.h"

class MojDbShardIdCache;
class MojDbShardEngine;

class MojDbShardManagerTest : public MojTestCase
{
public:
    MojDbShardManagerTest();

    virtual MojErr run(void);

private:
    MojErr _testShardIdCache (MojDbShardIdCache* ip_cache);
    MojErr _testShardManager (MojDbShardEngine* ip_eng);

    MojErr displayMessage(const MojChar* format, ...);
    void cleanup(void);
};

#endif /* MOJDBSHARDMANAGER_H_ */