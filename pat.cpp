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

#include <cmath>
#include <iostream>
#include <string>
#include <stdexcept>
#include "sstring.h"

#include "pat.h"
#include "filebuf.h"
#include "formats.h"
#include "tokenize.h"

#ifdef USE_SRA

#include "tinythread.h"
#include <ncbi-vdb/NGS.hpp>
#include <ngs/ErrorMsg.hpp>
#include <ngs/ReadCollection.hpp>
#include <ngs/ReadIterator.hpp>
#include <ngs/Read.hpp>

#endif

using namespace std;

/**
 * Calculate a per-read random seed based on a combination of
 * the read data (incl. sequence, name, quals) and the global
 * seed in '_randSeed'.
 */
static uint32_t genRandSeed(
	const BTDnaString& qry,
	const BTString& qual,
	const BTString& name,
	uint32_t seed)
{
	// Calculate a per-read random seed based on a combination of
	// the read data (incl. sequence, name, quals) and the global
	// seed
	uint32_t rseed = (seed + 101) * 59 * 61 * 67 * 71 * 73 * 79 * 83;
	size_t qlen = qry.length();
	// Throw all the characters of the read into the random seed
	for(size_t i = 0; i < qlen; i++) {
		int p = (int)qry[i];
		assert_leq(p, 4);
		size_t off = ((i & 15) << 1);
		rseed ^= (p << off);
	}
	// Throw all the quality values for the read into the random
	// seed
	for(size_t i = 0; i < qlen; i++) {
		int p = (int)qual[i];
		assert_leq(p, 255);
		size_t off = ((i & 3) << 3);
		rseed ^= (p << off);
	}
	// Throw all the characters in the read name into the random
	// seed
	size_t namelen = name.length();
	for(size_t i = 0; i < namelen; i++) {
		int p = (int)name[i];
		if(p == '/') break;
		assert_leq(p, 255);
		size_t off = ((i & 3) << 3);
		rseed ^= (p << off);
	}
	return rseed;
}

/**
 * Return a new dynamically allocated PatternSource for the given
 * format, using the given list of strings as the filenames to read
 * from or as the sequences themselves (i.e. if -c was used).
 */
PatternSource* PatternSource::patsrcFromStrings(
	const PatternParams& p,
	const EList<string>& qs)
{
	switch(p.format) {
		case FASTA:       return new FastaPatternSource(qs, p);
		case FASTA_CONT:  return new FastaContinuousPatternSource(qs, p);
		case RAW:         return new RawPatternSource(qs, p);
		case FASTQ:       return new FastqPatternSource(qs, p);
		case TAB_MATE5:   return new TabbedPatternSource(qs, p, false);
		case TAB_MATE6:   return new TabbedPatternSource(qs, p, true);
		case CMDLINE:     return new VectorPatternSource(qs, p);
		case QSEQ:        return new QseqPatternSource(qs, p);
#ifdef USE_SRA
		case SRA_FASTA:
		case SRA_FASTQ: return new SRAPatternSource(qs, p, p.nthreads);
#endif
		default: {
			cerr << "Internal error; bad patsrc format: " << p.format << endl;
			throw 1;
		}
	}
}

/**
 * Once name/sequence/qualities have been parsed for an
 * unpaired read, set all the other key fields of the Read
 * struct.
 */
void PatternSourcePerThread::finalize(Read& ra) {
	ra.mate = 1;
	ra.rdid = buf_.rdid();
	ra.seed = genRandSeed(ra.patFw, ra.qual, ra.name, pp_.seed);
	ra.finalize(); // set ns_ and construct rev, revcomp
	if(pp_.fixName) {
		ra.fixMateName(1);
	}
}

/**
 * Once name/sequence/qualities have been parsed for a
 * paired-end read, set all the other key fields of the Read
 * structs.
 */
void PatternSourcePerThread::finalizePair(Read& ra, Read& rb) {
	ra.mate = 1;
	rb.mate = 2;
	ra.rdid = rb.rdid = buf_.rdid();
	ra.seed = genRandSeed(ra.patFw, ra.qual, ra.name, pp_.seed);
	rb.seed = genRandSeed(rb.patFw, rb.qual, rb.name, pp_.seed);
	ra.finalize(); // set ns_ and construct rev, revcomp
	rb.finalize(); // set ns_ and construct rev, revcomp
	if(pp_.fixName) {
		ra.fixMateName(1);
		rb.fixMateName(2);
	}
}

/**
 * Get the next paired or unpaired read from the wrapped
 * PatternComposer.  Returns a pair of bools; first indicates
 * whether we were successful, second indicates whether we're
 * done.
 */
pair<bool, bool> PatternSourcePerThread::nextReadPair() {
	// Prepare batch
	if(buf_.exhausted()) {
		pair<bool, int> res = nextBatch();
		if(res.first && res.second == 0) {
			return make_pair(false, true);
		}
		last_batch_ = res.first;
		last_batch_size_ = res.second;
		assert_eq(0, buf_.cur_buf_);
	} else {
		buf_.next(); // advance cursor
		assert_gt(buf_.cur_buf_, 0);
	}
	// Parse read/pair
	assert(buf_.read_a().empty());
	if(!parse(buf_.read_a(), buf_.read_b())) {
		return make_pair(false, false);
	}
	// Finalize read/pair
	if(!buf_.read_b().patFw.empty()) {
		finalizePair(buf_.read_a(), buf_.read_b());
	} else {
		finalize(buf_.read_a());
	}
	bool this_is_last = buf_.cur_buf_ == last_batch_size_-1;
	return make_pair(true, this_is_last ? last_batch_ : false);
}

/**
 * The main member function for dispensing pairs of reads or
 * singleton reads.  Returns true iff ra and rb contain a new
 * pair; returns false if ra contains a new unpaired read.
 */
pair<bool, int> SoloPatternComposer::nextBatch(PerThreadReadBuf& pt) {
	size_t cur = cur_;
	while(cur < src_->size()) {
		// Patterns from srca_[cur_] are unpaired
		pair<bool, int> res;
		do {
			res = (*src_)[cur]->nextBatch(
				pt,
				true,  // batch A (or pairs)
				true); // grab lock below
		} while(!res.first && res.second == 0);
		if(res.second == 0) {
			ThreadSafe ts(mutex_m);
			if(cur + 1 > cur_) {
				cur_++;
			}
			cur = cur_;
			continue; // on to next pair of PatternSources
		}
		return res;
	}
	assert_leq(cur, src_->size());
	return make_pair(true, 0);
}

/**
 * The main member function for dispensing pairs of reads or
 * singleton reads.  Returns true iff ra and rb contain a new
 * pair; returns false if ra contains a new unpaired read.
 */
