/*
 * Copyright 2011, Ben Langmead <langmea@cs.jhu.edu>
 *
 * This file is part of Bowtie 2.
 *
 * Bowtie 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bowtie 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bowtie 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OUTQ_H_
#define OUTQ_H_

#include "assert_helpers.h"
#include "ds.h"
#include "sstring.h"
#include "read.h"
#include "threading.h"
#include "mem_ids.h"

/**
 * Encapsulates a list of lines of output.  If the earliest as-yet-unreported
 * read has id N and Bowtie 2 wants to write a record for read with id N+1, we
 * resize the lines_ and committed_ lists to have at least 2 elements (1 for N,
 * 1 for N+1) and return the BTString * associated with the 2nd element.  When
 * the user calls commit() for the read with id N, 
 */
class OutputQueue {

	static const size_t NFLUSH_THRESH = 8;

public:

	OutputQueue(
		const std::string& ofn, // empty -> stdin
		size_t output_buffer_size,
		bool reorder,
		size_t nthreads,
		bool threadSafe,
		int perThreadBufSize,
		TReadId rdid = 0) :
		ofh_(stdout),
		obuf_(NULL),
		cur_(rdid),
		lines_(RES_CAT),
		started_(RES_CAT),
		finished_(RES_CAT),
		reorder_(reorder),
		threadSafe_(threadSafe),
		mutex_m(),
		nthreads_(nthreads),
		perThreadBuf_(NULL),
		perThreadCounter_(NULL),
		perThreadStarted_(NULL),
		perThreadFinished_(NULL),
		perThreadFlushed_(NULL),
		perThreadBufSize_(perThreadBufSize)
	{
		assert_gt(nthreads_, 0);
		assert(nthreads_ == 1 || threadSafe);
        assert_gt(perThreadBufSize_, 0);
		perThreadBuf_ = new BTString*[nthreads_];
		perThreadCounter_ = new int[nthreads_ * 4];
		perThreadStarted_ = perThreadCounter_ + nthreads_;
		perThreadFinished_ = perThreadStarted_ + nthreads_;
		perThreadFlushed_ = perThreadFinished_ + nthreads_;
		for(size_t i = 0; i < nthreads_; i++) {
			perThreadBuf_[i] = new BTString[perThreadBufSize_];
			perThreadCounter_[i] = 0;
			perThreadStarted_[i] = 0;
			perThreadFinished_[i] = 0;
			perThreadFlushed_[i] = 0;
		}
		if(!ofn.empty()) {
			ofh_ = fopen(ofn.c_str(), "w");
			if(ofh_ == NULL) {
				std::cerr << "Error: Could not open alignment output file "
				          << ofn << std::endl;
				throw 1;
			}
			obuf_ = new char[output_buffer_size];
			int ret = setvbuf(ofh_, obuf_, _IOFBF, output_buffer_size);
			if(ret != 0) {
				std::cerr << "Warning: Could not allocate the proper "
				          << "buffer size for output file stream. "
				          << "Return value = " << ret << std::endl;
			}
		}
	}

	~OutputQueue() {
		if(perThreadBuf_ != NULL) {
			for(size_t i = 0; i < nthreads_; i++) {
				delete[] perThreadBuf_[i];
			}
			delete[] perThreadBuf_;
			perThreadBuf_ = NULL;
		}
		if(perThreadCounter_ != NULL) {
			delete[] perThreadCounter_;
			perThreadCounter_ = NULL;
		}
		if(obuf_ != NULL) {
			delete[] obuf_;
			obuf_ = NULL;
		}
		if(ofh_ != NULL) {
			fclose(ofh_);
			ofh_ = NULL;
		}
	}

	/**
	 * Caller is telling us that they're about to write output record(s) for
	 * the read with the given id.
	 */
	void beginRead(TReadId rdid, size_t threadId);
	
	/**
	 * Writer is finished writing to 
	 */
	void finishRead(const BTString& rec, TReadId rdid, size_t threadId);
	
	/**
	 * Return the number of records currently being buffered.
	 */
	size_t size() const {
		return lines_.size();
	}
	
	/**
	 * Return the number of records that have been flushed so far.
	 */
	TReadId numFlushed() const {
		TReadId tot = 0;
		for(size_t i = 0; i < nthreads_; i++) {
			tot += perThreadFlushed_[i];
		}
		return tot;
	}

	/**
	 * Return the number of records that have been started so far.
	 */
	TReadId numStarted() const {
		TReadId tot = 0;
		for(size_t i = 0; i < nthreads_; i++) {
			tot += perThreadStarted_[i];
		}
		return tot;
	}

	/**
	 * Return the number of records that have been finished so far.
	 */
	TReadId numFinished() const {
		TReadId tot = 0;
		for(size_t i = 0; i < nthreads_; i++) {
			tot += perThreadFinished_[i];
		}
		return tot;
	}
	
	/**
	 * Write a c++ string to the write buffer and, if necessary, flush.
	 */
	void writeString(const BTString& s);
	
	/**
	 * Write already-committed lines starting from cur_.
	 */
	void flush(bool force = false, bool getLock = true);

protected:

	FILE            *ofh_;
	char            *obuf_;
	TReadId         cur_;
	EList<BTString> lines_;
	EList<bool>     started_;
	EList<bool>     finished_;
	bool            reorder_;
	bool            threadSafe_;
	MUTEX_T         mutex_m;
	
	size_t nthreads_;
	BTString** perThreadBuf_;
	int* perThreadCounter_;
	int* perThreadStarted_;
	int* perThreadFinished_;
	int* perThreadFlushed_;
	int perThreadBufSize_;

private:

	void flushImpl(bool force);
	void beginReadImpl(TReadId rdid, size_t threadId);
	void finishReadImpl(const BTString& rec, TReadId rdid, size_t threadId);
};

class OutputQueueMark {
public:
	OutputQueueMark(
		OutputQueue& q,
		const BTString& rec,
		TReadId rdid,
		size_t threadId) :
		q_(q),
		rec_(rec),
		rdid_(rdid),
		threadId_(threadId)
	{
		q_.beginRead(rdid, threadId);
	}
	
	~OutputQueueMark() {
		q_.finishRead(rec_, rdid_, threadId_);
	}
	
protected:
	OutputQueue& q_;
	const BTString& rec_;
	TReadId rdid_;
	size_t threadId_;
};

#endif
