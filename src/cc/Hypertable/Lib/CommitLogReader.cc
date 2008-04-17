/** -*- c++ -*-
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 * 
 * This file is part of Hypertable.
 * 
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 * 
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <string>
#include <vector>

extern "C" {
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
}

#include "Common/Error.h"
#include "Common/Logger.h"

#include "Hypertable/Lib/CompressorFactory.h"
#include "CommitLog.h"
#include "CommitLogReader.h"

using namespace Hypertable;
using namespace std;

namespace {
  const uint32_t READAHEAD_BUFFER_SIZE = 131072;
}

/**
 *
 */
CommitLogReader::CommitLogReader(Filesystem *fs, const std::string &logDir) : m_fs(fs), m_log_dir(logDir), m_fd(-1), m_block_buffer(256), m_zblock_buffer(256), m_error(0), m_compressor(0) {
  LogFileInfoT fileInfo;
  int32_t fd;
  vector<string> listing;
  int64_t flen;
  uint32_t nread;
  DynamicBuffer input(0);

  HT_INFOF("Opening commit log %s", logDir.c_str());

  if (m_log_dir.find('/', m_log_dir.length()-1) == string::npos)
    m_log_dir += "/";

  m_fs->readdir(m_log_dir, listing);
  m_log_file_info.clear();
  fileInfo.trailer.set_magic(CommitLog::MAGIC_TRAILER);

  for (size_t i=0; i<listing.size(); i++) {
    char *endptr;
    long num = strtol(listing[i].c_str(), &endptr, 10);
    if (*endptr != 0) {
      HT_WARNF("Invalid file '%s' found in commit log directory '%s'", listing[i].c_str(), m_log_dir.c_str());
    }
    else {
      fileInfo.num = (uint32_t)num;
      fileInfo.fname = m_log_dir + listing[i];
      m_log_file_info.push_back(fileInfo);
    }
  }

  sort(m_log_file_info.begin(), m_log_file_info.end());

  input.ensure( BlockCompressionHeaderCommitLog::LENGTH );
  m_zblock_buffer.ensure( BlockCompressionHeaderCommitLog::LENGTH );

  for (size_t i=0; i<m_log_file_info.size(); i++) {

    m_log_file_info[i].trailer.set_timestamp(0);
    flen = m_fs->length(m_log_file_info[i].fname);

    if (flen < (int64_t)BlockCompressionHeaderCommitLog::LENGTH)
      continue;

    fd = m_fs->open(m_log_file_info[i].fname);

    nread = m_fs->pread(fd, input.buf, BlockCompressionHeaderCommitLog::LENGTH,
                        flen - BlockCompressionHeaderCommitLog::LENGTH);
    HT_EXPECT(nread != m_fixed_header_length, Error::RESPONSE_TRUNCATED);
    m_fs->close(fd);
    input.ptr = input.buf;

    size_t remaining = nread;
    m_log_file_info[i].trailer.decode(&input.ptr, &remaining);

    if (m_log_file_info[i].trailer.check_magic(CommitLog::MAGIC_TRAILER)) {
      if (m_compressor == 0) {
        m_compressor = CompressorFactory::create_block_codec(
                        (BlockCompressionCodec::Type)
                        m_log_file_info[i].trailer.get_compression_type());
	m_got_compressor = true;
      }
    }
    else {
      m_log_file_info[i].trailer.set_timestamp(0);
      m_compressor = CompressorFactory::create_block_codec(
                        BlockCompressionCodec::NONE);
      m_got_compressor = false;
    }

    //cout << m_log_file_info[i].num << ":  " << m_log_file_info[i].fname << " " << m_log_file_info[i].timestamp << endl;
  }
}


CommitLogReader::~CommitLogReader() {
  delete m_compressor;
}


void CommitLogReader::initialize_read(uint64_t timestamp) {
  m_cutoff_time = timestamp;
  m_cur_log_offset = 0;
  m_fd = -1;
}



bool CommitLogReader::next_block(const uint8_t **blockp, size_t *lenp, BlockCompressionHeaderCommitLog *header) {
  size_t remaining;
  uint32_t nread;
  uint64_t timestamp;

 try_again:

  if (m_fd == -1) {

    while (m_cur_log_offset < m_log_file_info.size()) {
      timestamp = m_log_file_info[m_cur_log_offset].trailer.get_timestamp();
      if (timestamp == 0 || timestamp >= m_cutoff_time)
	break;
      m_cur_log_offset++;
    }

    if (m_cur_log_offset >= m_log_file_info.size())
      return false;

    m_fd = m_fs->open_buffered(m_log_file_info[m_cur_log_offset].fname,
                               READAHEAD_BUFFER_SIZE, 2);
  }

  m_zblock_buffer.ptr = m_zblock_buffer.buf;
  nread = m_fs->read(m_fd, m_zblock_buffer.ptr,
                     BlockCompressionHeaderCommitLog::LENGTH);
    
  if (nread != BlockCompressionHeaderCommitLog::LENGTH) {
    HT_ERRORF("Short read of commit log block '%s'",
              m_log_file_info[m_cur_log_offset].fname.c_str());
    m_fs->close(m_fd);
    m_fd = -1;
    m_cur_log_offset++;
    m_error = Error::RANGESERVER_TRUNCATED_COMMIT_LOG;
    return false;
  }
  else {
    remaining = nread;

    // decode header
    header->decode(&m_zblock_buffer.ptr, &remaining);

    if (header->check_magic(CommitLog::MAGIC_TRAILER)) {
      // TODO: this could be asynchronous
      m_fs->close(m_fd);
      m_fd = -1;
      m_cur_log_offset++;
      goto try_again;
    }
  }

  m_zblock_buffer.ensure(header->get_data_zlength());
  nread = m_fs->read(m_fd, m_zblock_buffer.ptr, header->get_data_zlength());
      
  if (nread != header->get_data_zlength()) {
    HT_ERRORF("Short read of commit log block '%s'",
              m_log_file_info[m_cur_log_offset].fname.c_str());
    m_fs->close(m_fd);
    m_fd = -1;
    m_error = Error::RANGESERVER_TRUNCATED_COMMIT_LOG;
    return false;
  }

  m_zblock_buffer.ptr += nread;

  if (!m_got_compressor && header->get_compression_type() != BlockCompressionCodec::NONE) {
    delete m_compressor;
    m_compressor = CompressorFactory::create_block_codec(
        (BlockCompressionCodec::Type)header->get_compression_type());
    m_got_compressor = true;
  }

  /**
   * decompress block
   */
  if ((m_error = m_compressor->inflate(m_zblock_buffer, m_block_buffer, *header)) != Error::OK)
    return false;

  *blockp = m_block_buffer.buf;
  *lenp = m_block_buffer.fill();

  return true;
}



/**
 *
 */
void CommitLogReader::dump_log_metadata() {
  for (size_t i=0; i<m_log_file_info.size(); i++)
    cout << "LOG FRAGMENT name='" << m_log_file_info[i].fname << "' timestamp=" << m_log_file_info[i].trailer.get_timestamp() << endl;
}
