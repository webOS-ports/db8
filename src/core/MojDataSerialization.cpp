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


#include "core/MojDataSerialization.h"
#include "core/MojDecimal.h"

MojErr MojDataWriter::writeUInt16(guint16 val)
{
	val = MojUInt16ToBigEndian(val);
	MojErr err = m_buf.write((guint8*) &val, sizeof(val));
	MojErrCheck(err);
	return MojErrNone;
}

MojErr MojDataWriter::writeUInt32(guint32 val)
{
	val = MojUInt32ToBigEndian(val);
	MojErr err = m_buf.write((guint8*) &val, sizeof(val));
	MojErrCheck(err);
	return MojErrNone;
}

MojErr MojDataWriter::writeInt64(gint64 val)
{
	val = MojInt64ToBigEndian(val);
	MojErr err = m_buf.write((guint8*) &val, sizeof(val));
	MojErrCheck(err);
	return MojErrNone;
}

MojErr MojDataWriter::writeDecimal(const MojDecimal& val)
{
	MojErr err = writeInt64(val.rep());
	MojErrCheck(err);
	return MojErrNone;
}

MojErr MojDataWriter::writeChars(const MojChar* chars, gsize len)
{
#ifdef MOJ_ENCODING_UTF8
	MojAssert(sizeof(MojChar) == 1);
	MojErr err = m_buf.write((guint8*) chars, len);
	MojErrCheck(err);
	return MojErrNone;
#endif // MOJ_ENCODING_UTF8
}

gsize MojDataWriter::sizeChars(const MojChar* chars, gsize len)
{
#ifdef MOJ_ENCODING_UTF8
	MojAssert(sizeof(MojChar) == 1);
	return len;
#endif // MOJ_ENCODING_UTF8
}

gsize MojDataWriter::sizeDecimal(const MojDecimal& val)
{
	return sizeof(val.rep());
}

MojDataReader::MojDataReader()
: m_begin(NULL),
  m_end(NULL),
  m_pos(NULL)
{
}

MojDataReader::MojDataReader(const guint8* data, gsize size)
: m_begin(data),
  m_end(data + size),
  m_pos(data)
{
	MojAssert(data || size == 0);
}

void MojDataReader::data(const guint8* data, gsize size)
{
	MojAssert(data || size == 0);
	m_begin = data;
	m_end = data + size;
	m_pos = data;
}

MojErr MojDataReader::readUInt8(guint8& val)
{
	if (available() < sizeof(val))
		MojErrThrow(MojErrUnexpectedEof);
	val = *(m_pos++);
	return MojErrNone;
}

MojErr MojDataReader::readUInt16(guint16& val)
{
	if (available() < sizeof(val))
		MojErrThrow(MojErrUnexpectedEof);
	val = (guint16) (m_pos[1] + (m_pos[0] << 8));
	m_pos += sizeof(val);
	return MojErrNone;
}

MojErr MojDataReader::readUInt32(guint32& val)
{
	if (available() < sizeof(val))
		MojErrThrow(MojErrUnexpectedEof);
	val = m_pos[3] + (m_pos[2] << 8) + (m_pos[1] << 16) + (m_pos[0] << 24);
	m_pos += sizeof(val);
	return MojErrNone;
}

MojErr MojDataReader::readInt64(gint64& val)
{
	if (available() < sizeof(val))
		MojErrThrow(MojErrUnexpectedEof);
	val = (gint64)m_pos[7] + ((gint64)m_pos[6] << 8)
		  + ((gint64)m_pos[5] << 16) + ((gint64)m_pos[4] << 24)
		  + ((gint64)m_pos[3] << 32) + ((gint64)m_pos[2] << 40)
		  + ((gint64)m_pos[1] << 48) + ((gint64)m_pos[0] << 56);
	m_pos += sizeof(val);
	return MojErrNone;
}

MojErr MojDataReader::readDecimal(MojDecimal& val)
{
	gint64 rep;
	MojErr err = readInt64(rep);
	MojErrCheck(err);
	val.assignRep(rep);
	return MojErrNone;
}

MojErr MojDataReader::skip(gsize len)
{
	if (available() < len)
		MojErrThrow(MojErrUnexpectedEof);
	m_pos += len;
	return MojErrNone;
}