pair<bool, int> DualPatternComposer::nextBatch(PerThreadReadBuf& pt) {
	// 'cur' indexes the current pair of PatternSources
	size_t cur = cur_;
	while(cur < srca_->size()) {
		if((*srcb_)[cur] == NULL) {
			// Patterns from srca_ are unpaired
			pair<bool, int> res = (*srca_)[cur]->nextBatch(
				pt,
				true,  // batch A (or pairs)
				true); // grab lock below
			bool done = res.first;
			if(!done && res.second == 0) {
				ThreadSafe ts(mutex_m);
				if(cur + 1 > cur_) cur_++;
				cur = cur_; // Move on to next PatternSource
				continue; // on to next pair of PatternSources
			}
			return make_pair(done, res.second);
		} else {
			pair<bool, int> resa, resb;
			// Lock to ensure that this thread gets parallel reads
			// in the two mate files
			{
				ThreadSafe ts(mutex_m);
				resa = (*srca_)[cur]->nextBatch(
					pt,
					true,   // batch A
					false); // don't grab lock below
				resb = (*srcb_)[cur]->nextBatch(
					pt,
					false,  // batch B
					false); // don't grab lock below
				assert_eq((*srca_)[cur]->readCount(),
				          (*srcb_)[cur]->readCount());
			}
			if(resa.second < resb.second) {
				cerr << "Error, fewer reads in file specified with -1 "
				     << "than in file specified with -2" << endl;
				throw 1;
			} else if(resa.second == 0 && resb.second == 0) {
				ThreadSafe ts(mutex_m);
				if(cur + 1 > cur_) {
					cur_++;
				}
				cur = cur_; // Move on to next PatternSource
				continue; // on to next pair of PatternSources
			} else if(resb.second < resa.second) {
				cerr << "Error, fewer reads in file specified with -2 "
				     << "than in file specified with -1" << endl;
				throw 1;
			}
			assert_eq(resa.first, resb.first);
			assert_eq(resa.second, resb.second);
			return make_pair(resa.first, resa.second);
		}
	}
	assert_leq(cur, srca_->size());
	return make_pair(true, 0);
}

/**
 * Given the values for all of the various arguments used to specify
 * the read and quality input, create a list of pattern sources to
 * dispense them.
 */
PatternComposer* PatternComposer::setupPatternComposer(
	const EList<string>& si,   // singles, from argv
	const EList<string>& m1,   // mate1's, from -1 arg
	const EList<string>& m2,   // mate2's, from -2 arg
	const EList<string>& m12,  // both mates on each line, from --12 arg
#ifdef USE_SRA
	const EList<string>& sra_accs, // SRA accessions
#endif
	const EList<string>& q,    // qualities associated with singles
	const EList<string>& q1,   // qualities associated with m1
	const EList<string>& q2,   // qualities associated with m2
	const PatternParams& p,    // read-in parameters
	bool verbose)              // be talkative?
{
	EList<PatternSource*>* a  = new EList<PatternSource*>();
	EList<PatternSource*>* b  = new EList<PatternSource*>();
	EList<PatternSource*>* ab = new EList<PatternSource*>();
	// Create list of pattern sources for paired reads appearing
	// interleaved in a single file
	for(size_t i = 0; i < m12.size(); i++) {
		const EList<string>* qs = &m12;
		EList<string> tmp;
		if(p.fileParallel) {
			// Feed query files one to each PatternSource
			qs = &tmp;
			tmp.push_back(m12[i]);
			assert_eq(1, tmp.size());
		}
		ab->push_back(PatternSource::patsrcFromStrings(p, *qs));
		if(!p.fileParallel) {
			break;
		}
	}

	// Create list of pattern sources for paired reads
	for(size_t i = 0; i < m1.size(); i++) {
		const EList<string>* qs = &m1;
		EList<string> tmpSeq;
		EList<string> tmpQual;
		if(p.fileParallel) {
			// Feed query files one to each PatternSource
			qs = &tmpSeq;
			tmpSeq.push_back(m1[i]);
			assert_eq(1, tmpSeq.size());
		}
		a->push_back(PatternSource::patsrcFromStrings(p, *qs));
		if(!p.fileParallel) {
			break;
		}
	}

	// Create list of pattern sources for paired reads
	for(size_t i = 0; i < m2.size(); i++) {
		const EList<string>* qs = &m2;
		EList<string> tmpSeq;
		EList<string> tmpQual;
		if(p.fileParallel) {
			// Feed query files one to each PatternSource
			qs = &tmpSeq;
			tmpSeq.push_back(m2[i]);
			assert_eq(1, tmpSeq.size());
		}
		b->push_back(PatternSource::patsrcFromStrings(p, *qs));
		if(!p.fileParallel) {
			break;
		}
	}
	// All mates/mate files must be paired
	assert_eq(a->size(), b->size());

	// Create list of pattern sources for the unpaired reads
	for(size_t i = 0; i < si.size(); i++) {
		const EList<string>* qs = &si;
		PatternSource* patsrc = NULL;
		EList<string> tmpSeq;
		EList<string> tmpQual;
		if(p.fileParallel) {
			// Feed query files one to each PatternSource
			qs = &tmpSeq;
			tmpSeq.push_back(si[i]);
			assert_eq(1, tmpSeq.size());
		}
		patsrc = PatternSource::patsrcFromStrings(p, *qs);
		assert(patsrc != NULL);
		a->push_back(patsrc);
		b->push_back(NULL);
		if(!p.fileParallel) {
			break;
		}
	}

	PatternComposer *patsrc = NULL;
	if(m12.size() > 0) {
		patsrc = new SoloPatternComposer(ab, p);
		for(size_t i = 0; i < a->size(); i++) delete (*a)[i];
		for(size_t i = 0; i < b->size(); i++) delete (*b)[i];
		delete a; delete b;
	} else {
		patsrc = new DualPatternComposer(a, b, p);
		for(size_t i = 0; i < ab->size(); i++) delete (*ab)[i];
		delete ab;
	}
	return patsrc;
}

void PatternComposer::free_EList_pmembers( const EList<PatternSource*> &elist) {
    for (size_t i = 0; i < elist.size(); i++)
        if (elist[i] != NULL)
            delete elist[i];
}

/**
 * Fill Read with the sequence, quality and name for the next
 * read in the list of read files.  This function gets called by
 * all the search threads, so we must handle synchronization.
 *
 * Returns pair<bool, int> where bool indicates whether we're
 * completely done, and int indicates how many reads were read.
 */
