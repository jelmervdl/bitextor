#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <unordered_map>
#include <thread>
#include <memory>
#include <mutex>
#include <boost/program_options.hpp>
#include "document.h"
#include "blocking_queue.h"
#include "util/file_piece.hh"

using namespace bitextor;
using namespace std;

namespace po = boost::program_options;

mutex print_lock;

void print_score(float score, DocumentRef const &left, DocumentRef const &right)
{
	unique_lock<mutex> lock(print_lock);
	cout << fixed << setprecision(5)
	     << score
	     << '\t' << left.id
	     << '\t' << right.id
	     << '\n';
}

/**
 * Count the number of documents a term occurs in. Optinally it will only assess one in N documents (where
 * N is the skip_rate) but note that the document count it returns is for all documents, including the ones it
 * skipped. Because we are dividing by this number for TF/IDF it increments counts with skip_rate instead of
 * 1. So at the end of the day you can just do df / document_count to get the IDF.
 */
size_t read_df(util::FilePiece &fin, unordered_map<uint64_t, size_t> &df, size_t ngram_size, size_t skip_rate = 1) {
	
	// TODO: Can I make this thing multithreaded? Producer/consumer, but the
	// updating of the df map can't be concurrent, so the only profit would
	// be the base64-decode + ngram generation.

	// Number of documents actually read.
	size_t document_count = 0;
	for (StringPiece line : fin) {
		if (document_count++ % skip_rate)
			continue;

		Document document;
		ReadDocument(line, document, ngram_size);
		for (auto const &entry : document.vocab)
			df[entry.first] += skip_rate;
	}
	return document_count;
}

size_t read_document_refs(util::FilePiece &fin_tokens, unordered_map<uint64_t,size_t> df, size_t document_cnt, size_t ngram_size, vector<DocumentRef>::iterator it) {
	size_t n = 0;
	
	for (StringPiece line : fin_tokens) {
		Document buffer;
		ReadDocument(line, buffer, ngram_size);
		
		buffer.id = ++n;

		calculate_tfidf(buffer, *it++, document_cnt, df);
	}
	
	return n;
}

int score_documents(vector<DocumentRef> const &refs, unordered_map<uint64_t, size_t> const &df, size_t document_cnt, size_t ngram_size, util::FilePiece &in_tokens, float threshold, unsigned int n_threads, size_t queue_size = 16, size_t batch_size = 16, bool verbose = false)
{
	unsigned int n_line_threads = max(n_threads / 4U, 1U);
	unsigned int n_align_threads = max(n_threads - n_line_threads, 1U);

	vector<thread> line_consumers;
	line_consumers.reserve(n_line_threads);

	vector<thread> align_consumers;
	align_consumers.reserve(n_align_threads);

	struct Line {
		size_t n;
		string line;
	};

	blocking_queue<vector<unique_ptr<Line>>> line_queue(n_threads * queue_size);
	
	blocking_queue<vector<unique_ptr<DocumentRef>>> align_queue(n_threads * queue_size);

	for (unsigned int n = 0; n < n_line_threads; ++n)
		line_consumers.push_back(thread([&line_queue, &align_queue, &document_cnt, &df, &ngram_size]() {
			while (true) {
				vector<unique_ptr<Line>> in_batch(line_queue.pop());

				if (in_batch.empty())
					break;

				vector<unique_ptr<DocumentRef>> out_batch;
				out_batch.reserve(in_batch.size());

				for (auto &line : in_batch) {
					Document buffer;

					ReadDocument(line->line, buffer, ngram_size);

					unique_ptr<DocumentRef> buffer_ref(new DocumentRef());
					calculate_tfidf(buffer, *buffer_ref, document_cnt, df);
					out_batch.push_back(move(buffer_ref));
				}

				align_queue.push(move(out_batch));
			}
		}));
	
	for (unsigned int n = 0; n < n_align_threads; ++n)
		align_consumers.push_back(thread([&align_queue, &refs, &document_cnt, &df, &threshold]() {
			while (true) {
				vector<unique_ptr<DocumentRef>> batch(align_queue.pop());

				if (batch.empty())
					break;

				for (unique_ptr<DocumentRef> const &buffer : batch) {
					for (auto const &document_ref : refs) {
						float score = calculate_alignment(document_ref, *buffer);

						// Document not a match? Skip to the next.
						if (score < threshold)
							continue;

						print_score(score, document_ref, *buffer);
					}
				}
			}
		}));
	
	vector<unique_ptr<Line>> batch;
	batch.reserve(batch_size);
	
	for (size_t n = 1; true; ++n) {
		StringPiece line;
		if (!in_tokens.ReadLineOrEOF(line))
			break;

		unique_ptr<Line> buffer(new Line{
			.n = n,
			.line = string(line.data(), line.size()) // TODO: I don't want to do this but StringPiece's backing will move on
		});

		// Push this document to the batch
		batch.push_back(std::move(buffer));

		// If the batch is full, push it to the queue
		if (batch.size() == batch_size) {
			line_queue.push(std::move(batch));
			batch.clear();
		}
	}

	// Push any trailing files
	if (!batch.empty())
		line_queue.push(std::move(batch));

	// Tell all workers there is nothing left and wait for them to stop.
	for (size_t i = 0; i < n_line_threads; ++i)
		line_queue.push({});

	// Wait for all line workers to finish.
	for (auto &line_consumer : line_consumers)
		line_consumer.join();

	if (verbose)
		cerr << "Line consumers are all done" << endl;

	// Now we know that the line workers won't push any more content, we can
	// send the stop signal to the align workers
	for (size_t i = 0; i < n_align_threads; ++i)
		align_queue.push({});

	for (auto &align_consumer : align_consumers)
		align_consumer.join();
	
	if (verbose)
		cerr << "Read queue performance:\n"
		     << "  underflow: " << line_queue.performance().underflow << '\n'
		     << "  overflow: " << line_queue.performance().overflow << '\n'
			 << "Align queue performance:\n"
		     << "  underflow: " << align_queue.performance().underflow << '\n'
		     << "  overflow: " << align_queue.performance().overflow << endl;
	
	return 0;
}

