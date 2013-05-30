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


#include "core/MojTokenSet.h"
#include "core/MojObjectSerialization.h"

MojTokenSet::MojTokenSet()
{
}

MojErr MojTokenSet::init(MojSharedTokenSet* sharedSet)
{
	MojErr err = sharedSet->tokenSet(m_tokenVec, m_obj);
	MojErrCheck(err);
	m_sharedTokenSet.reset(sharedSet);

	return MojErrNone;
}

MojErr MojTokenSet::tokenFromString(const MojChar* str, guint8& tokenOut, bool add)
{
	tokenOut = InvalidToken;

	// check if we've already added the prop
	guint32 token;
	bool found;
	MojErr err = m_obj.get(str, token, found);
	MojErrCheck(err);
	if (found) {
		MojAssert(token <= G_MAXUINT8 && token != InvalidToken);
		tokenOut = (guint8) token;
	} else if (add) {
		TokenVec updatedVec;
		MojObject updatedObj;
		err = m_sharedTokenSet->addToken(str, tokenOut, updatedVec, updatedObj);
		MojErrCheck(err);
		m_tokenVec.swap(updatedVec);
		m_obj = updatedObj;
	}
	return MojErrNone;
}

MojErr MojTokenSet::stringFromToken(guint8 token, MojString& propNameOut) const
{
	if (token < MojObjectWriter::TokenStartMarker ||
		(gsize)(token - MojObjectWriter::TokenStartMarker) >= m_tokenVec.size()) {
		MojErrThrow(MojErrDbInvalidToken);
	}
	propNameOut = m_tokenVec.at(token - MojObjectWriter::TokenStartMarker);
	
	return MojErrNone;
}