pair<bool, int> CFilePatternSource::nextBatchImpl(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	bool done = false;
	int nread = 0;
	
	// synchronization at this level because both reading and manipulation of
	// current file pointer have to be protected
	pt.setReadId(readCnt_);
	while(true) { // loop that moves on to next file when needed
		do {
			pair<bool, int> ret = nextBatchFromFile(pt, batch_a);
			done = ret.first;
			nread = ret.second;
		} while(!done && nread == 0); // not sure why this would happen
		if(done && filecur_ < infiles_.size()) { // finished with this file
			open();
			resetForNextFile(); // reset state to handle a fresh file
			filecur_++;
			if(nread == 0) {
				continue;
			}
		}
		break;
	}
	assert_geq(nread, 0);
	readCnt_ += nread;
	return make_pair(done, nread);
}

pair<bool, int> CFilePatternSource::nextBatch(
	PerThreadReadBuf& pt,
	bool batch_a,
	bool lock)
{
	if(lock) {
		// synchronization at this level because both reading and manipulation of
		// current file pointer have to be protected
		ThreadSafe ts(mutex);
		return nextBatchImpl(pt, batch_a);
	} else {
		return nextBatchImpl(pt, batch_a);
	}
}

/**
 * Open the next file in the list of input files.
 */
void CFilePatternSource::open() {
	if(is_open_) {
		is_open_ = false;
		fclose(fp_);
		fp_ = NULL;
	}
	while(filecur_ < infiles_.size()) {
		if(infiles_[filecur_] == "-") {
			fp_ = stdin;
		} else if((fp_ = fopen(infiles_[filecur_].c_str(), "rb")) == NULL) {
			if(!errs_[filecur_]) {
				cerr << "Warning: Could not open read file \""
				<< infiles_[filecur_].c_str()
				<< "\" for reading; skipping..." << endl;
				errs_[filecur_] = true;
			}
			filecur_++;
			continue;
		}
		is_open_ = true;
		setvbuf(fp_, buf_, _IOFBF, buffer_sz_);
		return;
	}
	cerr << "Error: No input read files were valid" << endl;
	exit(1);
	return;
}

/**
 * Constructor for vector pattern source, used when the user has
 * specified the input strings on the command line using the -c
 * option.
 */
VectorPatternSource::VectorPatternSource(
	const EList<string>& seqs,
	const PatternParams& p) :
	PatternSource(p),
	cur_(p.skip),
	paired_(false),
	tokbuf_(),
	bufs_()
{
	// Install sequences in buffers, ready for immediate copying in
	// nextBatch().  Formatting of the buffer is just like
	// TabbedPatternSource.
	const size_t seqslen = seqs.size();
	for(size_t i = 0; i < seqslen; i++) {
		tokbuf_.clear();
		tokenize(seqs[i], ":", tokbuf_, 2);
		assert_gt(tokbuf_.size(), 0);
		assert_leq(tokbuf_.size(), 2);
		// Get another buffer ready
		bufs_.expand();
		bufs_.back().clear();
		// Install name
		itoa10<TReadId>(static_cast<TReadId>(i), nametmp_);
		bufs_.back().install(nametmp_);
		bufs_.back().append('\t');
		// Install sequence
		bufs_.back().append(tokbuf_[0].c_str());
		bufs_.back().append('\t');
		// Install qualities
		if(tokbuf_.size() > 1) {
			bufs_.back().append(tokbuf_[1].c_str());
		} else {
			const size_t len = tokbuf_[0].length();
			for(size_t i = 0; i < len; i++) {
				bufs_.back().append('I');
			}
		}
	}
}

/**
 * Read next batch.  However, batch concept is not very applicable for this
 * PatternSource where all the info has already been parsed into the fields
 * in the contsructor.	This essentially modifies the pt as though we read
 * in some number of patterns.
 */
pair<bool, int> VectorPatternSource::nextBatchImpl(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	pt.setReadId(cur_);
	EList<Read>& readbuf = batch_a ? pt.bufa_ : pt.bufb_;
	size_t readi = 0;
	for(; readi < pt.max_buf_ && cur_ < bufs_.size(); readi++, cur_++) {
		readbuf[readi].readOrigBuf = bufs_[cur_];
	}
	readCnt_ += readi;
	return make_pair(cur_ == bufs_.size(), readi);
}

pair<bool, int> VectorPatternSource::nextBatch(
	PerThreadReadBuf& pt,
	bool batch_a,
	bool lock)
{
	if(lock) {
		ThreadSafe ts(mutex);
		return nextBatchImpl(pt, batch_a);
	} else {
		return nextBatchImpl(pt, batch_a);
	}
}

/**
 * Finishes parsing outside the critical section.
 */
bool VectorPatternSource::parse(
	Read& ra, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	// Very similar to TabbedPatternSource

	// Light parser (nextBatchFromFile) puts unparsed data
	// into Read& r, even when the read is paired.
	assert(ra.empty());
	assert(!ra.readOrigBuf.empty()); // raw data for read/pair is here
	int c = '\t';
	
	// Loop over the two ends
	for(int endi = 0; endi < 2 && c == '\t'; endi++) {
		Read& r = ((endi == 0) ? ra : rb);
		ParsingCursor& cursor = ((endi == 0) ? cura : curb);
		size_t& off = cursor.off;
		const size_t buflen = cursor.buf->length();
		assert(r.name.empty());
		// Parse name if (a) this is the first end, or
		// (b) this is tab6
		if(endi < 1 || paired_) {
			// Parse read name
			c = (*cursor.buf)[off++];
			while(c != '\t' && off < buflen) {
				r.name.append(c);
				c = (*cursor.buf)[off++];
			}
			assert_eq('\t', c);
			if(off >= buflen) {
				return false; // record ended prematurely
			}
		} else if(endi > 0) {
			// if this is the second end and we're parsing
			// tab5, copy name from first end
			rb.name = ra.name;
		}

		// Parse sequence
		assert(r.patFw.empty());
		c = (*cura.buf)[off++];
		int nchar = 0;
		while(c != '\t' && off < buflen) {
			if(isalpha(c)) {
				assert_in(toupper(c), "ACGTN");
				if(nchar++ >= pp_.trim5) {
					assert_neq(0, asc2dnacat[c]);
					r.patFw.append(asc2dna[c]); // ascii to int
				}
			}
			c = (*cura.buf)[off++];
		}
		assert_eq('\t', c);
		if(off >= buflen) {
			return false; // record ended prematurely
		}
		// record amt trimmed from 5' end due to --trim5
		r.trimmed5 = (int)(nchar - r.patFw.length());
		// record amt trimmed from 3' end due to --trim3
		r.trimmed3 = (int)(r.patFw.trimEnd(pp_.trim3));
		
		// Parse qualities
		assert(r.qual.empty());
		c = (*cura.buf)[off++];
		int nqual = 0;
		while(c != '\t' && c != '\n' && c != '\r') {
			if(c == ' ') {
				wrongQualityFormat(r.name);
				return false;
			}
			char cadd = charToPhred33(c, false, false);
			if(++nqual > pp_.trim5) {
				r.qual.append(cadd);
			}
			if(off >= buflen) break;
			c = (*cura.buf)[off++];
		}
		if(nchar > nqual) {
			tooFewQualities(r.name);
			return false;
		} else if(nqual > nchar) {
			tooManyQualities(r.name);
			return false;
		}
		r.qual.trimEnd(pp_.trim3);
		assert(c == '\t' || c == '\n' || c == '\r' || off >= buflen);
		assert_eq(r.patFw.length(), r.qual.length());
	}
	ra.parsed = true;
	if(!rb.parsed && !rb.readOrigBuf.empty()) {
		return parse(rb, ra, curb, cura, rdid);
	}
	return true;
}

