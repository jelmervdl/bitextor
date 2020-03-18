# Document aligner & friends

Kitchen-sink example:
```
gzip -cd is/sentences_en.gz | b64filter tokenize | gzip -c is/tokenized_en.gz

< en/sentences.gz | docenc -d | tokenize | docenc | gzip -c en/tokenised.gz

docalign is/tokenised_en.gz en/tokenised.gz \
    | docjoin -li -ri -l is/sentences.gz -r en/sentences.gz -l is/sentences_en.gz \
    | parallel --gnu --pipe bluealign-cpp \
    | gzip -c \
    > aligned.gz
```

# Tools
Besides docalign and it's little companion tool docjoin there are a couple more tools in here to work with base64-encoded documents.

- **docalign**: Give it two (optionally compressed) files with base64-encoded tokenised documents, and it will tell you how well each of the documents in the two files match up. Output is scores + document indices. To be used with docjoin.
- **docjoin**: Take two sets of input files, and merge their lines into multiple columns based on index pairs provided to stdin.
- **docenc**: Encode (or decode) sentences into documents. Sentences are grouped in documents by separating batches of sentences by a document marker. This can be either an empty line (i.e. \n, like HTTP) or \0 (when using the -0 flag). Reminder for myself: encode (the default) combines sentences into documents. Decode explodes documents into sentences. Sentences are always split by newlines, documents either by blank lines or null bytes.
- **b64filter**: Wraps a program and passes all lines from all documents through. Think of `< sentences.gz b64filter cat` as `< sentences.gz docenc -d | cat | docenc`. Difference is that it doesn't pass any document separators to the delegate program, it just counts how many lines go in and gathers that many lines at the output side of it. C++ reimplementation of [b64filter](https://github.com/paracrawl/b64filter)

# docalign
```
Usage: docalign TRANSLATED-TOKENS ENGLISH-TOKENS

Additional options:
  --help                  produce help message
  --df-sample-rate arg    set sample rate to every n-th document (default: 1)
  -n [ --ngram_size ] arg ngram size (default: 2)
  -j [ --jobs ] arg       set number of threads (default: all)
  --threshold arg         set score threshold (default: 0.1)
  --min_count arg         minimal number of documents an ngram can appear in to
                          be included in DF (default: 2)
  --max_count arg         maximum number of documents for ngram to to appear in
                          (default: 1000)
  --best arg              only output the best match for each document
                          (default: on)
  -v [ --verbose ]        show additional output
```

It is advisable to pass in --df-sample-rate to reduce start-up time and memory
usage for the document frequency part of TFIDF. 1 indicates that every document
will be read while 4 would mean that one of every four documents will be added
to the DF.

## Input
Two files (gzip-compressed or plain text) with on each line a single base64-
encoded list of tokens (separated by whitespace).

## Output
For each alignment score that is greater or equal to the threshold it prints the
score, and the indexes (starting with 1) of the documents in TRANSLATED-TOKENS
and ENGLISH-TOKENS, separated by tabs to STDOUT.

# docjoin
```
Usage: bin/docjoin [ -l filename | -r filename | -li | -ri ] ...
Input via stdin: <float> "\t" <left index> "\t" <right index> "\n"

This program joins rows from two sets of files into tab-separated output.

Column options:
  -l    Use left index for the following files
  -r    Use right index for the following files
  -li   Print the left index
  -ri   Print the right index

The order of the columns in the output is the same as the order of the
arguments given to the program.
```

# docenc
```
Usage: docenc [ -d ] [ -0 ] [ index ... ]
```

Better served by an example:
```
docenc -d < plain_text.gz \
	| tr '[:lower:]' '[:upper:]' \
	| bin/docenc \
	| gzip -c \
	> loud_text.gz
```

# b64filter
```
Usage: b64filter command
```

Again, an example shows much more:
```
gzip -cd plain_text.gz \
	| b64filter tr '[:lower:]' '[:upper:]' \
	| gzip -c \
	> very_loud_text.gz
```

# Building on CSD3
```
module load gcc
module load cmake
```

Compile boost (available boost seems to not link properly).
```
wget https://dl.bintray.com/boostorg/release/1.72.0/source/boost_1_72_0.tar.gz
tar -xzf boost_1_72_0.tar.gz
cd boost_1_72_0.tar.gz
./bootstrap.sh --prefix=$HOME/.local
./b2 install
```

Compile dalign with the up-to-date Boost
```
cd bitextor/document-aligner
mkdir build
cd build
cmake -D Boost_DIR=$HOME/.local/lib/cmake/Boost-1.72.0 ..
make -j4
```

Now you should have a `bin/docalign` and others in your build directory. Note that it is
linked to your custom Boost which makes it a bit less portable.
