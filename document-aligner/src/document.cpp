#include "document.h"
#include "base64.h"
#include "ngram.h"
#include <sstream>
#include <iostream>
#include <cmath>

using namespace std;

namespace bitextor {

/**
 * Reads a single line of base64 encoded document into a Document.
 */
void ReadDocument(const StringPiece &encoded, Document &document, size_t ngram_size)
{
	std::string body;
	base64_decode(encoded, body);
	for (NGramIter ngram_it(body, ngram_size); ngram_it; ++ngram_it)
		document.vocab[*ngram_it] += 1;
}


inline float tfidf(size_t tf, size_t dc, size_t df) {
	// Note: Matches tf_smooth setting 14 (2 for TF and 2 for IDF) of the python implementation
	return logf(tf + 1) * logf(dc / (1.0f + df));
}
	
/**
 * Calculate TF/DF based on how often an ngram occurs in this document and how often it occurs at least once
 * across all documents. Only terms that are seen in this document and in the document frequency table are
 * counted. All other terms are ignored.
*/
void calculate_tfidf(Document const &document, DocumentRef &document_ref, size_t document_count, unordered_map<NGram, size_t> const &df) {
	document_ref.id = document.id;

	// document_ref.wordvec.clear();
	document_ref.wordvec.reserve(document.vocab.size());
	
	float total_tfidf_l2 = 0;

	for (auto const &entry : document.vocab) {
		// How often does the term occur in the whole dataset?
		auto it = df.find(entry.first);

		// Match Python implementation behaviour
		if (it == df.end())
			continue;
	
		// If we can't find it (e.g. because we didn't really read the whole
		// dataset) we just assume one: just this document.
		size_t term_df = it == df.end() ? 1 : it->second;
	
		float document_tfidf = tfidf(entry.second, document_count, term_df);
		
		// Keep track of the squared sum of all values for L2 normalisation
		total_tfidf_l2 += document_tfidf * document_tfidf;
		
		// Insert the entry in our sparse vector. This is also effectively
		// insertion sort, but it's not a bottleneck.
		document_ref.wordvec.insert(entry.first) = document_tfidf;
	}
	
	// Normalize
	total_tfidf_l2 = sqrt(total_tfidf_l2);
	document_ref.wordvec /= total_tfidf_l2;
}

template <class It, class T> It lower_bound_interp(It it, It end, T const &val) {
	// Assert that the value we search is inside our range
	if (*prev(end) < val)
		return end;

	while (*it < val) {
		// Assert it is smaller than our current end otherwise it is out of range or we overshot it.
		// assert(*it < *prev(end));

		size_t step_size = (*prev(end) - *it) / distance(it, end);
		size_t dist = (val - *it) * step_size;
		
		// Note: dist can be 0. That's fine, we still move it or end
		It pivot = it + dist;

		// Assert we're still in our range and didn't do anything funny
		// assert(pivot < end);
		
		if (*pivot < val)
			it = next(pivot);
		else if (val < *pivot)
			end = prev(pivot);
		else
			it = pivot;
	}

	return it;
}

/**
 * Dot product of two documents (of their ngram frequency really)
 */
float calculate_alignment(DocumentRef const &left, DocumentRef const &right) {
	return left.wordvec.dot(right.wordvec);
}

} // namespace bitextor