/**
 * Light-parse a FASTA batch into the given buffer.
 */
pair<bool, int> FastaPatternSource::nextBatchFromFile(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	int c;
	EList<Read>& readbuf = batch_a ? pt.bufa_ : pt.bufb_;
	if(first_) {
		c = getc_wrapper();
		if (c == EOF) {
			return make_pair(true, 0);
		}
		while(c == '\r' || c == '\n') {
			c = getc_wrapper();
		}
		if(c != '>') {
			cerr << "Error: reads file does not look like a FASTA file" << endl;
			throw 1;
		}
		first_ = false;
	}
	bool done = false;
	size_t readi = 0;
	// Read until we run out of input or until we've filled the buffer
	for(; readi < pt.max_buf_ && !done; readi++) {
		Read::TBuf& buf = readbuf[readi].readOrigBuf;
		buf.clear();
		buf.append('>');
		while(true) {
			c = getc_wrapper();
			if(c < 0 || c == '>') {
				done = c < 0;
				break;
			}
			buf.append(c);
		}
	}
	// Immediate EOF case
	if(done && readbuf[readi-1].readOrigBuf.length() == 1) {
		readi--;
	}
	return make_pair(done, readi);
}

/**
 * Finalize FASTA parsing outside critical section.
 */
bool FastaPatternSource::parse(
	Read& r, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	// We assume the light parser has put the raw data for the separate ends
	// into separate Read objects.	That doesn't have to be the case, but
	// that's how we've chosen to do it for FastqPatternSource
	if(r.readOrigBuf.empty()) {
		return false;
	}
	assert(r.empty());
	int c = -1;
	size_t& off = cura.off;  // starts at 0
	if(off == 0) {
		off = 1;
	}
	const size_t buflen = cura.buf->length();
	
	// Parse read name
	assert(r.name.empty());
	while(off < buflen) {
		c = (*cura.buf)[off++];
		if(c == '\n' || c == '\r') {
			do {
				c = (*cura.buf)[off++];
			} while((c == '\n' || c == '\r') && off < buflen);
			break;
		}
		r.name.append(c);
	}
	if(off >= buflen) {
		return false; // FASTA ended prematurely
	}
	
	// Parse sequence
	int nchar = 0;
	assert(r.patFw.empty());
	assert(c != '\n' && c != '\r');
	assert_lt(off, buflen);
	while(c != '\n' && off < buflen) {
		if(c == '.') {
			c = 'N';
		}
		if(isalpha(c)) {
			// If it's past the 5'-end trim point
			if(nchar++ >= pp_.trim5) {
				r.patFw.append(asc2dna[c]);
			}
		}
		assert_lt(off, buflen);
		c = (*cura.buf)[off++];
	}
	r.trimmed5 = (int)(nchar - r.patFw.length());
	r.trimmed3 = (int)(r.patFw.trimEnd(pp_.trim3));
	
	for(size_t i = 0; i < r.patFw.length(); i++) {
		r.qual.append('I');
	}

	// Set up a default name if one hasn't been set
	if(r.name.empty()) {
		char cbuf[20];
		itoa10<TReadId>(static_cast<TReadId>(rdid), cbuf);
		r.name.install(cbuf);
	}
	r.parsed = true;
	if(!rb.parsed && !rb.readOrigBuf.empty()) {
		return parse(rb, r, curb, cura, rdid);
	}
	return true;
}

/**
 * Light-parse a FASTA-continuous batch into the given buffer.
 * This is trickier for FASTA-continuous than for other formats,
 * for several reasons:
 *
 * 1. Reads are substrings of a longer FASTA input string
 * 2. Reads may overlap w/r/t the longer FASTA string
 * 3. Read names depend on the most recently observed FASTA
 *	  record name
 */
pair<bool, int> FastaContinuousPatternSource::nextBatchFromFile(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	int c = -1;
	EList<Read>& readbuf = batch_a ? pt.bufa_ : pt.bufb_;
	size_t readi = 0;
	while(readi < pt.max_buf_) {
		c = getc_wrapper();
		if(c < 0) {
			break;
		}
		if(c == '>') {
			resetForNextFile();
			c = getc_wrapper();
			bool sawSpace = false;
			while(c != '\n' && c != '\r') {
				if(!sawSpace) {
					sawSpace = isspace(c);
				}
				if(!sawSpace) {
					name_prefix_buf_.append(c);
				}
				c = getc_wrapper();
			}
			while(c == '\n' || c == '\r') {
				c = getc_wrapper();
			}
			if(c < 0) {
				break;
			}
			name_prefix_buf_.append('_');
		}
		int cat = asc2dnacat[c];
		if(cat >= 2) c = 'N';
		if(cat == 0) {
			// Non-DNA, non-IUPAC char; skip
			continue;
		} else {
			// DNA char
			buf_[bufCur_++] = c;
			if(bufCur_ == 1024) {
				bufCur_ = 0; // wrap around circular buf
			}
			if(eat_ > 0) {
				eat_--;
				// Try to keep readCnt_ aligned with the offset
				// into the reference; that lets us see where
				// the sampling gaps are by looking at the read
				// name
				if(!beginning_) {
					readCnt_++;
				}
				continue;
			}
			// install name
			readbuf[readi].readOrigBuf = name_prefix_buf_;
			itoa10<TReadId>(readCnt_ - subReadCnt_, name_int_buf_);
			readbuf[readi].readOrigBuf.append(name_int_buf_);
			readbuf[readi].readOrigBuf.append('\t');
			// install sequence
			for(size_t i = 0; i < length_; i++) {
				if(length_ - i <= bufCur_) {
					c = buf_[bufCur_ - (length_ - i)];
				} else {
					// Rotate
					c = buf_[bufCur_ - (length_ - i) + 1024];
				}
				readbuf[readi].readOrigBuf.append(c);
			}
			eat_ = freq_-1;
			readCnt_++;
			beginning_ = false;
			readi++;
		}
	}
	return make_pair(c < 0, readi);
}