int main(int argc, char *argv[]) {
	unsigned int n_threads = thread::hardware_concurrency() - 1;
	
	float threshold = 0.1;
	
	size_t df_sample_rate = 1;
	
	size_t ngram_size = 2;

	size_t batch_size = 64;

	size_t queue_size = 16;
	
	bool verbose = false;
	
	po::positional_options_description arg_desc;
	arg_desc.add("translated-tokens", 1);
	arg_desc.add("english-tokens", 1);
	
	po::options_description generic_desc("Additional options");
	generic_desc.add_options()
		("help", "produce help message")
		("df-sample-rate", po::value<size_t>(&df_sample_rate), "set sample rate to every n-th document (default: 1)")
	    ("ngram_size,n", po::value<size_t>(&ngram_size), "ngram size (default: 2)")
		("threshold", po::value<float>(&threshold), "set score threshold (default: 0.1)")
		("verbose,v", po::value<bool>(&verbose), "show additional output (default: nope)");

	po::options_description perf_desc("Performance options");
	perf_desc.add_options()
		("jobs,j", po::value<unsigned int>(&n_threads), "set number of threads (default: all cpus)")
		("batch-size", po::value<size_t>(&batch_size), "number of documents per batch (default 64)")
		("queue-size", po::value<size_t>(&queue_size), "number of batches that can be queued per worker (default: 16)");

	po::options_description hidden_desc("Hidden options");
	hidden_desc.add_options()
		("translated-tokens", po::value<string>(), "set input filename")
		("english-tokens", po::value<string>(), "set input filename");

	po::options_description opt_desc;
	opt_desc.add(generic_desc).add(perf_desc).add(hidden_desc);
	
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
		     << generic_desc << '\n' << perf_desc << endl;
		return 1;
	}
	
	// Calculate the document frequency for terms.
	// We'll use in_document_cnt later to reserve some space for the documents
	// we want to keep in memory.
	unordered_map<uint64_t,size_t> df;
	
	size_t in_document_cnt, en_document_cnt;

	{
		util::FilePiece in_tokens(vm["translated-tokens"].as<std::string>().c_str());
		in_document_cnt = read_df(in_tokens, df, ngram_size, df_sample_rate);
	}

	{
		util::FilePiece en_tokens(vm["english-tokens"].as<std::string>().c_str());
		en_document_cnt = read_df(en_tokens, df, ngram_size, df_sample_rate);
	}

	size_t document_cnt = in_document_cnt + en_document_cnt;

	if (verbose)
		cerr << "Calculated DF from " << document_cnt / df_sample_rate << " documents" << endl;

	// Read translated documents & calculate TF/DF over the documents we have in memory
	std::vector<DocumentRef> refs(in_document_cnt);

	{
		util::FilePiece in_tokens(vm["translated-tokens"].as<std::string>().c_str());
		read_document_refs(in_tokens, df, document_cnt, ngram_size, refs.begin());
	}

	if (verbose)
		cerr << "Read " << refs.size() << " documents into memory" << endl;

	// Start reading the other set of documents we match against
	util::FilePiece en_tokens(vm["english-tokens"].as<std::string>().c_str());
	return score_documents(refs, df, document_cnt, ngram_size, en_tokens, threshold, n_threads, queue_size, batch_size, verbose);
}
