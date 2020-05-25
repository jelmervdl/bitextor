#include <iostream>
#include <iomanip>
#include <fstream>
#include <map>
#include <unordered_map>
#include <thread>
#include <memory>
#include <mutex>
#include <vector>
#include <cmath>
#include <boost/program_options.hpp>
#include "util/file_piece.hh"
#include "src/document.h"
#include "src/blocking_queue.h"
#include "src/broadcast_queue.h"


using namespace bitextor;
using namespace std;

namespace po = boost::program_options;

struct Line {
	string str;
	size_t n;
};

struct DocumentPair {
	float score;
	size_t in_idx;
	size_t en_idx;
};

/**
 * Utility to start N threads executing fun. Returns a vector with those thread objects.
 */
template<class Function, class... Args> vector<thread> start(unsigned int n_threads, Function const &fun, Args const &... args) {
	vector<thread> threads;
	threads.reserve(n_threads);
	for (unsigned int n = 0; n < n_threads; ++n)
		threads.emplace_back(fun, args...);
	return threads;
}

/**
 * Utility to stop & join threads. Needs access to the queue to supply it null pointers after which it waits
 * for the workers to stop & join.
 */
template <class T> void stop(blocking_queue<T> &queue, vector<thread> &workers) {
	for (size_t i = 0; i < workers.size(); ++i)
		queue.push(nullptr);

	for (auto &worker : workers)
		worker.join();
}

template <class T> void stop(BroadcastQueue<T> &queue, vector<thread> &workers) {
	queue.push(nullptr);

	for (auto &worker : workers)
		worker.join();
}

ostream &operator<<(ostream &out, queue_performance const &performance) {
	return out << "  underflow: " << performance.underflow << '\n'
	           << "   overflow: " << performance.overflow << '\n';
}

void print_score(float score, size_t left_id, size_t right_id)
{
	cout << fixed << setprecision(5)
	     << score
	     << '\t' << left_id
	     << '\t' << right_id
	     << '\n';
}

template <class T> size_t queue_lines(util::FilePiece &fin, T &queue, size_t skip_rate = 1)
{
	size_t document_count = 0;
	for (StringPiece line : fin) {
		if (document_count++ % skip_rate)
			continue;

		queue.push(typename T::value_type(new Line{
			.str = string(line.data(), line.size()),
			.n = document_count
		}));
	}

	return document_count;
}

template <class T> size_t queue_lines(std::string const &path, T &queue, size_t skip_rate = 1)
{
	util::FilePiece fin(path.c_str());
	return queue_lines(fin, queue, skip_rate);
}