/**
 * Finalize FASTA-continuous parsing outside critical section.
 */
bool FastaContinuousPatternSource::parse(
	Read& ra, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	// Light parser (nextBatchFromFile) puts unparsed data
	// into Read& r, even when the read is paired.
	assert(ra.empty());
	assert(rb.empty());
	assert(!ra.readOrigBuf.empty()); // raw data for read/pair is here
	assert(rb.readOrigBuf.empty());
	int c = '\t';
	size_t& off = cura.off;
	const size_t buflen = cura.buf->length();
	
	// Parse read name
	c = (*cura.buf)[off++];
	while(c != '\t' && off < buflen) {
		ra.name.append(c);
		c = (*cura.buf)[off++];
	}
	assert_eq('\t', c);
	if(off >= buflen) {
		return false; // record ended prematurely
	}

	// Parse sequence
	assert(ra.patFw.empty());
	c = (*cura.buf)[off++];
	int nchar = 0;
	while(off < buflen) {
		if(isalpha(c)) {
			assert_in(toupper(c), "ACGTN");
			if(nchar++ >= pp_.trim5) {
				assert_neq(0, asc2dnacat[c]);
				ra.patFw.append(asc2dna[c]); // ascii to int
			}
		}
		c = (*cura.buf)[off++];
	}
	// record amt trimmed from 5' end due to --trim5
	ra.trimmed5 = (int)(nchar - ra.patFw.length());
	// record amt trimmed from 3' end due to --trim3
	ra.trimmed3 = (int)(ra.patFw.trimEnd(pp_.trim3));
	
	// Make fake qualities
	assert(ra.qual.empty());
	const size_t len = ra.patFw.length();
	for(size_t i = 0; i < len; i++) {
		ra.qual.append('I');
	}
	return true;
}

#ifdef HAVE_FREAD_UNLOCKED
#define FREAD fread_unlocked
#else
#define FREAD fread
#endif

/**
 * "Light" parser. This is inside the critical section, so the key is to do
 * just enough parsing so that another function downstream (finalize()) can do
 * the rest of the parsing.  Really this function's only job is to stick every
 * for lines worth of the input file into a buffer (r.readOrigBuf).  finalize()
 * then parses the contents of r.readOrigBuf later.
 */
pair<bool, int> FastqPatternSource::nextBatchFromFile(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	EList<Read>* readbuf = batch_a ? &pt.bufa_ : &pt.bufb_;
	bool use_fread = true;  // TODO
	if(pp_.reads_per_block > 0) {
		// We're going to stick all the unparsed data into the buffer for the
		// first read in the group
		(*readbuf)[0].readOrigBuf.resize(pp_.block_bytes);
		char *buf = (*readbuf)[0].readOrigBuf.wbuf();
		int nread = pp_.reads_per_block;
		bool done = false;
		int nl = 0, i = 0;
		if(use_fread) {
			int ret = (int)FREAD(buf, 1, pp_.block_bytes, fp_);
			if(ret != pp_.block_bytes) {
				assert_lt(ret, pp_.block_bytes);
				if(ferror(fp_)) {
					cerr << "Error while parsing FASTQ input" << endl;
					throw 1;
				} else if(feof(fp_)) {
					// Count how many records got parsed
					for(; i < ret; i++) {
						if(buf[i] == '\n') {
							nl++;
						}
					}
					nread = (nl + 1) >> 2; // robust to missing newline at end
					done = true;
				} else {
					cerr << "Unexpected end of file parsing FASTQ input"
					     << endl;
					throw 1;
				}
			}
		} else {
			for(; i < pp_.block_bytes; i++) {
				// Round EOF up to 0 for now. Keeps the loop simple and won't
				// make a difference later when we count newlines or otherwise
				// parse the buffer.
				*buf++ = max(getc_wrapper(), 0);
			}
			if(feof(fp_)) {
				// Count how many records got parsed
				for(; i < pp_.block_bytes; i++) {
					if(buf[i] == '\n') {
						nl++;
					}
				}
				nread = (nl + 1) >> 2; // robust to missing newline at end
				done = true;
			} else if(ferror(fp_)) {
				cerr << "Error while parsing FASTQ input" << endl;
				throw 1;
			}
		}
		return make_pair(done, nread);
	} else {
		int c = -1;
		if(first_) {
			c = getc_wrapper();
			if (c == EOF) {
				return make_pair(true, 0);
			}
			while(c == '\r' || c == '\n') {
				c = getc_wrapper();
			}
			if(c != '@') {
				cerr << "Error: reads file does not look like a FASTQ file"
				     << endl;
				throw 1;
			}
			first_ = false;
			(*readbuf)[0].readOrigBuf.append('@');
		}

		bool done = false, aborted = false;
		size_t readi = 0;
		// Read until we run out of input or until we've filled the buffer
		while (readi < pt.max_buf_ && !done) {
			Read::TBuf& buf = (*readbuf)[readi].readOrigBuf;
			assert(readi == 0 || buf.empty());
			int newlines = 4;
			while(newlines) {
				c = getc_wrapper();
				done = c < 0;
				if(c == '\n' || (done && newlines == 1)) {
					// Saw newline, or EOF that we're
					// interpreting as final newline
					newlines--;
					c = '\n';
				} else if(done) {
					// account for newline at the end of the file
					if (newlines == 4) {
						newlines = 0;
					}
					else {
						aborted = true; // Unexpected EOF
					}
					break;
				}
				buf.append(c);
			}
			if (c > 0) {
				if (interleaved_) {
					// alternate between read buffers
					batch_a = !batch_a;
					readbuf = batch_a ? &pt.bufa_ : &pt.bufb_;
					// increment read counter after each pair gets read
					readi = batch_a ? readi+1 : readi;
				}
				else {
					readi++;
				}
			}
		}
		if(aborted) {
			readi--;
		}
		return make_pair(done, readi);
	}
}

/**
 * Finalize FASTQ parsing outside critical section.
 */
bool FastqPatternSource::parse(
	Read& r, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	// We assume the light parser has put the raw data for the separate ends
	// into separate Read objects. That doesn't have to be the case, but
	// that's how we've chosen to do it for FastqPatternSource
	assert(!cura.buf->empty());
	assert(r.empty());
	int c;
	size_t& off = ++cura.off;
	const size_t buflen = cura.buf->length();

	// Parse read name
	assert(r.name.empty());
	int spacerun = 0;
	while(true) {
		assert_lt(off, buflen);
		c = (*cura.buf)[off++];
		if(c == '\n' || c == '\r') {
			do {
				c = (*cura.buf)[off++];
			} while(c == '\n' || c == '\r');
			break;
		} else if(c == ' ') {
			spacerun++;
			continue;
		}
		if(spacerun > 0) {
			for(int i = 0; i < spacerun; i++) {
				r.name.append(' ');
			}
			spacerun = 0;
		}
		r.name.append(c);
	}
	
	// Parse sequence
	int nchar = 0;
	assert(r.patFw.empty());
	while(c != '+') {
		if(c == '.') {
			c = 'N';
		}
		if(isalpha(c)) {
			// If it's past the 5'-end trim point
			if(nchar++ >= pp_.trim5) {
				r.patFw.append(asc2dna[c]);
			}
		}
		assert_lt(off, buflen);
		c = (*cura.buf)[off++];
	}
	r.trimmed5 = (int)(nchar - r.patFw.length());
	r.trimmed3 = (int)(r.patFw.trimEnd(pp_.trim3));
	
	assert_eq('+', c);
	do {
		assert_lt(off, buflen);
		c = (*cura.buf)[off++];
	} while(c != '\n' && c != '\r');
	while(off < buflen && (c == '\n' || c == '\r')) {
		c = (*cura.buf)[off++];
	}
	
	assert(r.qual.empty());
	if(nchar > 0) {
		int nqual = 0;
		if (pp_.intQuals) {
			int cur_int = 0;
			while(c != '\t' && c != '\n' && c != '\r') {
				cur_int *= 10;
				cur_int += (int)(c - '0');
				c = (*cura.buf)[off++];
				if(c == ' ' || c == '\t' || c == '\n' || c == '\r') {
					char cadd = intToPhred33(cur_int, pp_.solexa64);
					cur_int = 0;
					assert_geq(cadd, 33);
					if(++nqual > pp_.trim5) {
						r.qual.append(cadd);
					}
				}
			}
		} else {
			c = charToPhred33(c, pp_.solexa64, pp_.phred64);
			if(nqual++ >= r.trimmed5) {
				r.qual.append(c);
			}
			while(off < buflen) {
				c = (*cura.buf)[off++];
				if (c == ' ') {
					wrongQualityFormat(r.name);
					return false;
				}
				if(c == '\r' || c == '\n' || c == '\0') {
					break;
				}
				c = charToPhred33(c, pp_.solexa64, pp_.phred64);
				if(nqual++ >= r.trimmed5) {
					r.qual.append(c);
				}
			}
			r.qual.trimEnd(r.trimmed3);
			if(r.qual.length() < r.patFw.length()) {
				tooFewQualities(r.name);
				return false;
			} else if(r.qual.length() > r.patFw.length()) {
				tooManyQualities(r.name);
				return false;
			}
		}
	}
	// Set up a default name if one hasn't been set
	if(r.name.empty()) {
		char cbuf[20];
		itoa10<TReadId>(static_cast<TReadId>(readCnt_), cbuf);
		r.name.install(cbuf);
	}
	r.parsed = true;
	if(!rb.parsed && curb.off < curb.buf->length()) {
		return parse(rb, r, curb, cura, rdid);
	}
	return true;
}

/**
 * Light-parse a batch of tabbed-format reads into given buffer.
 */
pair<bool, int> TabbedPatternSource::nextBatchFromFile(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	int c = getc_wrapper();
	while(c >= 0 && (c == '\n' || c == '\r')) {
		c = getc_wrapper();
	}
	EList<Read>& readbuf = batch_a ? pt.bufa_ : pt.bufb_;
	size_t readi = 0;
	// Read until we run out of input or until we've filled the buffer
	for(; readi < pt.max_buf_ && c >= 0; readi++) {
		readbuf[readi].readOrigBuf.clear();
		while(c >= 0 && c != '\n' && c != '\r') {
			readbuf[readi].readOrigBuf.append(c);
			c = getc_wrapper();
		}
		while(c >= 0 && (c == '\n' || c == '\r')) {
			c = getc_wrapper();
		}
	}
	return make_pair(c < 0, readi);
}

/**
 * Finalize tabbed parsing outside critical section.
 */
bool TabbedPatternSource::parse(
	Read& ra, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	// Light parser (nextBatchFromFile) puts unparsed data
	// into Read& r, even when the read is paired.
	assert(ra.empty());
	assert(rb.empty());
	assert(!cura.buf->empty()); // raw data for read/pair is here
	assert(curb.buf->empty());
	int c = '\t';
	size_t& off = cura.off;
	const size_t buflen = cura.buf->length();
	
	// Loop over the two ends
	for(int endi = 0; endi < 2 && c == '\t'; endi++) {
		Read& r = ((endi == 0) ? ra : rb);
		assert(r.name.empty());
		// Parse name if (a) this is the first end, or
		// (b) this is tab6
		if(endi < 1 || secondName_) {
			// Parse read name
			c = (*cura.buf)[off++];
			while(c != '\t' && off < buflen) {
				r.name.append(c);
				c = (*cura.buf)[off++];
			}
			assert_eq('\t', c);
			if(off >= buflen) {
				return false; // record ended prematurely
			}
		} else if(endi > 0) {
			// if this is the second end and we're parsing
			// tab5, copy name from first end
			rb.name = ra.name;
		}

		// Parse sequence
		assert(r.patFw.empty());
		c = (*cura.buf)[off++];
		int nchar = 0;
		while(c != '\t' && off < buflen) {
			if(isalpha(c)) {
				assert_in(toupper(c), "ACGTN");
				if(nchar++ >= pp_.trim5) {
					assert_neq(0, asc2dnacat[c]);
					r.patFw.append(asc2dna[c]); // ascii to int
				}
			}
			c = (*cura.buf)[off++];
		}
		assert_eq('\t', c);
		if(off >= buflen) {
			return false; // record ended prematurely
		}
		// record amt trimmed from 5' end due to --trim5
		r.trimmed5 = (int)(nchar - r.patFw.length());
		// record amt trimmed from 3' end due to --trim3
		r.trimmed3 = (int)(r.patFw.trimEnd(pp_.trim3));
		
		// Parse qualities
		assert(r.qual.empty());
		c = (*cura.buf)[off++];
		int nqual = 0;
		if (pp_.intQuals) {
			int cur_int = 0;
			while(c != '\t' && c != '\n' && c != '\r' && off < buflen) {
				cur_int *= 10;
				cur_int += (int)(c - '0');
				c = (*cura.buf)[off++];
				if(c == ' ' || c == '\t' || c == '\n' || c == '\r') {
					char cadd = intToPhred33(cur_int, pp_.solexa64);
					cur_int = 0;
					assert_geq(cadd, 33);
					if(++nqual > pp_.trim5) {
						r.qual.append(cadd);
					}
				}
			}
		} else {
			while(c != '\t' && c != '\n' && c != '\r') {
				if(c == ' ') {
					wrongQualityFormat(r.name);
					return false;
				}
				char cadd = charToPhred33(c, pp_.solexa64, pp_.phred64);
				if(++nqual > pp_.trim5) {
					r.qual.append(cadd);
				}
				if(off >= buflen) break;
				c = (*cura.buf)[off++];
			}
		}
		if(nchar > nqual) {
			tooFewQualities(r.name);
			return false;
		} else if(nqual > nchar) {
			tooManyQualities(r.name);
			return false;
		}
		r.qual.trimEnd(pp_.trim3);
		assert(c == '\t' || c == '\n' || c == '\r' || off >= buflen);
		assert_eq(r.patFw.length(), r.qual.length());
	}
	return true;
}