int main(int argc, char *argv[])
{
	unsigned int n_threads = thread::hardware_concurrency();
	
	float threshold = 0.1;
	
	size_t df_sample_rate = 1;
	
	size_t ngram_size = 2;

	size_t min_ngram_cnt = 2;

	size_t max_ngram_cnt = 1000;

	bool verbose = false;

	bool print_all = false;
	
	po::positional_options_description arg_desc;
	arg_desc.add("translated-tokens", 1);
	arg_desc.add("english-tokens", 1);
	
	po::options_description generic_desc("Additional options");
	generic_desc.add_options()
		("help", "produce help message")
		("df-sample-rate", po::value<size_t>(&df_sample_rate), "set sample rate to every n-th document (default: 1)")
		("ngram_size,n", po::value<size_t>(&ngram_size), "ngram size (default: 2)")
		("jobs,j", po::value<unsigned int>(&n_threads), "set number of threads (default: all)")
		("threshold", po::value<float>(&threshold), "set score threshold (default: 0.1)")
		("min_count", po::value<size_t>(&min_ngram_cnt), "minimal number of documents an ngram can appear in to be included in DF (default: 2)")
		("max_count", po::value<size_t>(&max_ngram_cnt), "maximum number of documents for ngram to to appear in (default: 1000)")
		("all", po::bool_switch(&print_all), "print all scores, not only the best pairs")
		("verbose,v", po::bool_switch(&verbose), "show additional output");
	
	po::options_description hidden_desc("Hidden options");
	hidden_desc.add_options()
		("translated-tokens", po::value<string>(), "set input filename")
		("english-tokens", po::value<string>(), "set input filename");

	po::options_description opt_desc;
	opt_desc.add(generic_desc).add(hidden_desc);

	po::variables_map vm;
	
	try {
		po::store(po::command_line_parser(argc, argv).options(opt_desc).positional(arg_desc).run(), vm);
		po::notify(vm);
	} catch (const po::error &exception) {
		cerr << exception.what() << endl;
		return 1;
	}
	
	if (vm.count("help") || !vm.count("translated-tokens") || !vm.count("english-tokens")) {
		cout << "Usage: " << argv[0]
		     << " TRANSLATED-TOKENS ENGLISH-TOKENS\n\n"
		     << generic_desc << endl;
		return 1;
	}

	// Calculate the document frequency for terms. Starts a couple of threads
	// that parse documents and keep a local hash table for counting. At the
	// end these tables are merged into df.
	unordered_map<NGram,size_t> df;
	size_t in_document_cnt, en_document_cnt, document_cnt;

	{
		mutex df_mutex;
		blocking_queue<unique_ptr<Line>> queue(n_threads * 128);
		vector<thread> workers(start(n_threads, [&queue, &df, &df_mutex, &ngram_size, &df_sample_rate]() {
			unordered_map<NGram, size_t> local_df;

			while (true) {
				unique_ptr<Line> line(queue.pop());

				if (!line)
					break;

				Document document;
				ReadDocument(line->str, document, ngram_size);
				for (auto const &entry : document.vocab)
					local_df[entry.first] += 1; // Count once every document
			}

			// Merge the local DF into the global one. Multiply by df_sample_rate
			// to compensate for reading only nth part of the whole collection.
			{
				unique_lock<mutex> lock(df_mutex);
				for (auto const &entry : local_df)
					df[entry.first] += entry.second * df_sample_rate;
			}
		}));

		// We'll use in_document_cnt later to reserve some space for the documents
		// we want to keep in memory. (Also this line is the whole reason the
		// worker management + reading isn't wrapped in a single function: I
		// want to re-use the same workers for two files.)
		en_document_cnt = queue_lines(vm["english-tokens"].as<std::string>(), queue, df_sample_rate);
		in_document_cnt = queue_lines(vm["translated-tokens"].as<std::string>(), queue, df_sample_rate);
		document_cnt = in_document_cnt + en_document_cnt;

		stop(queue, workers);

		if (verbose)
			cerr << "Calculated DF from " << document_cnt / df_sample_rate << " documents" << endl;

		if (verbose)
			cerr << "DF queue performance:\n" << queue.performance();
	}

	// Prune the DF table, similar to what the Python implementation does. Note
	// that these counts are linked to sample-rate already, so if you have a
	// sample rate of higher than 1, your min_ngram_count should also be a
	// multiple of sample rate + 1.
	{
		unordered_map<NGram, size_t> pruned_df;
		for (auto const &entry : df) {
			if (entry.second < min_ngram_cnt)
				continue;

			if (entry.second > max_ngram_cnt)
				continue;

			pruned_df[entry.first] = entry.second;
		}

		if (verbose)
			cerr << "Pruned " << df.size() - pruned_df.size() << " (" << 100.0 - 100.0 * pruned_df.size() / df.size() << "%) entries from DF" << endl;

		swap(df, pruned_df);
	}

	// Function used to report the score. Implementation depends on whether
	// we are doing print_all or not. Mutex is necessary for both cases,
	// either for writing to top_scores or for printing to stdout.
	function<void (float, DocumentRef const &, DocumentRef const &)> mark_score;
	mutex mark_score_mutex;

	// Scores for all pairs (that meet the threshold). Only used with 
	vector<DocumentPair> scored_pairs;

	if (!print_all) {
		mark_score = [&scored_pairs, &mark_score_mutex] (float score, DocumentRef const &in_ref, DocumentRef const &en_ref) {
			unique_lock<mutex> lock(mark_score_mutex);
			scored_pairs.push_back({score, in_ref.id, en_ref.id});
		};
	} else {
		mark_score = [&mark_score_mutex](float score, DocumentRef const &in_ref, DocumentRef const &en_ref) {
			unique_lock<mutex> lock(mark_score_mutex);
			print_score(score, in_ref.id, en_ref.id);
		};
	}

	// Read translated documents & pre-calculate TF/DF for each of these documents
	blocking_queue<unique_ptr<Line>> read_queue(n_threads * 128);

	blocking_queue<unique_ptr<Line>> load_queue(n_threads * 128);

	BroadcastQueue<shared_ptr<vector<DocumentRef>>> score_queue;
		
	vector<thread> load_workers(start(1, [&load_queue, &score_queue, &document_cnt, &ngram_size, &df]() {
		shared_ptr<vector<DocumentRef>> buffer(new vector<DocumentRef>());

		while (true) {
			unique_ptr<Line> line(load_queue.pop());

			if (!line)
				break;

			Document doc{.id = line->n, .vocab = {}};
			ReadDocument(line->str, doc, ngram_size);

			buffer->emplace_back();
			calculate_tfidf(doc, buffer->back(), document_cnt, df);

			if (buffer->size() == 256) {
				score_queue.push(buffer);
				buffer.reset(new vector<DocumentRef>());
			}
		}

		if (buffer->size())
			score_queue.push(buffer);
	}));

	vector<thread> workers(start(n_threads, [&read_queue, &df, &document_cnt, &ngram_size, &threshold, &mark_score](BroadcastQueue<shared_ptr<vector<DocumentRef>>>::listener score_queue_it) {
		vector<DocumentRef> refs;

		// Load documents we compare to later on
		while (true) {
			unique_ptr<Line> line(read_queue.pop());

			if (!line)
				break;

			Document doc{.id = line->n, .vocab = {}};
			ReadDocument(line->str, doc, ngram_size);

			// Note that each worker writes to a different line in the refs
			// vector and the vector has been initialized with enough lines
			// so there should be no concurrency issue.
			// DF is accessed read-only. N starts counting at 1.
			refs.emplace_back(); // Only C++17 has an emplace_back() that returns a reference :/
			calculate_tfidf(doc, refs.back(), document_cnt, df);
		}

		size_t doc_cnt = 0;

		// Score English documents
		while (true) {
			shared_ptr<vector<DocumentRef>> doc_refs(score_queue_it.pop());

			// Empty pointer is poison
			if (!doc_refs)
				break;
			
			for (auto const &ref : refs) {
				for (auto const &doc_ref : *doc_refs) {
					float score = calculate_alignment(ref, doc_ref);

					// Document not a match? Skip to the next.
					if (score < threshold)
						continue;

					mark_score(score, ref, doc_ref);
				}
			}

			doc_cnt += doc_refs->size();

			cerr << "Thread " << this_thread::get_id() << " processed " << doc_cnt << " docs" << endl;
		}
	}, score_queue.listen()));

	queue_lines(vm["translated-tokens"].as<std::string>(), read_queue);

	for (size_t n = 0; n < n_threads; ++n)
		read_queue.push(nullptr);

	if (verbose)
		cerr << "Done reading foreign documents into memory: " << read_queue.performance();

	queue_lines(vm["english-tokens"].as<std::string>(), load_queue);

	if (verbose)
		cerr << "Done reading english documents into the queue" << endl;

	stop(load_queue, load_workers);

	if (verbose)
		cerr << "Load workers have finished" << endl;

	stop(score_queue, workers);

	if (verbose)
		cerr << "Score workers have finished" << endl;

	if (!print_all) {
		// Sort scores, best on top. Also sort on other properties to make
		// it a consistent order, c.f. not depending on the processing order.
		sort(scored_pairs.begin(), scored_pairs.end(), [](DocumentPair const &a, DocumentPair const &b) {
			if (a.score != b.score)
				return a.score > b.score;

			if (a.in_idx != b.in_idx)
				return a.in_idx > b.in_idx;

			return a.en_idx > b.en_idx;
		});

		// Keep track of which documents have already been assigned
		vector<bool> in_seen(in_document_cnt);
		vector<bool> en_seen(en_document_cnt);

		// Also keep a quick tally on whether we've printed scores for
		// every document, so we don't keep searching while in_seen or
		// en_seen is completely filled.
		size_t cnt = 0;
		size_t document_cnt = min(in_document_cnt, en_document_cnt);

		// For each pair (with score, sorted from good to bad)
		for (DocumentPair const &pair : scored_pairs) {
			// If either of the documents has already been printed, skip it.
			if (in_seen[pair.in_idx - 1] || en_seen[pair.en_idx - 1])
				continue;

			print_score(pair.score, pair.in_idx, pair.en_idx);
			in_seen[pair.in_idx - 1] = true;
			en_seen[pair.en_idx - 1] = true;

			if (++cnt == document_cnt)
				break;
		}
	}

	return 0;
}