/**
 * Light-parse a batch of raw-format reads into given buffer.
 */
pair<bool, int> RawPatternSource::nextBatchFromFile(
	PerThreadReadBuf& pt,
	bool batch_a)
{
	int c = getc_wrapper();
	while(c >= 0 && (c == '\n' || c == '\r')) {
		c = getc_wrapper();
	}
	EList<Read>& readbuf = batch_a ? pt.bufa_ : pt.bufb_;
	size_t readi = 0;
	// Read until we run out of input or until we've filled the buffer
	for(; readi < pt.max_buf_ && c >= 0; readi++) {
		readbuf[readi].readOrigBuf.clear();
		while(c >= 0 && c != '\n' && c != '\r') {
			readbuf[readi].readOrigBuf.append(c);
			c = getc_wrapper();
		}
		while(c >= 0 && (c == '\n' || c == '\r')) {
			c = getc_wrapper();
		}
	}
	// incase a valid character is consumed between batches
	if (c >= 0 && c != '\n' && c != '\r') {
		ungetc_wrapper(c);
	}
	return make_pair(c < 0, readi);
}

/**
 * Finalize raw parsing outside critical section.
 */
bool RawPatternSource::parse(
	Read& r, Read& rb,
	ParsingCursor& cura, ParsingCursor& curb,
	TReadId rdid) const
{
	assert(r.empty());
	assert(!cura.buf->empty()); // raw data for read/pair is here
	int c = '\n';
	size_t& off = cura.off;
	const size_t buflen = cura.buf->length();

	// Parse sequence
	assert(r.patFw.empty());
	int nchar = 0;
	while(off < buflen) {
		c = (*cura.buf)[off++];
		assert(c != '\r' && c != '\n');
		if(isalpha(c)) {
			assert_in(toupper(c), "ACGTN");
			if(nchar++ >= pp_.trim5) {
				assert_neq(0, asc2dnacat[c]);
				r.patFw.append(asc2dna[c]); // ascii to int
			}
		}
	}
	assert_eq(off, buflen);
	// record amt trimmed from 5' end due to --trim5
	r.trimmed5 = (int)(nchar - r.patFw.length());
	// record amt trimmed from 3' end due to --trim3
	r.trimmed3 = (int)(r.patFw.trimEnd(pp_.trim3));
	
	// Give the name field a dummy value
	char cbuf[20];
	itoa10<TReadId>(rdid, cbuf);
	r.name.install(cbuf);
	
	// Give the base qualities dummy values
	assert(r.qual.empty());
	const size_t len = r.patFw.length();
	for(size_t i = 0; i < len; i++) {
		r.qual.append('I');
	}
	r.parsed = true;
	if(!rb.parsed && !rb.readOrigBuf.empty()) {
		return parse(rb, r, curb, cura, rdid);
	}
	return true;
}


void wrongQualityFormat(const BTString& read_name) {
	cerr << "Error: Encountered one or more spaces while parsing the quality "
		 << "string for read " << read_name << ".  If this is a FASTQ file "
		 << "with integer (non-ASCII-encoded) qualities, try re-running with "
		 << "the --integer-quals option." << endl;
	throw 1;
}

void tooFewQualities(const BTString& read_name) {
	cerr << "Error: Read " << read_name << " has more read characters than "
		 << "quality values." << endl;
	throw 1;
}

void tooManyQualities(const BTString& read_name) {
	cerr << "Error: Read " << read_name << " has more quality values than read "
		 << "characters." << endl;
	throw 1;
}

#ifdef USE_SRA
    
struct SRA_Read {
    SStringExpandable<char, 64>      name;      // read name
    SDnaStringExpandable<128, 2>     patFw;     // forward-strand sequence
    SStringExpandable<char, 128, 2>  qual;      // quality values
    
    void reset() {
        name.clear();
        patFw.clear();
        qual.clear();
    }
};
    
static const uint64_t buffer_size_per_thread = 4096;

struct SRA_Data {
    uint64_t read_pos;
    uint64_t write_pos;
    uint64_t buffer_size;
    bool     done;
    EList<pair<SRA_Read, SRA_Read> > paired_reads;
    
    ngs::ReadIterator* sra_it;
    
    SRA_Data() {
        read_pos = 0;
        write_pos = 0;
        buffer_size = buffer_size_per_thread;
        done = false;
        sra_it = NULL;
    }
    
    bool isFull() {
        assert_leq(read_pos, write_pos);
        assert_geq(read_pos + buffer_size, write_pos);
        return read_pos + buffer_size <= write_pos;
    }
    
    bool isEmpty() {
        assert_leq(read_pos, write_pos);
        assert_geq(read_pos + buffer_size, write_pos);
        return read_pos == write_pos;
    }
    
    pair<SRA_Read, SRA_Read>& getPairForRead() {
        assert(!isEmpty());
        return paired_reads[read_pos % buffer_size];
    }
    
    pair<SRA_Read, SRA_Read>& getPairForWrite() {
        assert(!isFull());
        return paired_reads[write_pos % buffer_size];
    }
    
    void advanceReadPos() {
        assert(!isEmpty());
        read_pos++;
    }
    
    void advanceWritePos() {
        assert(!isFull());
        write_pos++;
    }
};

static void SRA_IO_Worker(void *vp)
{
    SRA_Data* sra_data = (SRA_Data*)vp;
    assert(sra_data != NULL);
    ngs::ReadIterator* sra_it = sra_data->sra_it;
    assert(sra_it != NULL);
    
    while(!sra_data->done) {
        while(sra_data->isFull()) {
			SLEEP(10);
        }
        pair<SRA_Read, SRA_Read>& pair = sra_data->getPairForWrite();
        SRA_Read& ra = pair.first;
        SRA_Read& rb = pair.second;
        bool exception_thrown = false;
        try {
            if(!sra_it->nextRead() || !sra_it->nextFragment()) {
                ra.reset();
                rb.reset();
                sra_data->done = true;
                return;
            }
            
            // Read the name out of the first field
            ngs::StringRef rname = sra_it->getReadId();
            ra.name.install(rname.data(), rname.size());
            assert(!ra.name.empty());
            
            ngs::StringRef ra_seq = sra_it->getFragmentBases();
            if(gTrim5 + gTrim3 < (int)ra_seq.size()) {
                ra.patFw.installChars(ra_seq.data() + gTrim5, ra_seq.size() - gTrim5 - gTrim3);
            }
            ngs::StringRef ra_qual = sra_it->getFragmentQualities();
            if(ra_seq.size() == ra_qual.size() && gTrim5 + gTrim3 < (int)ra_qual.size()) {
                ra.qual.install(ra_qual.data() + gTrim5, ra_qual.size() - gTrim5 - gTrim3);
            } else {
                ra.qual.resize(ra.patFw.length());
                ra.qual.fill('I');
            }
            assert_eq(ra.patFw.length(), ra.qual.length());
            
            if(!sra_it->nextFragment()) {
                rb.reset();
            } else {
                // rb.name = ra.name;
                ngs::StringRef rb_seq = sra_it->getFragmentBases();
                if(gTrim5 + gTrim3 < (int)rb_seq.size()) {
                    rb.patFw.installChars(rb_seq.data() + gTrim5, rb_seq.size() - gTrim5 - gTrim3);
                }
                ngs::StringRef rb_qual = sra_it->getFragmentQualities();
                if(rb_seq.size() == rb_qual.size() && gTrim5 + gTrim3 < (int)rb_qual.size()) {
                    rb.qual.install(rb_qual.data() + gTrim5, rb_qual.size() - gTrim5 - gTrim3);
                } else {
                    rb.qual.resize(rb.patFw.length());
                    rb.qual.fill('I');
                }
                assert_eq(rb.patFw.length(), rb.qual.length());
            }
            sra_data->advanceWritePos();
        } catch(ngs::ErrorMsg & x) {
            cerr << x.toString () << endl;
            exception_thrown = true;
        } catch(exception & x) {
            cerr << x.what () << endl;
            exception_thrown = true;
        } catch(...) {
            cerr << "unknown exception\n";
            exception_thrown = true;
        }
        
        if(exception_thrown) {
            ra.reset();
            rb.reset();
            sra_data->done = true;
            cerr << "An error happened while fetching SRA reads. Please rerun HISAT2. You may want to disable the SRA cache if you didn't (see the instructions at https://github.com/ncbi/sra-tools/wiki/Toolkit-Configuration).\n";
            exit(1);
        }
    }
}

SRAPatternSource::~SRAPatternSource() {
    if(io_thread_) delete io_thread_;
    if(sra_data_) delete sra_data_;
    if(sra_it_) delete sra_it_;
    if(sra_run_) delete sra_run_;
}

/// Read another pair of patterns from a FASTA input file
bool SRAPatternSource::readPair(
                                Read& ra,
                                Read& rb,
                                TReadId& rdid,
                                TReadId& endid,
                                bool& success,
                                bool& done,
                                bool& paired)
{
    assert(sra_run_ != NULL && sra_it_ != NULL);
    success = true;
    done = false;
    while(sra_data_->isEmpty()) {
        if(sra_data_->done && sra_data_->isEmpty()) {
            ra.reset();
            rb.reset();
            success = false;
            done = true;
            return false;
        }
		SLEEP(1);
    }
    
    pair<SRA_Read, SRA_Read>& pair = sra_data_->getPairForRead();
    ra.name.install(pair.first.name.buf(), pair.first.name.length());
    ra.patFw.install(pair.first.patFw.buf(), pair.first.patFw.length());
    ra.qual.install(pair.first.qual.buf(), pair.first.qual.length());
    ra.trimmed3 = gTrim3;
    ra.trimmed5 = gTrim5;
    if(pair.second.patFw.length() > 0) {
        rb.name.install(pair.first.name.buf(), pair.first.name.length());
        rb.patFw.install(pair.second.patFw.buf(), pair.second.patFw.length());
        rb.qual.install(pair.second.qual.buf(), pair.second.qual.length());
        rb.trimmed3 = gTrim3;
        rb.trimmed5 = gTrim5;
        paired = true;
    } else {
        rb.reset();
    }
    sra_data_->advanceReadPos();
    
    rdid = endid = readCnt_;
    readCnt_++;
    
    return true;
}

void SRAPatternSource::open() {
	string version = "hisat2-";
	version += HISAT2_VERSION;
	ncbi::NGS::setAppVersionString(version.c_str());
    assert(!sra_accs_.empty());
    while(sra_acc_cur_ < sra_accs_.size()) {
        // Open read
        if(sra_it_) {
            delete sra_it_;
            sra_it_ = NULL;
        }
        if(sra_run_) {
            delete sra_run_;
            sra_run_ = NULL;
        }
        try {
            // open requested accession using SRA implementation of the API
            sra_run_ = new ngs::ReadCollection(ncbi::NGS::openReadCollection(sra_accs_[sra_acc_cur_]));
#if 0
			string run_name = sra_run_->getName();
			cerr << " ReadGroups for " << run_name << endl;

			ngs::ReadGroupIterator it = sra_run_->getReadGroups();
			do {
				ngs::Statistics s = it.getStatistics();
				cerr << "Statistics for group <" << it.getName() << ">" << endl;
				// for(string p = s.nextPath(""); p != ""; p = s.nextPath(p)){
				//    System.out.println("\t"+p+": "+s.getAsString(p));
			} while(it.nextReadGroup());
			exit(1);
#endif
            // compute window to iterate through
            size_t MAX_ROW = sra_run_->getReadCount();
            sra_it_ = new ngs::ReadIterator(sra_run_->getReadRange(1, MAX_ROW, ngs::Read::all));
            
            // create a buffer for SRA data
            sra_data_ = new SRA_Data;
            sra_data_->sra_it = sra_it_;
            sra_data_->buffer_size = nthreads_ * buffer_size_per_thread;
            sra_data_->paired_reads.resize(sra_data_->buffer_size);
            
            // create a thread for handling SRA data access
            io_thread_ = new std::thread(SRA_IO_Worker, (void*)sra_data_);
            // io_thread_->join();
        } catch(...) {
            if(!errs_[sra_acc_cur_]) {
                cerr << "Warning: Could not access \"" << sra_accs_[sra_acc_cur_].c_str() << "\" for reading; skipping..." << endl;
                errs_[sra_acc_cur_] = true;
            }
            sra_acc_cur_++;
            continue;
        }
        return;
    }
    cerr << "Error: No input SRA accessions were valid" << endl;
    exit(1);
    return;
}

#endif
